#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/websocket.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace ws = beast::websocket;
using tcp = asio::ip::tcp;

// ===== Protocol =====
//
// format:
//   0 = UINT16_RAW_UNKNOWN
//   1 = UINT16_TLINEAR_KELVIN
//
// scale:
//   format=1 のとき value/scale [K]
//   format=0 のとき意味なし（0を推奨）
//
static constexpr uint16_t kFormatUint16RawUnknown = 0;
static constexpr uint16_t kFormatUint16TLinearKelvin = 1;
static constexpr uint16_t kHeaderVersion = 1;
static constexpr uint16_t kDefaultScaleKelvin = 100;

#pragma pack(push, 1)
struct FrameHeader {
  char magic[4];         // "L3R1"
  uint16_t version;      // 1
  uint16_t header_bytes; // 32
  uint16_t width;        // e.g. 160
  uint16_t height;       // e.g. 120
  uint16_t format;       // 0=RAW_UNKNOWN, 1=UINT16_TLINEAR_KELVIN
  uint16_t scale;        // value/scale Kelvin if format==1, else 0
  uint16_t reserved;     // 0
  uint64_t timestamp_us; // monotonic
  uint32_t frame_id;     // incrementing
  uint16_t reserved2;    // padding to keep header 32 bytes
};
static_assert(sizeof(FrameHeader) == 32, "FrameHeader must be 32 bytes");
#pragma pack(pop)

struct Frame {
  FrameHeader hdr{};
  std::vector<uint16_t> pixels;
};

static uint64_t now_us() {
  using namespace std::chrono;
  auto t = steady_clock::now().time_since_epoch();
  return static_cast<uint64_t>(duration_cast<microseconds>(t).count());
}

static void fill_frame_header(FrameHeader &hdr, uint16_t format, uint16_t scale,
                              uint16_t width, uint16_t height,
                              uint32_t frame_id) {
  std::memcpy(hdr.magic, "L3R1", 4);
  hdr.version = kHeaderVersion;
  hdr.header_bytes = sizeof(FrameHeader);
  hdr.width = width;
  hdr.height = height;
  hdr.format = format;
  hdr.scale = scale;
  hdr.reserved = 0;
  hdr.timestamp_us = now_us();
  hdr.frame_id = frame_id;
  hdr.reserved2 = 0;
}

// ===== Logger =====

enum class LogLevel { INFO, WARN, ERROR };
static std::mutex g_log_mu;

static void log(LogLevel lv, const std::string &msg) {
  using namespace std::chrono;
  const auto tp = system_clock::now();
  const auto t = system_clock::to_time_t(tp);
  const auto us =
      duration_cast<microseconds>(tp.time_since_epoch()) % seconds(1);

  char buf[64];
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif

  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d.%06d",
                tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                tm.tm_min, tm.tm_sec, static_cast<int>(us.count()));

  const char *lvl = (lv == LogLevel::INFO)   ? "INFO"
                    : (lv == LogLevel::WARN) ? "WARN"
                                             : "ERROR";

  std::lock_guard<std::mutex> lk(g_log_mu);
  std::clog << "[" << buf << "] [" << lvl << "] " << msg << "\n";
}

// ===== Common helpers =====

static std::string yesno(bool v) { return v ? "yes" : "no"; }

static bool is_expected_lepton_size(uint16_t w, uint16_t h) {
  return (w == 160 && h == 120) || (w == 80 && h == 60);
}

static bool is_probe_candidate_size(uint16_t w, uint16_t h) {
  // 160x122 is commonly advertised when telemetry rows are enabled.
  return is_expected_lepton_size(w, h) || (w == 160 && h == 122);
}

static void log_center_stats(const std::vector<uint16_t> &p, uint16_t w,
                             uint16_t h) {
  if (p.empty() || w == 0 || h == 0)
    return;

  const int x0 = static_cast<int>(w) / 4;
  const int x1 = static_cast<int>(w) * 3 / 4;
  const int y0 = static_cast<int>(h) / 4;
  const int y1 = static_cast<int>(h) * 3 / 4;

  uint16_t mn = 65535;
  uint16_t mx = 0;
  uint64_t sum = 0;
  size_t cnt = 0;

  for (int y = y0; y < y1; ++y) {
    for (int x = x0; x < x1; ++x) {
      const uint16_t v = p[static_cast<size_t>(y) * w + x];
      mn = std::min(mn, v);
      mx = std::max(mx, v);
      sum += v;
      ++cnt;
    }
  }

  const double mean =
      cnt ? static_cast<double>(sum) / static_cast<double>(cnt) : 0.0;

  char b[160];
  std::snprintf(b, sizeof(b), "center stats: min=%u max=%u mean=%.2f", mn, mx,
                mean);
  log(LogLevel::INFO, b);
}

// ===== Source interface =====

class IFrameSource {
public:
  virtual ~IFrameSource() = default;
  virtual bool start() = 0;
  virtual void stop() = 0;
  virtual std::optional<Frame> latest() = 0;
  virtual uint16_t width() const = 0;
  virtual uint16_t height() const = 0;
};

// ===== Dummy source =====

class DummySource : public IFrameSource {
public:
  DummySource(uint16_t w = 160, uint16_t h = 120, double fps = 9.0,
              bool assume_tlinear = true, uint16_t scale = kDefaultScaleKelvin)
      : w_(w), h_(h), fps_(fps), assume_tlinear_(assume_tlinear),
        scale_(scale) {}

  bool start() override {
    if (running_.exchange(true))
      return true;
    th_ = std::thread([this] { run(); });
    return true;
  }

  void stop() override {
    if (!running_.exchange(false))
      return;
    if (th_.joinable())
      th_.join();
  }

  std::optional<Frame> latest() override {
    std::lock_guard<std::mutex> lk(mu_);
    return latest_;
  }

  uint16_t width() const override { return w_; }
  uint16_t height() const override { return h_; }

private:
  void run() {
    uint32_t fid = 0;
    const auto period = std::chrono::duration<double>(1.0 / fps_);
    auto next = std::chrono::steady_clock::now();

    // 背景 295.00 K, ホットスポット 310.00 K
    const uint16_t bgK = 29500;
    const uint16_t hotK = 31000;

    while (running_.load()) {
      next += std::chrono::duration_cast<std::chrono::steady_clock::duration>(
          period);

      Frame f;
      fill_frame_header(f.hdr,
                        assume_tlinear_ ? kFormatUint16TLinearKelvin
                                        : kFormatUint16RawUnknown,
                        assume_tlinear_ ? scale_ : 0, w_, h_, fid++);

      f.pixels.assign(static_cast<size_t>(w_) * h_, bgK);

      const int cx = static_cast<int>(fid % w_);
      const int cy = static_cast<int>((fid / 2) % h_);
      const double r = 12.0;
      const double sigma = r * 0.6;
      const double inv2sigma2 = 1.0 / (2.0 * sigma * sigma);
      const double amp = static_cast<double>(hotK - bgK);

      for (int y = 0; y < static_cast<int>(h_); ++y) {
        for (int x = 0; x < static_cast<int>(w_); ++x) {
          const double dx = static_cast<double>(x - cx);
          const double dy = static_cast<double>(y - cy);
          const double d2 = dx * dx + dy * dy;
          const double gw = std::exp(-d2 * inv2sigma2);
          const double val = static_cast<double>(bgK) + amp * gw;
          const uint16_t k =
              static_cast<uint16_t>(std::lround(std::clamp(val, 0.0, 65535.0)));
          f.pixels[static_cast<size_t>(y) * w_ + x] = k;
        }
      }

      {
        std::lock_guard<std::mutex> lk(mu_);
        latest_ = std::move(f);
      }

      std::this_thread::sleep_until(next);
    }
  }

  uint16_t w_;
  uint16_t h_;
  double fps_;
  bool assume_tlinear_;
  uint16_t scale_;

  std::atomic<bool> running_{false};
  std::thread th_;
  std::mutex mu_;
  std::optional<Frame> latest_;
};

#ifdef USE_LIBUVC
#include <libuvc/libuvc.h>

// libuvc の frame_format をログに出すための簡易変換
static const char *uvc_format_name(uvc_frame_format fmt) {
  switch (fmt) {
  case UVC_FRAME_FORMAT_UNKNOWN:
    return "UNKNOWN";
  case UVC_FRAME_FORMAT_UNCOMPRESSED:
    return "UNCOMPRESSED";
  case UVC_FRAME_FORMAT_COMPRESSED:
    return "COMPRESSED";
#ifdef UVC_COLOR_FORMAT_YUYV
  case UVC_COLOR_FORMAT_YUYV:
    return "YUYV";
#endif
#ifdef UVC_COLOR_FORMAT_UYVY
  case UVC_COLOR_FORMAT_UYVY:
    return "UYVY";
#endif
#ifdef UVC_COLOR_FORMAT_GRAY8
  case UVC_COLOR_FORMAT_GRAY8:
    return "GRAY8";
#endif
#ifdef UVC_FRAME_FORMAT_GRAY16
  case UVC_FRAME_FORMAT_GRAY16:
    return "GRAY16";
#endif
#ifdef UVC_COLOR_FORMAT_Y16
  case UVC_COLOR_FORMAT_Y16:
    return "Y16";
#endif
#ifdef UVC_COLOR_FORMAT_MJPEG
  case UVC_COLOR_FORMAT_MJPEG:
    return "MJPEG";
#endif
  default:
    if (static_cast<int>(fmt) == 10)
      return "GRAY16(id=10)";
    if (static_cast<int>(fmt) == 13)
      return "Y16(id=13)";
    return "OTHER";
  }
}

static std::vector<uvc_frame_format> y16_candidate_formats() {
  std::vector<uvc_frame_format> out;
  auto add_unique = [&](uvc_frame_format f) {
    if (std::find(out.begin(), out.end(), f) == out.end())
      out.push_back(f);
  };

#ifdef UVC_FRAME_FORMAT_GRAY16
  add_unique(UVC_FRAME_FORMAT_GRAY16);
#endif
#ifdef UVC_COLOR_FORMAT_Y16
  add_unique(UVC_COLOR_FORMAT_Y16);
#endif
  // libuvcのビルド差分で enum 値がずれるケースに備えて既知IDも試す。
  add_unique(static_cast<uvc_frame_format>(10));
  add_unique(static_cast<uvc_frame_format>(13));
  return out;
}

static bool is_y16_like_format(uvc_frame_format fmt) {
  const int fi = static_cast<int>(fmt);
  const auto candidates = y16_candidate_formats();
  for (const auto c : candidates) {
    if (fi == static_cast<int>(c))
      return true;
  }
  return false;
}

static bool is_y16_fourcc(const uint8_t fourcc[4]) {
  return std::memcmp(fourcc, "Y16 ", 4) == 0;
}

static std::string fourcc_to_string(const uint8_t fourcc[4]) {
  char out[5];
  for (int i = 0; i < 4; ++i) {
    const unsigned char c = fourcc[i];
    out[i] = std::isprint(c) ? static_cast<char>(c) : '.';
  }
  out[4] = '\0';
  return std::string(out, 4);
}

class PT3Source : public IFrameSource {
public:
  PT3Source(double fps = 9.0, bool fps_auto = false,
            bool assume_tlinear = false, uint16_t scale = kDefaultScaleKelvin)
      : fps_(fps), fps_auto_(fps_auto), assume_tlinear_(assume_tlinear),
        scale_(scale) {}

  bool start() override {
    if (running_.exchange(true))
      return true;

    uvc_error_t res = uvc_init(&ctx_, nullptr);
    if (res != UVC_SUCCESS) {
      log(LogLevel::ERROR,
          std::string("uvc_init failed: ") + uvc_strerror(res));
      running_ = false;
      return false;
    }

    constexpr int kVidGroupGets = 0x1e4e;
    res = uvc_find_device(ctx_, &dev_, kVidGroupGets, 0, nullptr);
    if (res != UVC_SUCCESS) {
      log(LogLevel::ERROR, "PureThermal/GroupGets device not found.");
      cleanup();
      return false;
    }

    res = uvc_open(dev_, &devh_);
    if (res != UVC_SUCCESS) {
      log(LogLevel::ERROR,
          std::string("uvc_open failed: ") + uvc_strerror(res));
      cleanup();
      return false;
    }

    if (dev_) {
      uvc_device_descriptor_t *desc = nullptr;
      if (uvc_get_device_descriptor(dev_, &desc) == UVC_SUCCESS && desc) {
        char buf[256];
        std::snprintf(
            buf, sizeof(buf),
            "Opened device VID:PID=%04x:%04x manufacturer='%s' product='%s'",
            desc->idVendor, desc->idProduct,
            desc->manufacturer ? desc->manufacturer : "",
            desc->product ? desc->product : "");
        log(LogLevel::INFO, buf);
        uvc_free_device_descriptor(desc);
      }
    }

    uvc_stream_ctrl_t ctrl{};
    bool ok = false;

    int rfps = static_cast<int>(fps_ > 0 ? std::lround(fps_) : 9);
    if (rfps <= 0)
      rfps = 9;

    auto try_fmt = [&](uvc_frame_format fmt, int w, int h, int fps) -> bool {
      uvc_error_t r =
          uvc_get_stream_ctrl_format_size(devh_, &ctrl, fmt, w, h, fps);
      if (r == UVC_SUCCESS) {
        if (ctrl.dwMaxPayloadTransferSize == 0) {
          char zbuf[220];
          std::snprintf(zbuf, sizeof(zbuf),
                        "Negotiation invalid (payload=0): fmt=%s w=%d h=%d "
                        "fps=%d if=%u",
                        uvc_format_name(fmt), w, h, fps,
                        static_cast<unsigned>(ctrl.bInterfaceNumber));
          log(LogLevel::WARN, zbuf);
          return false;
        }

        char buf[220];
        std::snprintf(buf, sizeof(buf),
                      "Negotiated stream: fmt=%s w=%d h=%d fps=%d if=%u "
                      "payload=%u fmt_idx=%u frame_idx=%u",
                      uvc_format_name(fmt), w, h, fps,
                      static_cast<unsigned>(ctrl.bInterfaceNumber),
                      static_cast<unsigned>(ctrl.dwMaxPayloadTransferSize),
                      static_cast<unsigned>(ctrl.bFormatIndex),
                      static_cast<unsigned>(ctrl.bFrameIndex));
        log(LogLevel::INFO, buf);
        return true;
      }
      char err[200];
      std::snprintf(err, sizeof(err),
                    "Negotiation failed: fmt=%s w=%d h=%d fps=%d err=%s",
                    uvc_format_name(fmt), w, h, fps, uvc_strerror(r));
      log(LogLevel::WARN, err);
      return false;
    };

    struct StreamCandidate {
      uvc_frame_format fmt;
      int w;
      int h;
      int fps;
      uint32_t interval_100ns;
      uint8_t format_index;
      uint8_t frame_index;
      std::string fourcc;
    };

    auto try_desc = [&](const StreamCandidate &c) -> bool {
      if (c.format_index == 0 || c.frame_index == 0 || c.interval_100ns == 0)
        return false;

      const uint8_t kInterfaceTryOrder[] = {1, 2, 3, 4, 5, 6, 7, 0};
      for (const uint8_t iface : kInterfaceTryOrder) {
        uvc_stream_ctrl_t req{};
        req.bmHint = 1; // dwFrameInterval を優先するヒント
        req.bFormatIndex = c.format_index;
        req.bFrameIndex = c.frame_index;
        req.dwFrameInterval = c.interval_100ns;
        req.bInterfaceNumber = iface;

        uvc_error_t r = uvc_probe_stream_ctrl(devh_, &req);
        if (r == UVC_SUCCESS) {
          if (req.dwMaxPayloadTransferSize == 0) {
            char zbuf[240];
            std::snprintf(zbuf, sizeof(zbuf),
                          "Descriptor probe returned payload=0; rejecting "
                          "(if=%u fmt_idx=%u frame_idx=%u)",
                          static_cast<unsigned>(iface),
                          static_cast<unsigned>(c.format_index),
                          static_cast<unsigned>(c.frame_index));
            log(LogLevel::WARN, zbuf);
            continue;
          }

          ctrl = req;
          char buf[240];
          std::snprintf(buf, sizeof(buf),
                        "Negotiated stream via descriptor: if=%u fmt=%s w=%d "
                        "h=%d fps=%d interval=%u payload=%u",
                        static_cast<unsigned>(iface), uvc_format_name(c.fmt),
                        c.w, c.h, c.fps, static_cast<unsigned>(c.interval_100ns),
                        static_cast<unsigned>(ctrl.dwMaxPayloadTransferSize));
          log(LogLevel::INFO, buf);
          return true;
        }

        char err[280];
        std::snprintf(err, sizeof(err),
                      "Descriptor negotiation failed: if=%u format_index=%u "
                      "frame_index=%u interval=%u err=%s",
                      static_cast<unsigned>(iface),
                      static_cast<unsigned>(c.format_index),
                      static_cast<unsigned>(c.frame_index),
                      static_cast<unsigned>(c.interval_100ns), uvc_strerror(r));
        log(LogLevel::WARN, err);
      }

      return false;
    };

    const auto y16_formats = y16_candidate_formats();
    std::vector<StreamCandidate> candidates;
    const uvc_format_desc_t *fmt = uvc_get_format_descs(devh_);
    for (const uvc_format_desc_t *f = fmt; f; f = f->next) {
      if (f->bDescriptorSubtype != UVC_VS_FORMAT_UNCOMPRESSED &&
          f->bDescriptorSubtype != UVC_VS_FORMAT_FRAME_BASED) {
        continue;
      }

      if (!is_y16_fourcc(f->fourccFormat))
        continue;

      for (const uvc_frame_desc_t *fd = f->frame_descs; fd; fd = fd->next) {
        if (fd->wWidth <= 0 || fd->wHeight <= 0)
          continue;

        const uint16_t w = static_cast<uint16_t>(fd->wWidth);
        const uint16_t h = static_cast<uint16_t>(fd->wHeight);
        if (!is_probe_candidate_size(w, h))
          continue;

        struct IntervalCandidate {
          int fps;
          uint32_t interval_100ns;
        };
        std::vector<IntervalCandidate> interval_candidates;
        if (fps_auto_) {
          if (fd->intervals) {
            for (const uint32_t *p = fd->intervals; *p; ++p) {
              const int fpx = std::max(
                  1, static_cast<int>(std::lround(1e7 / static_cast<double>(*p))));
              bool duplicate = false;
              for (const auto &e : interval_candidates) {
                if (e.interval_100ns == *p) {
                  duplicate = true;
                  break;
                }
              }
              if (!duplicate) {
                interval_candidates.push_back(
                    IntervalCandidate{fpx, static_cast<uint32_t>(*p)});
              }
            }
          }
          if (interval_candidates.empty() && fd->dwDefaultFrameInterval > 0) {
            interval_candidates.push_back(IntervalCandidate{
                std::max(1, static_cast<int>(std::lround(
                                1e7 / static_cast<double>(fd->dwDefaultFrameInterval)))),
                fd->dwDefaultFrameInterval});
          }
        } else {
          interval_candidates.push_back(
              IntervalCandidate{rfps, static_cast<uint32_t>(std::lround(1e7 / rfps))});
        }

        if (interval_candidates.empty()) {
          interval_candidates.push_back(IntervalCandidate{
              rfps, static_cast<uint32_t>(std::lround(1e7 / rfps))});
        }

        for (const auto &i : interval_candidates) {
          for (const auto fmt_try : y16_formats) {
            candidates.push_back(StreamCandidate{
                fmt_try,
                static_cast<int>(w),
                static_cast<int>(h),
                i.fps,
                i.interval_100ns,
                f->bFormatIndex,
                fd->bFrameIndex,
                fourcc_to_string(f->fourccFormat)});
          }
        }
      }
    }

    if (candidates.empty()) {
      log(LogLevel::WARN,
          "No Y16 candidates found in advertised descriptors; using fallback.");
      for (const auto fmt_try : y16_formats) {
        candidates.push_back(
            StreamCandidate{fmt_try, 160, 120, rfps,
                            static_cast<uint32_t>(std::lround(1e7 / rfps)), 0, 0,
                            "Y16 "});
        candidates.push_back(
            StreamCandidate{fmt_try, 80, 60, rfps,
                            static_cast<uint32_t>(std::lround(1e7 / rfps)), 0, 0,
                            "Y16 "});
      }
    }

    uvc_set_status_callback(devh_, &PT3Source::on_status_static, this);

    for (const auto &c : candidates) {
      char buf[260];
      std::snprintf(
          buf, sizeof(buf),
          "Trying advertised stream: format_index=%u frame_index=%u fourcc='%s' "
          "fmt=%s w=%d h=%d fps=%d interval=%u",
          static_cast<unsigned>(c.format_index),
          static_cast<unsigned>(c.frame_index), c.fourcc.c_str(),
          uvc_format_name(c.fmt), c.w, c.h, c.fps,
          static_cast<unsigned>(c.interval_100ns));
      log(LogLevel::INFO, buf);

      auto start_with_current_ctrl = [&](const char *mode_name) -> bool {
        uvc_error_t sr = uvc_start_streaming(devh_, &ctrl,
                                             &PT3Source::on_frame_static, this, 0);
        if (sr == UVC_SUCCESS) {
          char okbuf[128];
          std::snprintf(okbuf, sizeof(okbuf),
                        "Streaming started (negotiation=%s).", mode_name);
          log(LogLevel::INFO, okbuf);
          return true;
        }
        char err[200];
        std::snprintf(
            err, sizeof(err),
            "Start failed after %s negotiation: err=%s if=%u fmt_idx=%u "
            "frame_idx=%u interval=%u payload=%u",
            mode_name, uvc_strerror(sr), static_cast<unsigned>(ctrl.bInterfaceNumber),
            static_cast<unsigned>(ctrl.bFormatIndex),
            static_cast<unsigned>(ctrl.bFrameIndex),
            static_cast<unsigned>(ctrl.dwFrameInterval),
            static_cast<unsigned>(ctrl.dwMaxPayloadTransferSize));
        log(LogLevel::WARN, err);
        return false;
      };

      if (try_desc(c)) {
        if (start_with_current_ctrl("descriptor")) {
          ok = true;
          break;
        }
      }

      if (try_fmt(c.fmt, c.w, c.h, c.fps)) {
        if (start_with_current_ctrl("format_size")) {
          ok = true;
          break;
        }
      }
    }

    if (!ok) {
      log(LogLevel::ERROR, "Could not negotiate Y16/GRAY16 stream.");
      uvc_print_diag(devh_, stderr);
      cleanup();
      return false;
    }

    return true;
  }

  void stop() override {
    if (!running_.exchange(false))
      return;
    if (devh_)
      uvc_stop_streaming(devh_);
    cleanup();
  }

  std::optional<Frame> latest() override {
    std::lock_guard<std::mutex> lk(mu_);
    return latest_;
  }

  uint16_t width() const override { return negotiated_w_; }
  uint16_t height() const override { return negotiated_h_; }

private:
  static void on_frame_static(uvc_frame_t *frame, void *user) {
    static_cast<PT3Source *>(user)->on_frame(frame);
  }

  static void on_status_static(uvc_status_class status_class, int event,
                               int selector,
                               uvc_status_attribute status_attribute,
                               void *data, size_t data_len, void *user) {
    static_cast<PT3Source *>(user)->on_status(status_class, event, selector,
                                              status_attribute, data, data_len);
  }

  void on_frame(uvc_frame_t *frame) {
    if (!frame || frame->data_bytes == 0)
      return;

    {
      const auto fmt = frame->frame_format;
      char buf[256];
      std::snprintf(
          buf, sizeof(buf),
          "frame received: fmt=%s(%d) width=%u height=%u data_bytes=%zu",
          uvc_format_name(fmt), static_cast<int>(fmt),
          static_cast<unsigned>(frame->width),
          static_cast<unsigned>(frame->height), frame->data_bytes);
      log(LogLevel::INFO, buf);
    }

    if (!is_y16_like_format(frame->frame_format)) {
      log(LogLevel::WARN, "Dropped non-Y16/GRAY16 frame.");
      return;
    }

    const uint16_t src_w = static_cast<uint16_t>(frame->width);
    const uint16_t src_h = static_cast<uint16_t>(frame->height);
    const bool has_telemetry_rows = (src_w == 160 && src_h == 122);

    if (!is_expected_lepton_size(src_w, src_h) && !has_telemetry_rows) {
      char buf[160];
      std::snprintf(buf, sizeof(buf),
                    "Dropped unexpected frame size %ux%u (possible telemetry "
                    "or wrong mode).",
                    static_cast<unsigned>(src_w), static_cast<unsigned>(src_h));
      log(LogLevel::WARN, buf);
      return;
    }

    const size_t src_npx = static_cast<size_t>(src_w) * src_h;
    const size_t expected_bytes = src_npx * sizeof(uint16_t);

    if (frame->data_bytes != expected_bytes) {
      char buf[192];
      std::snprintf(buf, sizeof(buf),
                    "Dropped frame: data_bytes mismatch. got=%zu expected=%zu",
                    frame->data_bytes, expected_bytes);
      log(LogLevel::WARN, buf);
      return;
    }

    uint16_t out_w = src_w;
    uint16_t out_h = src_h;
    if (has_telemetry_rows) {
      // Keep wire output compatible (160x120) by dropping telemetry rows.
      out_h = 120;
      if (!telemetry_strip_logged_) {
        log(LogLevel::WARN,
            "Telemetry frame 160x122 detected; stripping last 2 rows and "
            "publishing 160x120.");
        telemetry_strip_logged_ = true;
      }
    }

    const size_t out_npx = static_cast<size_t>(out_w) * out_h;

    Frame f;
    fill_frame_header(f.hdr,
                      assume_tlinear_ ? kFormatUint16TLinearKelvin
                                      : kFormatUint16RawUnknown,
                      assume_tlinear_ ? scale_ : 0, out_w, out_h, ++frame_id_);

    f.pixels.resize(out_npx);
    std::memcpy(f.pixels.data(), frame->data, out_npx * sizeof(uint16_t));

    log_center_stats(f.pixels, out_w, out_h);

    {
      std::lock_guard<std::mutex> lk(mu_);
      negotiated_w_ = out_w;
      negotiated_h_ = out_h;
      latest_ = std::move(f);
    }
  }

  void on_status(uvc_status_class status_class, int event, int selector,
                 uvc_status_attribute attr, void *, size_t) {
    char buf[160];
    std::snprintf(buf, sizeof(buf),
                  "UVC status event: class=%d event=%d selector=%d attr=%d",
                  static_cast<int>(status_class), event, selector,
                  static_cast<int>(attr));
    log(LogLevel::WARN, buf);
  }

  void cleanup() {
    if (devh_) {
      uvc_close(devh_);
      devh_ = nullptr;
    }
    if (dev_) {
      uvc_unref_device(dev_);
      dev_ = nullptr;
    }
    if (ctx_) {
      uvc_exit(ctx_);
      ctx_ = nullptr;
    }
  }

  double fps_;
  bool fps_auto_;
  bool assume_tlinear_;
  uint16_t scale_;

  std::atomic<bool> running_{false};
  std::mutex mu_;
  std::optional<Frame> latest_;

  uvc_context_t *ctx_{nullptr};
  uvc_device_t *dev_{nullptr};
  uvc_device_handle_t *devh_{nullptr};

  uint16_t negotiated_w_{160};
  uint16_t negotiated_h_{120};
  uint32_t frame_id_{0};
  bool telemetry_strip_logged_{false};
};
#endif

// ===== WebSocket session =====

class Hub;

class Session : public std::enable_shared_from_this<Session> {
public:
  Session(tcp::socket sock, Hub &hub) : ws_(std::move(sock)), hub_(hub) {}
  void start();
  void send(std::shared_ptr<std::vector<uint8_t>> msg);

private:
  void on_accept(beast::error_code ec);
  void do_read();
  void on_read(beast::error_code ec, std::size_t bytes);
  void do_write();
  void on_write(beast::error_code ec, std::size_t bytes);

  ws::stream<tcp::socket> ws_;
  Hub &hub_;
  beast::flat_buffer rbuf_;

  std::mutex wmu_;
  std::deque<std::shared_ptr<std::vector<uint8_t>>> wq_;
  bool writing_{false};
};

class Hub {
public:
  void join(std::shared_ptr<Session> s) {
    std::lock_guard<std::mutex> lk(mu_);
    sessions_.insert(std::move(s));
  }

  void leave(const std::shared_ptr<Session> &s) {
    std::lock_guard<std::mutex> lk(mu_);
    sessions_.erase(s);
  }

  void broadcast(std::shared_ptr<std::vector<uint8_t>> msg) {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto &s : sessions_) {
      s->send(msg);
    }
  }

private:
  std::mutex mu_;
  std::unordered_set<std::shared_ptr<Session>> sessions_;
};

void Session::start() {
  ws_.set_option(ws::stream_base::timeout::suggested(beast::role_type::server));
  ws_.set_option(ws::stream_base::decorator([](ws::response_type &res) {
    res.set(beast::http::field::server, "lepton_ws_server");
  }));
  ws_.binary(true);
  ws_.async_accept(
      beast::bind_front_handler(&Session::on_accept, shared_from_this()));
}

void Session::on_accept(beast::error_code ec) {
  if (ec)
    return;
  hub_.join(shared_from_this());
  do_read();
}

void Session::do_read() {
  ws_.async_read(
      rbuf_, beast::bind_front_handler(&Session::on_read, shared_from_this()));
}

void Session::on_read(beast::error_code ec, std::size_t) {
  if (ec) {
    hub_.leave(shared_from_this());
    return;
  }
  rbuf_.consume(rbuf_.size());
  do_read();
}

void Session::send(std::shared_ptr<std::vector<uint8_t>> msg) {
  std::lock_guard<std::mutex> lk(wmu_);

  constexpr size_t kMaxQueue = 2;
  if (wq_.size() >= kMaxQueue) {
    wq_.pop_front();
  }
  wq_.push_back(std::move(msg));

  if (!writing_) {
    writing_ = true;
    asio::post(ws_.get_executor(),
               [self = shared_from_this()] { self->do_write(); });
  }
}

void Session::do_write() {
  std::shared_ptr<std::vector<uint8_t>> msg;
  {
    std::lock_guard<std::mutex> lk(wmu_);
    if (wq_.empty()) {
      writing_ = false;
      return;
    }
    msg = wq_.front();
    wq_.pop_front();
  }

  ws_.async_write(
      asio::buffer(*msg),
      beast::bind_front_handler(&Session::on_write, shared_from_this()));
}

void Session::on_write(beast::error_code ec, std::size_t) {
  if (ec) {
    hub_.leave(shared_from_this());
    return;
  }
  do_write();
}

// ===== Listener =====

class Listener : public std::enable_shared_from_this<Listener> {
public:
  Listener(asio::io_context &ioc, tcp::endpoint ep, Hub &hub)
      : acceptor_(ioc), hub_(hub) {
    beast::error_code ec;
    acceptor_.open(ep.protocol(), ec);
    if (ec)
      throw beast::system_error(ec);

    acceptor_.set_option(asio::socket_base::reuse_address(true), ec);
    if (ec)
      throw beast::system_error(ec);

    acceptor_.bind(ep, ec);
    if (ec)
      throw beast::system_error(ec);

    acceptor_.listen(asio::socket_base::max_listen_connections, ec);
    if (ec)
      throw beast::system_error(ec);
  }

  void run() { do_accept(); }

private:
  void do_accept() {
    acceptor_.async_accept(
        asio::make_strand(acceptor_.get_executor()),
        [self = shared_from_this()](beast::error_code ec, tcp::socket sock) {
          if (!ec) {
            std::make_shared<Session>(std::move(sock), self->hub_)->start();
          }
          self->do_accept();
        });
  }

  tcp::acceptor acceptor_;
  Hub &hub_;
};

// ===== Frame packing =====

static std::shared_ptr<std::vector<uint8_t>> pack_frame(const Frame &f) {
  auto msg = std::make_shared<std::vector<uint8_t>>();
  const size_t npx = f.pixels.size();
  msg->resize(sizeof(FrameHeader) + npx * sizeof(uint16_t));

  std::memcpy(msg->data(), &f.hdr, sizeof(FrameHeader));
  std::memcpy(msg->data() + sizeof(FrameHeader), f.pixels.data(),
              npx * sizeof(uint16_t));
  return msg;
}

// ===== Args =====

struct Args {
  std::string mode = "dummy"; // dummy | pt3
  uint16_t port = 8765;
  double fps = 9.0;
  bool fps_auto = true;
  uint16_t scale = kDefaultScaleKelvin;
  bool assume_tlinear = false; // 明示指定時のみ true
};

static Args parse_args(int argc, char **argv) {
  Args a;

  for (int i = 1; i < argc; ++i) {
    std::string s = argv[i];

    auto next = [&](std::string &out) {
      if (i + 1 >= argc)
        throw std::runtime_error("missing value for " + s);
      out = argv[++i];
    };

    if (s == "--mode") {
      std::string v;
      next(v);
      a.mode = v;
    } else if (s == "--port") {
      std::string v;
      next(v);
      a.port = static_cast<uint16_t>(std::stoi(v));
    } else if (s == "--fps") {
      std::string v;
      next(v);
      if (v == "auto" || v == "max") {
        a.fps_auto = true;
      } else {
        a.fps = std::stod(v);
        if (a.fps <= 0.0)
          throw std::runtime_error("--fps must be > 0 or auto");
        a.fps_auto = false;
      }
    } else if (s == "--scale") {
      std::string v;
      next(v);
      const int parsed = std::stoi(v);
      if (parsed <= 0 || parsed > 65535) {
        throw std::runtime_error("unsupported --scale (use 1..65535)");
      }
      a.scale = static_cast<uint16_t>(parsed);
    } else if (s == "--assume-tlinear") {
      a.assume_tlinear = true;
    } else if (s == "--help" || s == "-h") {
      std::cout
          << "Usage: lepton_ws_server [--mode dummy|pt3] [--port 8765] "
             "[--fps auto|NUM] [--scale NUM] [--assume-tlinear]\n"
             "Protocol: 32-byte header + uint16 pixels (little-endian)\n"
             "Header.format: 0=RAW_UNKNOWN, 1=UINT16_TLINEAR_KELVIN\n"
             "--assume-tlinear を付けた場合のみ format=1 として配信します。\n";
      std::exit(0);
    } else {
      throw std::runtime_error("unknown option: " + s);
    }
  }

  return a;
}

// ===== Source monitor =====

class SourceMonitor : public IFrameSource {
public:
  SourceMonitor(std::string mode, double fps, bool fps_auto,
                bool assume_tlinear, uint16_t scale)
      : mode_(std::move(mode)), fps_(fps), fps_auto_(fps_auto),
        assume_tlinear_(assume_tlinear), scale_(scale) {}

  bool start() override {
    if (running_.exchange(true))
      return true;
    th_ = std::thread([this] { loop(); });
    return true;
  }

  void stop() override {
    if (!running_.exchange(false))
      return;
    if (th_.joinable())
      th_.join();

    std::unique_ptr<IFrameSource> old;
    {
      std::lock_guard<std::mutex> lk(mu_);
      old = std::move(cur_);
    }
    if (old)
      old->stop();
  }

  std::optional<Frame> latest() override {
    std::lock_guard<std::mutex> lk(mu_);
    if (cur_)
      return cur_->latest();
    return std::nullopt;
  }

  uint16_t width() const override {
    std::lock_guard<std::mutex> lk(mu_);
    return last_w_;
  }

  uint16_t height() const override {
    std::lock_guard<std::mutex> lk(mu_);
    return last_h_;
  }

private:
  std::unique_ptr<IFrameSource> make_source() {
    if (mode_ == "dummy") {
      return std::make_unique<DummySource>(160, 120, fps_, assume_tlinear_,
                                           scale_);
    } else if (mode_ == "pt3") {
#ifdef USE_LIBUVC
      return std::make_unique<PT3Source>(fps_, fps_auto_, assume_tlinear_,
                                         scale_);
#else
      return nullptr;
#endif
    }
    return nullptr;
  }

  void loop() {
    using namespace std::chrono;

    const auto retry_interval_base = milliseconds(800);
    const auto retry_interval_max = seconds(8);
    const auto stall_timeout = duration<double>(
        fps_auto_ ? 4.0 : std::max(3.0, 3.0 / std::max(1.0, fps_)));

    auto last_log_status = steady_clock::now();
    uint32_t last_frame_id = UINT32_MAX;
    auto last_frame_tp = steady_clock::now();

    int consecutive_failures = 0;
    int consecutive_stalls = 0;

    while (running_.load()) {
      std::unique_ptr<IFrameSource> local;
      {
        std::lock_guard<std::mutex> lk(mu_);
        if (cur_)
          local.reset(cur_.release());
      }

      if (!local) {
        auto candidate = make_source();
        if (!candidate) {
          log(LogLevel::WARN, "No source available. Retrying...");
          const int exp = 1 << std::min(consecutive_failures, 4);
          const auto backoff_ms =
              std::min(std::chrono::duration_cast<std::chrono::milliseconds>(
                           retry_interval_max),
                       retry_interval_base * exp);
          std::this_thread::sleep_for(backoff_ms);
          continue;
        }

        log(LogLevel::INFO, "Probing device...");
        if (candidate->start()) {
          const auto warmup_deadline = steady_clock::now() + seconds(2);
          uint32_t warmup_last_id = UINT32_MAX;
          bool warmup_ok = false;

          while (steady_clock::now() < warmup_deadline) {
            auto opt = candidate->latest();
            if (opt) {
              if (warmup_last_id == UINT32_MAX)
                warmup_last_id = opt->hdr.frame_id;
              if (opt->hdr.frame_id != warmup_last_id) {
                warmup_ok = true;
                break;
              }
            }
            std::this_thread::sleep_for(milliseconds(50));
          }

          if (!warmup_ok && mode_ != "dummy") {
            log(LogLevel::WARN,
                "No frames during warmup; closing and retrying.");
            candidate->stop();
            consecutive_failures = std::min(consecutive_failures + 1, 16);
            const int exp = 1 << std::min(consecutive_failures, 4);
            const auto backoff_ms =
                std::min(std::chrono::duration_cast<std::chrono::milliseconds>(
                             retry_interval_max),
                         retry_interval_base * exp);
            std::this_thread::sleep_for(backoff_ms);
            continue;
          }

          log(LogLevel::INFO, "Device connected. Streaming started.");
          {
            std::lock_guard<std::mutex> lk(mu_);
            cur_ = std::move(candidate);
          }
          last_frame_id = UINT32_MAX;
          last_frame_tp = steady_clock::now();
          consecutive_failures = 0;
          consecutive_stalls = 0;
        } else {
          log(LogLevel::WARN, "Device not available. Will retry.");
          consecutive_failures = std::min(consecutive_failures + 1, 16);
          const int exp = 1 << std::min(consecutive_failures, 4);
          const auto backoff_ms =
              std::min(std::chrono::duration_cast<std::chrono::milliseconds>(
                           retry_interval_max),
                       retry_interval_base * exp);
          std::this_thread::sleep_for(backoff_ms);
        }
      } else {
        bool keep = true;
        auto opt = local->latest();

        if (opt) {
          if (opt->hdr.frame_id != last_frame_id) {
            last_frame_id = opt->hdr.frame_id;
            last_frame_tp = steady_clock::now();
            last_w_ = opt->hdr.width;
            last_h_ = opt->hdr.height;
            consecutive_stalls = 0;
          }
        }

        const auto now = steady_clock::now();
        if (now - last_frame_tp > stall_timeout && mode_ != "dummy") {
          ++consecutive_stalls;
          if (consecutive_stalls >= 2) {
            log(LogLevel::WARN,
                "No frames received recently; restarting stream...");
            keep = false;
          }
        } else {
          consecutive_stalls = 0;
        }

        if (!keep) {
          local->stop();
          log(LogLevel::INFO, "Device disconnected. Will try to reconnect.");
          consecutive_failures = std::min(consecutive_failures + 1, 16);
          const int exp = 1 << std::min(consecutive_failures, 4);
          const auto backoff_ms =
              std::min(std::chrono::duration_cast<std::chrono::milliseconds>(
                           retry_interval_max),
                       retry_interval_base * exp);
          std::this_thread::sleep_for(backoff_ms);
        } else {
          if (now - last_log_status > seconds(5)) {
            const auto age_ms =
                duration_cast<milliseconds>(now - last_frame_tp).count();
            log(LogLevel::INFO,
                "Streaming OK. last_frame_age_ms=" + std::to_string(age_ms));
            last_log_status = now;
          }
        }

        {
          std::lock_guard<std::mutex> lk(mu_);
          if (keep)
            cur_.reset(local.release());
        }

        std::this_thread::sleep_for(milliseconds(100));
      }
    }
  }

  std::string mode_;
  double fps_;
  bool fps_auto_;
  bool assume_tlinear_;
  uint16_t scale_;

  std::atomic<bool> running_{false};
  std::thread th_;
  mutable std::mutex mu_;
  std::unique_ptr<IFrameSource> cur_;
  uint16_t last_w_{0};
  uint16_t last_h_{0};
};

// ===== Main =====

int main(int argc, char **argv) {
  Args args;
  try {
    args = parse_args(argc, argv);
  } catch (const std::exception &e) {
    std::cerr << "Arg error: " << e.what() << "\n";
    return 1;
  }

  std::unique_ptr<IFrameSource> src = std::make_unique<SourceMonitor>(
      args.mode, args.fps, args.fps_auto, args.assume_tlinear, args.scale);

  (void)src->start();

  asio::io_context ioc{1};
  Hub hub;

  auto listener = std::make_shared<Listener>(
      ioc, tcp::endpoint{asio::ip::make_address("127.0.0.1"), args.port}, hub);
  listener->run();

  std::atomic<bool> running{true};

  std::thread broadcaster([&] {
    using namespace std::chrono;
    uint32_t last_id = UINT32_MAX;

    if (!args.fps_auto) {
      const auto period = duration<double>(1.0 / args.fps);
      auto next = steady_clock::now();

      while (running.load()) {
        next += duration_cast<steady_clock::duration>(period);

        auto opt = src->latest();
        if (opt && opt->hdr.frame_id != last_id) {
          last_id = opt->hdr.frame_id;
          hub.broadcast(pack_frame(*opt));
        }

        std::this_thread::sleep_until(next);
      }
    } else {
      const auto poll = 1ms;
      while (running.load()) {
        auto opt = src->latest();
        if (opt && opt->hdr.frame_id != last_id) {
          last_id = opt->hdr.frame_id;
          hub.broadcast(pack_frame(*opt));
        } else {
          std::this_thread::sleep_for(poll);
        }
      }
    }
  });

  log(LogLevel::INFO,
      std::string("WebSocket server on ws://127.0.0.1:") +
          std::to_string(args.port) + " mode=" + args.mode +
          (args.fps_auto ? " fps=auto" : (" fps=" + std::to_string(args.fps))) +
          " assume_tlinear=" + yesno(args.assume_tlinear) +
          " scale=" + std::to_string(args.scale));

  ioc.run();

  running.store(false);
  broadcaster.join();
  src->stop();
  return 0;
}
