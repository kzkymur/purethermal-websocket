#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cctype>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>
#ifdef ERROR
#undef ERROR
#endif
#endif

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

static std::string trim_ascii(std::string s) {
  const auto not_space = [](unsigned char c) { return !std::isspace(c); };
  const auto b = std::find_if(s.begin(), s.end(), not_space);
  if (b == s.end())
    return "";
  const auto e = std::find_if(s.rbegin(), s.rend(), not_space).base();
  return std::string(b, e);
}

static std::string ascii_lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

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
  virtual bool request_ffc() { return false; }
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

#if defined(_WIN32)
class WindowsPT3Source : public IFrameSource {
public:
  WindowsPT3Source(bool assume_tlinear, uint16_t scale)
      : assume_tlinear_(assume_tlinear), scale_(scale) {}

  bool start() override {
    if (running_.exchange(true))
      return true;
    startup_complete_.store(false);
    startup_ok_.store(false);
    thread_ = std::thread([this] { capture_loop(); });

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!startup_complete_.load() &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!startup_ok_.load()) {
      stop();
      return false;
    }
    return true;
  }

  void stop() override {
    running_.store(false);
    if (thread_.joinable())
      thread_.join();
  }

  std::optional<Frame> latest() override {
    std::lock_guard<std::mutex> lock(mutex_);
    return latest_;
  }

  uint16_t width() const override { return width_.load(); }
  uint16_t height() const override { return height_.load(); }

private:
  template <typename T> using ComPtr = Microsoft::WRL::ComPtr<T>;

  static bool is_y16(const GUID &subtype) {
    constexpr uint32_t kY16Fourcc =
        static_cast<uint32_t>('Y') | (static_cast<uint32_t>('1') << 8) |
        (static_cast<uint32_t>('6') << 16) | (static_cast<uint32_t>(' ') << 24);
    return subtype.Data1 == kY16Fourcc;
  }

  void fail_start(const std::string &message) {
    log(LogLevel::ERROR, message);
    startup_ok_.store(false);
    startup_complete_.store(true);
    running_.store(false);
  }

  void capture_loop() {
    constexpr DWORD kVideoStreamIndex = 0;
    const HRESULT com_hr =
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitialize_com = SUCCEEDED(com_hr);
    if (FAILED(com_hr) && com_hr != RPC_E_CHANGED_MODE) {
      fail_start("CoInitializeEx failed: " + std::to_string(com_hr));
      return;
    }

    HRESULT hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) {
      fail_start("MFStartup failed: " + std::to_string(hr));
      if (uninitialize_com)
        CoUninitialize();
      return;
    }

    ComPtr<IMFAttributes> attributes;
    hr = MFCreateAttributes(&attributes, 1);
    if (SUCCEEDED(hr)) {
      hr = attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                               MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    }

    IMFActivate **devices = nullptr;
    UINT32 device_count = 0;
    if (SUCCEEDED(hr))
      hr = MFEnumDeviceSources(attributes.Get(), &devices, &device_count);

    ComPtr<IMFActivate> selected;
    for (UINT32 i = 0; SUCCEEDED(hr) && i < device_count; ++i) {
      wchar_t *name = nullptr;
      UINT32 name_length = 0;
      if (SUCCEEDED(devices[i]->GetAllocatedString(
              MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &name_length))) {
        const std::wstring friendly(name, name_length);
        if (friendly.find(L"PureThermal") != std::wstring::npos)
          selected = devices[i];
        CoTaskMemFree(name);
      }
      devices[i]->Release();
    }
    CoTaskMemFree(devices);

    if (!selected) {
      fail_start("PureThermal camera was not found by Windows Media Foundation.");
      MFShutdown();
      if (uninitialize_com)
        CoUninitialize();
      return;
    }

    ComPtr<IMFMediaSource> media_source;
    hr = selected->ActivateObject(IID_PPV_ARGS(&media_source));
    ComPtr<IMFSourceReader> reader;
    if (SUCCEEDED(hr))
      hr = MFCreateSourceReaderFromMediaSource(media_source.Get(), nullptr,
                                               &reader);
    if (FAILED(hr)) {
      fail_start("Failed to create the PureThermal Media Foundation source "
                 "reader: " +
                 std::to_string(hr));
      if (media_source)
        media_source->Shutdown();
      MFShutdown();
      if (uninitialize_com)
        CoUninitialize();
      return;
    }

    ComPtr<IMFMediaType> selected_type;
    UINT32 selected_width = 0;
    UINT32 selected_height = 0;
    DWORD selected_stream_index = kVideoStreamIndex;
    ComPtr<IMFPresentationDescriptor> presentation;
    if (SUCCEEDED(hr))
      hr = media_source->CreatePresentationDescriptor(&presentation);
    DWORD stream_count = 0;
    if (SUCCEEDED(hr))
      hr = presentation->GetStreamDescriptorCount(&stream_count);
    log(LogLevel::INFO,
        "PureThermal presentation: hr=" + std::to_string(hr) +
            " streams=" + std::to_string(stream_count));

    for (DWORD stream = 0; SUCCEEDED(hr) && stream < stream_count &&
                           !selected_type;
         ++stream) {
      BOOL stream_selected = FALSE;
      ComPtr<IMFStreamDescriptor> descriptor;
      if (FAILED(presentation->GetStreamDescriptorByIndex(
              stream, &stream_selected, &descriptor)))
        continue;
      ComPtr<IMFMediaTypeHandler> handler;
      if (FAILED(descriptor->GetMediaTypeHandler(&handler)))
        continue;
      GUID major_type{};
      if (FAILED(handler->GetMajorType(&major_type)) ||
          major_type != MFMediaType_Video)
        continue;

      DWORD type_count = 0;
      if (FAILED(handler->GetMediaTypeCount(&type_count)))
        continue;
      for (DWORD index = 0; index < type_count; ++index) {
        ComPtr<IMFMediaType> type;
        if (FAILED(handler->GetMediaTypeByIndex(index, &type)))
          continue;

        GUID subtype{};
        UINT32 w = 0;
        UINT32 h = 0;
        const bool has_subtype =
            SUCCEEDED(type->GetGUID(MF_MT_SUBTYPE, &subtype));
        const bool has_size = SUCCEEDED(
            MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &w, &h));
        if (has_subtype && has_size) {
          char format_log[180];
          const uint32_t fourcc = subtype.Data1;
          std::snprintf(
              format_log, sizeof(format_log),
              "PureThermal stream[%lu] format[%lu]: fourcc='%c%c%c%c' "
              "data1=0x%08lx %lux%lu",
              static_cast<unsigned long>(stream),
              static_cast<unsigned long>(index),
              static_cast<char>(fourcc & 0xff),
              static_cast<char>((fourcc >> 8) & 0xff),
              static_cast<char>((fourcc >> 16) & 0xff),
              static_cast<char>((fourcc >> 24) & 0xff),
              static_cast<unsigned long>(fourcc), static_cast<unsigned long>(w),
              static_cast<unsigned long>(h));
          log(LogLevel::INFO, format_log);
        }
        if (has_subtype && has_size && is_y16(subtype) &&
            is_probe_candidate_size(static_cast<uint16_t>(w),
                                    static_cast<uint16_t>(h))) {
          selected_type = type;
          selected_width = w;
          selected_height = h;
          selected_stream_index = stream;
          if (w == 160 && h == 120)
            break;
        }
      }
    }

    if (!selected_type) {
      fail_start("PureThermal does not expose a Y16 160x120/160x122/80x60 "
                 "Media Foundation format.");
      if (media_source)
        media_source->Shutdown();
      MFShutdown();
      if (uninitialize_com)
        CoUninitialize();
      return;
    }

    hr = reader->SetCurrentMediaType(selected_stream_index, nullptr,
                                     selected_type.Get());
    if (FAILED(hr)) {
      fail_start("Failed to select the PureThermal Y16 format: " +
                 std::to_string(hr));
      media_source->Shutdown();
      MFShutdown();
      if (uninitialize_com)
        CoUninitialize();
      return;
    }

    width_.store(static_cast<uint16_t>(selected_width));
    height_.store(static_cast<uint16_t>(selected_height));
    startup_ok_.store(true);
    startup_complete_.store(true);
    log(LogLevel::INFO,
        "Windows Media Foundation opened PureThermal Y16 " +
            std::to_string(selected_width) + "x" +
            std::to_string(selected_height));

    uint32_t frame_id = 0;
    while (running_.load()) {
      DWORD stream_index = 0;
      DWORD flags = 0;
      LONGLONG timestamp = 0;
      ComPtr<IMFSample> sample;
      hr = reader->ReadSample(selected_stream_index, 0, &stream_index, &flags,
                              &timestamp, &sample);
      if (FAILED(hr)) {
        log(LogLevel::ERROR,
            "PureThermal ReadSample failed: " + std::to_string(hr));
        break;
      }
      if (!sample)
        continue;

      ComPtr<IMFMediaBuffer> buffer;
      if (FAILED(sample->ConvertToContiguousBuffer(&buffer)))
        continue;
      BYTE *data = nullptr;
      DWORD max_length = 0;
      DWORD length = 0;
      if (FAILED(buffer->Lock(&data, &max_length, &length)))
        continue;

      const size_t pixel_count =
          static_cast<size_t>(selected_width) * selected_height;
      if (length >= pixel_count * sizeof(uint16_t)) {
        Frame frame;
        fill_frame_header(
            frame.hdr,
            assume_tlinear_ ? kFormatUint16TLinearKelvin
                            : kFormatUint16RawUnknown,
            assume_tlinear_ ? scale_ : 0, static_cast<uint16_t>(selected_width),
            static_cast<uint16_t>(selected_height), frame_id++);
        frame.pixels.resize(pixel_count);
        std::memcpy(frame.pixels.data(), data,
                    pixel_count * sizeof(uint16_t));
        std::lock_guard<std::mutex> lock(mutex_);
        latest_ = std::move(frame);
      }
      buffer->Unlock();
    }

    running_.store(false);
    media_source->Shutdown();
    MFShutdown();
    if (uninitialize_com)
      CoUninitialize();
  }

  bool assume_tlinear_;
  uint16_t scale_;
  std::atomic<bool> running_{false};
  std::atomic<bool> startup_complete_{false};
  std::atomic<bool> startup_ok_{false};
  std::atomic<uint16_t> width_{0};
  std::atomic<uint16_t> height_{0};
  std::thread thread_;
  std::mutex mutex_;
  std::optional<Frame> latest_;
};
#endif

#ifdef USE_LIBUVC
#include <libuvc/libuvc.h>
#include <LEPTON_SDK.h>
#include <LEPTON_SYS.h>

struct LeptonUvcPortContext {
  uvc_device_handle_t *devh{nullptr};
};

static int lepton_command_id_to_unit_id(LEP_COMMAND_ID command_id) {
  constexpr LEP_COMMAND_ID kCidAgcModule = 0x0100;
  constexpr LEP_COMMAND_ID kCidOemModule = 0x0800;
  constexpr LEP_COMMAND_ID kCidRadModule = 0x0E00;
  constexpr LEP_COMMAND_ID kCidSysModule = 0x0200;
  constexpr LEP_COMMAND_ID kCidVidModule = 0x0300;

  constexpr int kXuLepAgcId = 3;
  constexpr int kXuLepOemId = 4;
  constexpr int kXuLepRadId = 5;
  constexpr int kXuLepSysId = 6;
  constexpr int kXuLepVidId = 7;

  switch (command_id & 0x3f00) { // Ignore upper command-type bits.
  case kCidAgcModule:
    return kXuLepAgcId;
  case kCidOemModule:
    return kXuLepOemId;
  case kCidRadModule:
    return kXuLepRadId;
  case kCidSysModule:
    return kXuLepSysId;
  case kCidVidModule:
    return kXuLepVidId;
  default:
    return static_cast<int>(LEP_RANGE_ERROR);
  }
}

extern "C" LEP_RESULT UVC_GetAttribute(LEP_CAMERA_PORT_DESC_T_PTR portDescPtr,
                                       LEP_COMMAND_ID commandID,
                                       LEP_ATTRIBUTE_T_PTR attributePtr,
                                       LEP_UINT16 attributeWordLength) {
  if (!portDescPtr || !portDescPtr->userPtr || !attributePtr) {
    return LEP_COMM_PORT_NOT_OPEN;
  }

  auto *ctx = static_cast<LeptonUvcPortContext *>(portDescPtr->userPtr);
  if (!ctx->devh) {
    return LEP_COMM_PORT_NOT_OPEN;
  }

  const int unit_id = lepton_command_id_to_unit_id(commandID);
  if (unit_id < 0) {
    return static_cast<LEP_RESULT>(unit_id);
  }

  const int control_id = ((commandID & 0x00ff) >> 2) + 1;
  const int payload_bytes = static_cast<int>(attributeWordLength) * 2;
  const int result = uvc_get_ctrl(ctx->devh, unit_id, control_id, attributePtr,
                                  payload_bytes, UVC_GET_CUR);
  if (result != payload_bytes) {
    return LEP_COMM_ERROR_READING_COMM;
  }
  return LEP_OK;
}

extern "C" LEP_RESULT UVC_SetAttribute(LEP_CAMERA_PORT_DESC_T_PTR portDescPtr,
                                       LEP_COMMAND_ID commandID,
                                       LEP_ATTRIBUTE_T_PTR attributePtr,
                                       LEP_UINT16 attributeWordLength) {
  if (!portDescPtr || !portDescPtr->userPtr || !attributePtr) {
    return LEP_COMM_PORT_NOT_OPEN;
  }

  auto *ctx = static_cast<LeptonUvcPortContext *>(portDescPtr->userPtr);
  if (!ctx->devh) {
    return LEP_COMM_PORT_NOT_OPEN;
  }

  const int unit_id = lepton_command_id_to_unit_id(commandID);
  if (unit_id < 0) {
    return static_cast<LEP_RESULT>(unit_id);
  }

  const int control_id = ((commandID & 0x00ff) >> 2) + 1;
  const int payload_bytes = static_cast<int>(attributeWordLength) * 2;
  const int result =
      uvc_set_ctrl(ctx->devh, unit_id, control_id, attributePtr, payload_bytes);
  if (result != payload_bytes) {
    return LEP_COMM_ERROR_WRITING_COMM;
  }
  return LEP_OK;
}

extern "C" LEP_RESULT UVC_RunCommand(LEP_CAMERA_PORT_DESC_T_PTR portDescPtr,
                                     LEP_COMMAND_ID commandID) {
  if (!portDescPtr || !portDescPtr->userPtr) {
    return LEP_COMM_PORT_NOT_OPEN;
  }

  auto *ctx = static_cast<LeptonUvcPortContext *>(portDescPtr->userPtr);
  if (!ctx->devh) {
    return LEP_COMM_PORT_NOT_OPEN;
  }

  const int unit_id = lepton_command_id_to_unit_id(commandID);
  if (unit_id < 0) {
    return static_cast<LEP_RESULT>(unit_id);
  }

  const int control_id = ((commandID & 0x00ff) >> 2) + 1;
  uint8_t run_arg = static_cast<uint8_t>(control_id);
  const int result = uvc_set_ctrl(ctx->devh, unit_id, control_id, &run_arg, 1);
  if (result != 1) {
    return LEP_COMM_ERROR_WRITING_COMM;
  }
  return LEP_OK;
}

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
            bool assume_tlinear = true, uint16_t scale = kDefaultScaleKelvin,
            std::string ffc_mode = "manual")
      : fps_(fps), fps_auto_(fps_auto), assume_tlinear_(assume_tlinear),
        scale_(scale), desired_ffc_mode_(std::move(ffc_mode)) {}

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

    {
      std::lock_guard<std::mutex> lk(ffc_mu_);
      if (!ensure_ffc_mode_locked("startup")) {
        log(LogLevel::WARN,
            "Failed to apply configured FFC shutter mode at startup.");
      }
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
  bool request_ffc() override {
    std::lock_guard<std::mutex> lk(ffc_mu_);
    if (!ensure_ffc_mode_locked("request")) {
      return false;
    }
    LEP_RESULT res = LEP_RunSysFFCNormalization(&lep_port_);
    if (res != LEP_OK) {
      log(LogLevel::ERROR,
          "LEP_RunSysFFCNormalization failed: code=" + std::to_string(res));
      return false;
    }

    log(LogLevel::INFO, "FFC normalization requested.");
    return true;
  }

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
    std::lock_guard<std::mutex> lk(ffc_mu_);
    lep_ctx_.devh = nullptr;
    lep_port_ = LEP_CAMERA_PORT_DESC_T{};
  }

  double fps_;
  bool fps_auto_;
  bool assume_tlinear_;
  uint16_t scale_;
  std::string desired_ffc_mode_;

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
  static const char *ffc_mode_name(LEP_SYS_FFC_SHUTTER_MODE_E mode) {
    switch (mode) {
    case LEP_SYS_FFC_SHUTTER_MODE_MANUAL:
      return "MANUAL";
    case LEP_SYS_FFC_SHUTTER_MODE_AUTO:
      return "AUTO";
    case LEP_SYS_FFC_SHUTTER_MODE_EXTERNAL:
      return "EXTERNAL";
    default:
      return "UNKNOWN";
    }
  }

  LEP_SYS_FFC_SHUTTER_MODE_E desired_ffc_mode_enum() const {
    if (desired_ffc_mode_ == "auto")
      return LEP_SYS_FFC_SHUTTER_MODE_AUTO;
    if (desired_ffc_mode_ == "external")
      return LEP_SYS_FFC_SHUTTER_MODE_EXTERNAL;
    return LEP_SYS_FFC_SHUTTER_MODE_MANUAL;
  }

  bool ensure_ffc_mode_locked(const char *context) {
    if (!devh_) {
      log(LogLevel::WARN, std::string("FFC ") + context +
                              " rejected: UVC handle is unavailable.");
      return false;
    }

    lep_ctx_.devh = devh_;
    lep_port_.portID = 1;
    lep_port_.portType = LEP_CCI_UVC;
    lep_port_.userPtr = &lep_ctx_;

    LEP_SYS_FFC_SHUTTER_MODE_OBJ_T obj{};
    LEP_RESULT res = LEP_GetSysFfcShutterModeObj(&lep_port_, &obj);
    if (res != LEP_OK) {
      log(LogLevel::ERROR,
          std::string("LEP_GetSysFfcShutterModeObj failed (") + context +
              "): code=" + std::to_string(res));
      return false;
    }

    const auto desired_mode = desired_ffc_mode_enum();
    if (obj.shutterMode != desired_mode) {
      const auto prev = obj.shutterMode;
      obj.shutterMode = desired_mode;
      res = LEP_SetSysFfcShutterModeObj(&lep_port_, obj);
      if (res != LEP_OK) {
        log(LogLevel::ERROR,
            std::string("LEP_SetSysFfcShutterModeObj failed (") + context +
                "): code=" + std::to_string(res));
        return false;
      }
      log(LogLevel::INFO, std::string("FFC shutter mode changed ") +
                              ffc_mode_name(prev) + " -> " +
                              ffc_mode_name(desired_mode) + " (" + context +
                              ")");
    }

    LEP_SYS_FFC_SHUTTER_MODE_OBJ_T verify{};
    res = LEP_GetSysFfcShutterModeObj(&lep_port_, &verify);
    if (res != LEP_OK) {
      log(LogLevel::ERROR,
          std::string("LEP_GetSysFfcShutterModeObj verify failed (") + context +
              "): code=" + std::to_string(res));
      return false;
    }
    if (verify.shutterMode != desired_mode) {
      log(LogLevel::ERROR, std::string("FFC shutter mode is still ") +
                               ffc_mode_name(verify.shutterMode) +
                               " after set to " + ffc_mode_name(desired_mode) +
                               " (" + context + ").");
      return false;
    }
    return true;
  }

  std::mutex ffc_mu_;
  LeptonUvcPortContext lep_ctx_{};
  LEP_CAMERA_PORT_DESC_T lep_port_{};
};
#endif

// ===== WebSocket session =====

#if 0
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
  using FfcHandler = std::function<bool()>;
  explicit Hub(FfcHandler on_ffc_request)
      : on_ffc_request_(std::move(on_ffc_request)) {}

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

  bool request_ffc() const {
    if (!on_ffc_request_)
      return false;
    return on_ffc_request_();
  }

private:
  std::mutex mu_;
  std::unordered_set<std::shared_ptr<Session>> sessions_;
  FfcHandler on_ffc_request_;
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

  const std::string payload = beast::buffers_to_string(rbuf_.data());
  bool is_ffc = false;
  if (!ws_.got_text()) {
    is_ffc = (payload.size() == 1 &&
              static_cast<unsigned char>(payload[0]) == 0x01u);
  } else {
    const std::string cmd = ascii_lower(trim_ascii(payload));
    is_ffc = (cmd == "ffc");
  }

  if (is_ffc) {
    const bool accepted = hub_.request_ffc();
    if (!accepted) {
      log(LogLevel::WARN, "FFC request rejected.");
    }
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
#endif

// ===== Args =====

struct Args {
  std::string mode = "dummy"; // dummy | pt3
  double fps = 9.0;
  bool fps_auto = true;
  uint16_t scale = kDefaultScaleKelvin;
  bool assume_tlinear = true;
  std::string ffc_mode = "manual"; // manual | auto | external
  std::string output = "capture.y16";
  std::string video_output;
  double duration_seconds = 10.0;
  uint64_t frame_limit = 0;
  double video_min_c = 20.0;
  double video_max_c = 40.0;
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
    } else if (s == "--output" || s == "-o") {
      std::string v;
      next(v);
      a.output = v;
    } else if (s == "--duration") {
      std::string v;
      next(v);
      a.duration_seconds = std::stod(v);
      if (a.duration_seconds < 0.0)
        throw std::runtime_error("--duration must be >= 0");
    } else if (s == "--frames") {
      std::string v;
      next(v);
      const auto parsed = std::stoll(v);
      if (parsed <= 0)
        throw std::runtime_error("--frames must be > 0");
      a.frame_limit = static_cast<uint64_t>(parsed);
    } else if (s == "--video-output") {
      next(a.video_output);
    } else if (s == "--video-min-c") {
      std::string v;
      next(v);
      a.video_min_c = std::stod(v);
    } else if (s == "--video-max-c") {
      std::string v;
      next(v);
      a.video_max_c = std::stod(v);
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
    } else if (s == "--no-assume-tlinear") {
      a.assume_tlinear = false;
    } else if (s == "--ffc-mode") {
      std::string v;
      next(v);
      v = ascii_lower(trim_ascii(v));
      if (v != "manual" && v != "auto" && v != "external")
        throw std::runtime_error("--ffc-mode must be manual|auto|external");
      a.ffc_mode = v;
    } else if (s == "--help" || s == "-h") {
      std::cout
          << "Usage: lepton_capture [--mode dummy|pt3] [-o capture.y16] "
             "[--duration SEC|--frames NUM] "
             "[--video-output capture.avi] "
             "[--video-min-c NUM] [--video-max-c NUM] "
             "[--fps auto|NUM] [--scale NUM] [--assume-tlinear|--no-assume-tlinear] "
             "[--ffc-mode manual|auto|external]\n"
             "Output: headerless little-endian uint16 (gray16le) frames.\n"
             "デフォルトは format=1/scale=--scale。RAWとして配信したい場合のみ\n"
             "--no-assume-tlinear を指定してください。\n";
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
                bool assume_tlinear, uint16_t scale, std::string ffc_mode)
      : mode_(std::move(mode)), fps_(fps), fps_auto_(fps_auto),
        assume_tlinear_(assume_tlinear), scale_(scale),
        ffc_mode_(std::move(ffc_mode)) {}

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

  bool request_ffc() override {
    if (mode_ != "pt3") {
      log(LogLevel::WARN,
          "FFC request ignored: control is available only in --mode pt3.");
      return false;
    }
    ffc_requested_.store(true);
    log(LogLevel::INFO, "FFC request queued.");
    return true;
  }

private:
  std::unique_ptr<IFrameSource> make_source() {
    if (mode_ == "dummy") {
      return std::make_unique<DummySource>(static_cast<uint16_t>(160),
                                           static_cast<uint16_t>(120), fps_,
                                           assume_tlinear_, scale_);
    } else if (mode_ == "pt3") {
#if defined(_WIN32)
      return std::make_unique<WindowsPT3Source>(assume_tlinear_, scale_);
#elif defined(USE_LIBUVC)
      return std::make_unique<PT3Source>(fps_, fps_auto_, assume_tlinear_,
                                         scale_, ffc_mode_);
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
        if (ffc_requested_.exchange(false)) {
          const bool ok = local->request_ffc();
          if (!ok) {
            log(LogLevel::WARN, "FFC request failed.");
          }
        }

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
  std::string ffc_mode_;

  std::atomic<bool> running_{false};
  std::thread th_;
  mutable std::mutex mu_;
  std::unique_ptr<IFrameSource> cur_;
  uint16_t last_w_{0};
  uint16_t last_h_{0};
  std::atomic<bool> ffc_requested_{false};
};

// ===== Main =====

class AviWriter {
public:
  bool open(const std::string &path, uint16_t width, uint16_t height,
            double fps) {
    width_ = width;
    height_ = height;
    row_bytes_ = (static_cast<uint32_t>(width_) * 3u + 3u) & ~3u;
    frame_bytes_ = row_bytes_ * height_;
    out_.open(path, std::ios::binary | std::ios::trunc);
    if (!out_)
      return false;

    fourcc("RIFF");
    riff_size_pos_ = out_.tellp();
    u32(0);
    fourcc("AVI ");

    fourcc("LIST");
    const auto hdrl_size_pos = out_.tellp();
    u32(0);
    const auto hdrl_start = out_.tellp();
    fourcc("hdrl");

    fourcc("avih");
    u32(56);
    u32(static_cast<uint32_t>(1000000.0 / fps));
    u32(static_cast<uint32_t>(frame_bytes_ * fps));
    u32(0);
    u32(0x10); // AVIF_HASINDEX
    total_frames_pos_ = out_.tellp();
    u32(0);
    u32(0);
    u32(1);
    u32(frame_bytes_);
    u32(width_);
    u32(height_);
    for (int i = 0; i < 4; ++i)
      u32(0);

    fourcc("LIST");
    const auto strl_size_pos = out_.tellp();
    u32(0);
    const auto strl_start = out_.tellp();
    fourcc("strl");

    fourcc("strh");
    u32(56);
    fourcc("vids");
    fourcc("DIB ");
    u32(0);
    u16(0);
    u16(0);
    u32(0);
    u32(1000);
    u32(static_cast<uint32_t>(std::lround(fps * 1000.0)));
    u32(0);
    stream_length_pos_ = out_.tellp();
    u32(0);
    u32(frame_bytes_);
    u32(0xffffffffu);
    u32(0);
    u16(0);
    u16(0);
    u16(width_);
    u16(height_);

    fourcc("strf");
    u32(40);
    u32(40);
    u32(width_);
    u32(height_); // positive height: bottom-up BGR rows
    u16(1);
    u16(24);
    u32(0);
    u32(frame_bytes_);
    u32(0);
    u32(0);
    u32(0);
    u32(0);

    const auto after_strl = out_.tellp();
    patch_u32(strl_size_pos,
              static_cast<uint32_t>(after_strl - strl_start));
    patch_u32(hdrl_size_pos,
              static_cast<uint32_t>(after_strl - hdrl_start));

    fourcc("LIST");
    movi_size_pos_ = out_.tellp();
    u32(0);
    movi_start_ = out_.tellp();
    fourcc("movi");
    return out_.good();
  }

  bool write_frame(const Frame &frame, double min_c, double max_c) {
    if (!out_ || frame.hdr.width != width_ || frame.hdr.height != height_ ||
        frame.hdr.scale == 0 || max_c <= min_c)
      return false;

    const auto chunk_pos = out_.tellp();
    fourcc("00db");
    u32(frame_bytes_);
    index_offsets_.push_back(
        static_cast<uint32_t>(chunk_pos - movi_start_));

    row_.assign(row_bytes_, 0);
    for (int y = static_cast<int>(height_) - 1; y >= 0; --y) {
      for (uint16_t x = 0; x < width_; ++x) {
        const uint16_t value =
            frame.pixels[static_cast<size_t>(y) * width_ + x];
        const double celsius =
            static_cast<double>(value) / frame.hdr.scale - 273.15;
        const double t =
            std::clamp((celsius - min_c) / (max_c - min_c), 0.0, 1.0);
        const auto rgb = thermal_color(t);
        const size_t dst = static_cast<size_t>(x) * 3;
        row_[dst] = rgb[2];
        row_[dst + 1] = rgb[1];
        row_[dst + 2] = rgb[0];
      }
      out_.write(reinterpret_cast<const char *>(row_.data()), row_.size());
    }
    ++frames_;
    return out_.good();
  }

  bool close() {
    if (!out_)
      return false;
    const auto movi_end = out_.tellp();
    patch_u32(movi_size_pos_,
              static_cast<uint32_t>(movi_end - movi_start_));

    fourcc("idx1");
    u32(static_cast<uint32_t>(index_offsets_.size() * 16u));
    for (const uint32_t offset : index_offsets_) {
      fourcc("00db");
      u32(0x10);
      u32(offset);
      u32(frame_bytes_);
    }

    const auto file_end = out_.tellp();
    patch_u32(total_frames_pos_, frames_);
    patch_u32(stream_length_pos_, frames_);
    patch_u32(riff_size_pos_,
              static_cast<uint32_t>(file_end - std::streampos(0)) - 8u);
    out_.seekp(file_end);
    out_.close();
    return out_.good();
  }

private:
  static std::array<uint8_t, 3> thermal_color(double t) {
    const double x = t * 4.0;
    const auto byte = [](double value) {
      return static_cast<uint8_t>(
          std::lround(std::clamp(value, 0.0, 1.0) * 255.0));
    };
    if (x < 1.0)
      return {0, byte(x), 255};
    if (x < 2.0)
      return {0, 255, byte(2.0 - x)};
    if (x < 3.0)
      return {byte(x - 2.0), 255, 0};
    return {255, byte(4.0 - x), 0};
  }

  void fourcc(const char *value) { out_.write(value, 4); }
  void u16(uint16_t value) {
    const char bytes[2] = {static_cast<char>(value),
                           static_cast<char>(value >> 8)};
    out_.write(bytes, sizeof(bytes));
  }
  void u32(uint32_t value) {
    const char bytes[4] = {
        static_cast<char>(value), static_cast<char>(value >> 8),
        static_cast<char>(value >> 16), static_cast<char>(value >> 24)};
    out_.write(bytes, sizeof(bytes));
  }
  void patch_u32(std::streampos position, uint32_t value) {
    const auto current = out_.tellp();
    out_.seekp(position);
    u32(value);
    out_.seekp(current);
  }

  std::ofstream out_;
  uint16_t width_{0};
  uint16_t height_{0};
  uint32_t row_bytes_{0};
  uint32_t frame_bytes_{0};
  uint32_t frames_{0};
  std::streampos riff_size_pos_{};
  std::streampos total_frames_pos_{};
  std::streampos stream_length_pos_{};
  std::streampos movi_size_pos_{};
  std::streampos movi_start_{};
  std::vector<uint8_t> row_;
  std::vector<uint32_t> index_offsets_;
};

static std::atomic<bool> g_recording{true};

static void on_signal(int) { g_recording.store(false); }

static bool write_y16_frame(std::ofstream &out, const Frame &frame) {
  if constexpr (std::endian::native == std::endian::little) {
    out.write(reinterpret_cast<const char *>(frame.pixels.data()),
              static_cast<std::streamsize>(frame.pixels.size() *
                                           sizeof(uint16_t)));
  } else {
    for (const uint16_t pixel : frame.pixels) {
      const char bytes[2] = {static_cast<char>(pixel & 0xff),
                             static_cast<char>(pixel >> 8)};
      out.write(bytes, sizeof(bytes));
    }
  }
  return out.good();
}

int main(int argc, char **argv) {
  Args args;
  try {
    args = parse_args(argc, argv);
  } catch (const std::exception &e) {
    std::cerr << "Arg error: " << e.what() << "\n";
    return 1;
  }

  if (args.mode != "dummy" && args.mode != "pt3") {
    std::cerr << "Arg error: --mode must be dummy or pt3\n";
    return 1;
  }
  if (args.video_max_c <= args.video_min_c) {
    std::cerr << "Arg error: --video-max-c must be greater than "
                 "--video-min-c\n";
    return 1;
  }
  if (args.video_output.empty()) {
    std::filesystem::path video_path(args.output);
    video_path.replace_extension(".avi");
    args.video_output = video_path.string();
  }
  if (std::filesystem::path(args.output) ==
      std::filesystem::path(args.video_output)) {
    std::cerr << "Arg error: Y16 and AVI output paths must be different\n";
    return 1;
  }
#if !defined(_WIN32) && !defined(USE_LIBUVC)
  if (args.mode == "pt3") {
    std::cerr << "PT3 support is not included in this build. Install libuvc, "
                 "initialize the Git submodules, and rebuild.\n";
    return 1;
  }
#endif

  std::ofstream output(args.output, std::ios::binary | std::ios::trunc);
  if (!output) {
    std::cerr << "Cannot open output file: " << args.output << "\n";
    return 2;
  }

  auto src = std::make_unique<SourceMonitor>(
      args.mode, args.fps, args.fps_auto, args.assume_tlinear, args.scale,
      args.ffc_mode);

  if (!src->start()) {
    std::cerr << "Failed to start capture source\n";
    return 3;
  }

  log(LogLevel::INFO,
      std::string("Recording to ") + args.output + " mode=" + args.mode +
          (args.fps_auto ? " fps=auto" : (" fps=" + std::to_string(args.fps))) +
          " assume_tlinear=" + yesno(args.assume_tlinear) +
          " scale=" + std::to_string(args.scale));

  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  using namespace std::chrono;
  const auto started = steady_clock::now();
  uint32_t last_id = UINT32_MAX;
  uint64_t frames_written = 0;
  uint16_t width = 0;
  uint16_t height = 0;
  bool write_ok = true;
  AviWriter video;
  bool video_open = false;

  while (g_recording.load()) {
    if (args.frame_limit > 0 && frames_written >= args.frame_limit)
      break;
    if (args.frame_limit == 0 && args.duration_seconds > 0.0 &&
        duration<double>(steady_clock::now() - started).count() >=
            args.duration_seconds)
      break;

    auto frame = src->latest();
    if (!frame || frame->hdr.frame_id == last_id) {
      std::this_thread::sleep_for(1ms);
      continue;
    }
    last_id = frame->hdr.frame_id;

    if (frames_written == 0) {
      width = frame->hdr.width;
      height = frame->hdr.height;
      video_open =
          video.open(args.video_output, width, height, args.fps);
      if (!video_open) {
        log(LogLevel::ERROR,
            "Cannot create RGB AVI output: " + args.video_output);
        write_ok = false;
        break;
      }
    } else if (frame->hdr.width != width || frame->hdr.height != height) {
      log(LogLevel::ERROR,
          "Frame dimensions changed while recording; stopping the file.");
      write_ok = false;
      break;
    }

    if (!write_y16_frame(output, *frame)) {
      log(LogLevel::ERROR, "Failed while writing output file.");
      write_ok = false;
      break;
    }
    if (!video.write_frame(*frame, args.video_min_c, args.video_max_c)) {
      log(LogLevel::ERROR,
          "Failed while converting a frame to the RGB AVI output.");
      write_ok = false;
      break;
    }
    ++frames_written;
  }

  src->stop();
  output.close();
  const bool video_ok = video_open && video.close();

  const double elapsed =
      duration<double>(steady_clock::now() - started).count();
  log(LogLevel::INFO,
      "Capture complete: frames=" + std::to_string(frames_written) +
          " size=" + std::to_string(width) + "x" + std::to_string(height) +
          " elapsed=" + std::to_string(elapsed) + "s output=" + args.output);
  if (video_ok) {
    log(LogLevel::INFO,
        "RGB video complete: output=" + args.video_output + " range=" +
            std::to_string(args.video_min_c) + ".." +
            std::to_string(args.video_max_c) + " C");
  }

  if (!write_ok || !output.good() || !video_ok)
    return 4;
  if (frames_written == 0) {
    log(LogLevel::ERROR, "No frames were captured.");
    return 5;
  }
  return 0;
}
