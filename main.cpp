#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/endian/conversion.hpp>

#include <atomic>
#include <algorithm>
#include <chrono>
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
#include <fstream>

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace ws = beast::websocket;
using tcp = asio::ip::tcp;

static constexpr uint16_t kFormatUint16TLinear = 1;

#pragma pack(push, 1)
struct FrameHeader {
  char magic[4];         // "L3R1"
  uint16_t version;      // 1
  uint16_t header_bytes; // 32
  uint16_t width;        // e.g. 160
  uint16_t height;       // e.g. 120
  uint16_t format;       // 1 = UINT16_TLINEAR
  uint16_t scale;        // 100 => value/100 Kelvin
  uint16_t reserved;     // 0
  uint64_t timestamp_us; // monotonic
  uint32_t frame_id;     // incrementing
  uint16_t reserved2;    // padding to keep header 32 bytes
};
static_assert(sizeof(FrameHeader) == 32);
#pragma pack(pop)

struct Frame {
  FrameHeader hdr{};
  std::vector<uint16_t> pixels; // width*height
};

static uint64_t now_us() {
  using namespace std::chrono;
  auto t = steady_clock::now().time_since_epoch();
  return (uint64_t)duration_cast<microseconds>(t).count();
}

// --- Minimal logger ---
enum class LogLevel { INFO, WARN, ERROR };

static std::mutex g_log_mu;

static void log(LogLevel lv, const std::string &msg) {
  using namespace std::chrono;
  const auto tp = system_clock::now();
  const auto t = system_clock::to_time_t(tp);
  const auto us = duration_cast<microseconds>(tp.time_since_epoch()) % seconds(1);
  char buf[64];
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d.%06d",
                tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                tm.tm_hour, tm.tm_min, tm.tm_sec, (int)us.count());
  const char *lvl = lv == LogLevel::INFO ? "INFO" : (lv == LogLevel::WARN ? "WARN" : "ERROR");
  std::lock_guard<std::mutex> lk(g_log_mu);
  std::clog << "[" << buf << "] [" << lvl << "] " << msg << "\n";
}

class IFrameSource {
public:
  virtual ~IFrameSource() = default;
  virtual bool start() = 0;
  virtual void stop() = 0;
  // latest frame (thread-safe copy)
  virtual std::optional<Frame> latest() = 0;
  virtual uint16_t width() const = 0;
  virtual uint16_t height() const = 0;
};

class DummySource : public IFrameSource {
public:
  DummySource(uint16_t w = 160, uint16_t h = 120, double fps = 9.0)
      : w_(w), h_(h), fps_(fps) {}

  bool start() override {
    running_.store(true);
    th_ = std::thread([this] { run(); });
    return true;
  }

  void stop() override {
    running_.store(false);
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

    // ダミー温度：背景 295.00K、移動するホットスポット 310.00K
    // 以前はホットスポットが“ベタ塗り”だったが、より滑らかに見えるよう
    // ここでガウシアン勾配に変更する。
    const uint16_t bgK = 29500;
    const uint16_t hotK = 31000;

    while (running_.load()) {
      next += std::chrono::duration_cast<std::chrono::steady_clock::duration>(
          period);

      Frame f;
      std::memcpy(f.hdr.magic, "L3R1", 4);
      f.hdr.version = 1;
      f.hdr.header_bytes = sizeof(FrameHeader);
      f.hdr.width = w_;
      f.hdr.height = h_;
      f.hdr.format = kFormatUint16TLinear;
      f.hdr.scale = 100;
      f.hdr.reserved = 0;
      f.hdr.timestamp_us = now_us();
      f.hdr.frame_id = fid++;

      f.pixels.assign((size_t)w_ * h_, bgK);

      // moving hotspot with smooth radial gradient (Gaussian)
      // 中心は 310.00K、背景は 295.00K。半径rを目安にシグマを決め、
      // 画素値 = bg + (hot-bg)*exp(-(d^2)/(2*sigma^2)) を適用する。
      const int cx = static_cast<int>(fid % w_);
      const int cy = static_cast<int>((fid / 2) % h_);
      const double r = 12.0;                 // 見た目のサイズ（px）
      const double sigma = r * 0.6;          // エッジのやわらかさ
      const double inv2sigma2 = 1.0 / (2.0 * sigma * sigma);
      const double amp = static_cast<double>(hotK - bgK);

      for (int y = 0; y < static_cast<int>(h_); ++y) {
        for (int x = 0; x < static_cast<int>(w_); ++x) {
          const double dx = static_cast<double>(x - cx);
          const double dy = static_cast<double>(y - cy);
          const double d2 = dx * dx + dy * dy;
          const double w = std::exp(-d2 * inv2sigma2); // 0..1
          const double val = static_cast<double>(bgK) + amp * w;
          // Kelvin*100 の範囲に丸める
          uint16_t k = static_cast<uint16_t>(std::lround(std::clamp(val, (double)0.0, (double)65535.0)));
          f.pixels[(size_t)y * w_ + x] = k;
        }
      }

      {
        std::lock_guard<std::mutex> lk(mu_);
        latest_ = std::move(f);
      }

      std::this_thread::sleep_until(next);
    }
  }

  uint16_t w_, h_;
  double fps_;
  std::atomic<bool> running_{false};
  std::thread th_;
  std::mutex mu_;
  std::optional<Frame> latest_;
};

#ifdef USE_LIBUVC
#include <libuvc/libuvc.h>

class PT3Source : public IFrameSource {
public:
  PT3Source(double fps = 9.0, bool fps_auto = false) : fps_(fps), fps_auto_(fps_auto) {}

  bool start() override {
    if (running_.exchange(true)) return true;

    uvc_error_t res = uvc_init(&ctx_, nullptr);
    if (res != UVC_SUCCESS) {
      std::cerr << "uvc_init failed: " << uvc_strerror(res) << "\n";
      running_ = false;
      return false;
    }

    // Prefer PureThermal devices (GroupGets VID 0x1e4e), like GetThermal does.
    // Try vendor filter first; fall back to any UVC if not found.
    constexpr int kVidGroupGets = 0x1e4e;
    res = uvc_find_device(ctx_, &dev_, kVidGroupGets, 0, nullptr);
    if (res != UVC_SUCCESS) {
      // fallback: first UVC
      res = uvc_find_device(ctx_, &dev_, 0, 0, nullptr);
    }
    if (res != UVC_SUCCESS) {
      std::cerr << "uvc_find_device failed: " << uvc_strerror(res) << "\n";
      cleanup();
      return false;
    }

    res = uvc_open(dev_, &devh_);
    if (res != UVC_SUCCESS) {
      std::cerr << "uvc_open failed: " << uvc_strerror(res) << "\n";
      cleanup();
      return false;
    }

    // Log the chosen device (helpful to verify we didn't bind to a webcam)
    if (dev_) {
      uvc_device_descriptor_t* desc = nullptr;
      if (uvc_get_device_descriptor(dev_, &desc) == UVC_SUCCESS && desc) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "Opened device VID:PID = %04x:%04x", desc->idVendor, desc->idProduct);
        log(LogLevel::INFO, buf);
        uvc_free_device_descriptor(desc);
      }
    }

    // Negotiate stream. Prefer Y16/GRAY16 at 160x120 or 80x60.
    uvc_stream_ctrl_t ctrl{};

    auto try_fmt = [&](uvc_frame_format fmt, int w, int h, int fps) -> bool {
      uvc_error_t r = uvc_get_stream_ctrl_format_size(devh_, &ctrl, fmt, w, h, fps);
      return r == UVC_SUCCESS;
    };

    bool ok = false;
    int rfps = static_cast<int>(fps_ > 0 ? std::lround(fps_) : 9);

    // fps_auto_ の場合、フレーム記述子から最大fpsを推定して試す
    if (fps_auto_) {
      double best_fps = 0.0;
      int best_w = 0, best_h = 0;
      const uvc_format_desc_t* fmt = uvc_get_format_descs(devh_);
      for (const uvc_format_desc_t* f = fmt; f; f = f->next) {
        for (const uvc_frame_desc_t* fd = f->frame_descs; fd; fd = fd->next) {
          if (fd->wWidth <= 0 || fd->wHeight <= 0) continue;
          if (fd->intervals) {
            for (const uint32_t* p = fd->intervals; *p; ++p) {
              const double fps_val = (*p > 0) ? (1e7 / static_cast<double>(*p)) : 0.0; // 100ns単位
              if (fps_val > best_fps) {
                best_fps = fps_val;
                best_w = fd->wWidth;
                best_h = fd->wHeight;
              }
            }
          }
        }
      }
      if (best_fps > 0.0) {
        rfps = std::max(1, static_cast<int>(std::lround(best_fps)));
        // Try Y16/GRAY16 first for thermal data
        // Prefer Y16/GRAY16 for thermal
#ifdef UVC_FRAME_FORMAT_Y16
        if (!ok) ok = try_fmt(UVC_FRAME_FORMAT_Y16, best_w, best_h, rfps);
#endif
#ifdef UVC_FRAME_FORMAT_GRAY16
        if (!ok) ok = try_fmt(UVC_FRAME_FORMAT_GRAY16, best_w, best_h, rfps);
#endif
        // Fallback to ANY if neither exists
        if (!ok) ok = try_fmt(UVC_FRAME_FORMAT_ANY, best_w, best_h, rfps);
      }
    }

    // Fixed sizes preferred by Lepton/PureThermal
    // Try preferred sizes
#ifdef UVC_FRAME_FORMAT_Y16
    if (!ok) ok = try_fmt(UVC_FRAME_FORMAT_Y16, 160, 120, rfps);
    if (!ok) ok = try_fmt(UVC_FRAME_FORMAT_Y16, 80, 60, rfps);
#endif
#ifdef UVC_FRAME_FORMAT_GRAY16
    if (!ok) ok = try_fmt(UVC_FRAME_FORMAT_GRAY16, 160, 120, rfps);
    if (!ok) ok = try_fmt(UVC_FRAME_FORMAT_GRAY16, 80, 60, rfps);
#endif
    if (!ok) ok = try_fmt(UVC_FRAME_FORMAT_ANY, 160, 120, rfps);
    if (!ok) ok = try_fmt(UVC_FRAME_FORMAT_ANY, 80, 60, rfps);

    if (!ok) {
      // Enumerate advertised frame sizes and pick the first Y16/GRAY16 that works.
      const uvc_format_desc_t* fmt = uvc_get_format_descs(devh_);
      for (const uvc_format_desc_t* f = fmt; f && !ok; f = f->next) {
        for (const uvc_frame_desc_t* fd = f->frame_descs; fd && !ok; fd = fd->next) {
          int w = fd->wWidth;
          int h = fd->wHeight;
          if (w <= 0 || h <= 0) continue;
          // Try Y16 then GRAY16; avoid ANY to prevent mismatched formats
#ifdef UVC_FRAME_FORMAT_Y16
          if (!ok && try_fmt(UVC_FRAME_FORMAT_Y16, w, h, rfps)) ok = true;
#endif
#ifdef UVC_FRAME_FORMAT_GRAY16
          if (!ok && try_fmt(UVC_FRAME_FORMAT_GRAY16, w, h, rfps)) ok = true;
#endif
          if (!ok && try_fmt(UVC_FRAME_FORMAT_ANY, w, h, rfps)) ok = true;
        }
      }
    }

    if (!ok) {
      std::cerr << "uvc_get_stream_ctrl_format_size failed for all tried modes\n";
      uvc_print_diag(devh_, stderr);
      cleanup();
      return false;
    }

    // Optional: status callback (disconnect/streamoff notifications)
    uvc_set_status_callback(devh_, &PT3Source::on_status_static, this);

    // Start streaming; callback will fill latest_.
    res = uvc_start_streaming(devh_, &ctrl, &PT3Source::on_frame_static, this, 0);
    if (res != UVC_SUCCESS) {
      std::cerr << "uvc_start_streaming failed: " << uvc_strerror(res) << "\n";
      cleanup();
      return false;
    }

    // Width/height aren’t exposed in uvc_stream_ctrl_t across libuvc versions.
    // Keep defaults here; we’ll record actual dimensions from frames in on_frame().

    return true;
  }

  void stop() override {
    if (!running_.exchange(false)) return;
    if (devh_) uvc_stop_streaming(devh_);
    cleanup();
  }

  std::optional<Frame> latest() override {
    std::lock_guard<std::mutex> lk(mu_);
    return latest_;
  }
  uint16_t width() const override { return negotiated_w_; }
  uint16_t height() const override { return negotiated_h_; }

private:
  static void on_frame_static(uvc_frame_t* frame, void* user) {
    static_cast<PT3Source*>(user)->on_frame(frame);
  }
  static void on_status_static(uvc_status_class status_class, int event, int selector,
                               uvc_status_attribute status_attribute, void* data, size_t data_len,
                               void* user) {
    static_cast<PT3Source*>(user)->on_status(status_class, event, selector, status_attribute, data, data_len);
  }

  void on_frame(uvc_frame_t* frame) {
    if (!frame || frame->data_bytes == 0) return;

    // Expect 16-bit grayscale (Y16) little-endian from PT3/Lepton.
    const uint16_t w = static_cast<uint16_t>(frame->width);
    const uint16_t h = static_cast<uint16_t>(frame->height);
    const size_t npx = static_cast<size_t>(w) * h;
    if (frame->data_bytes < npx * 2) return; // not a full frame

    Frame f;
    std::memcpy(f.hdr.magic, "L3R1", 4);
    f.hdr.version = 1;
    f.hdr.header_bytes = sizeof(FrameHeader);
    f.hdr.width = w;
    f.hdr.height = h;
    f.hdr.format = kFormatUint16TLinear;
    f.hdr.scale = 100;
    f.hdr.reserved = 0;
    f.hdr.timestamp_us = now_us();
    f.hdr.frame_id = ++frame_id_;
    f.hdr.reserved2 = 0;

    f.pixels.resize(npx);
    // Copy as little-endian uint16
    std::memcpy(f.pixels.data(), frame->data, npx * sizeof(uint16_t));

    {
      std::lock_guard<std::mutex> lk(mu_);
      negotiated_w_ = w;
      negotiated_h_ = h;
      latest_ = std::move(f);
    }
  }

  void on_status(uvc_status_class status_class, int event, int selector,
                 uvc_status_attribute attr, void*, size_t) {
    // Log notable events to aid diagnosis; SourceMonitor handles reconnect.
    // Common cases: UVC_STATUS_CLASS_CONTROL with STREAMING_INTERFACE status.
    (void)selector; (void)attr;
    log(LogLevel::WARN, std::string("UVC status event class=") + std::to_string((int)status_class) +
                         " event=" + std::to_string(event));
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
  bool fps_auto_{};
  std::atomic<bool> running_{false};
  std::mutex mu_;
  std::optional<Frame> latest_;

  // libuvc handles
  uvc_context_t* ctx_{};
  uvc_device_t* dev_{};
  uvc_device_handle_t* devh_{};

  // Negotiated frame size
  uint16_t negotiated_w_{160};
  uint16_t negotiated_h_{120};
  uint32_t frame_id_{0};
};
#endif

// --- WebSocket session ---
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

  // 最新優先：クライアントが詰まってたら古いのを捨てる（Session側でキュー制限）
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
  // クライアントからのメッセージは無視（必要なら制御用に利用可）
  rbuf_.consume(rbuf_.size());
  do_read();
}

void Session::send(std::shared_ptr<std::vector<uint8_t>> msg) {
  std::lock_guard<std::mutex> lk(wmu_);

  // キュー制限：送信が詰まってたら古いフレームを捨てる（最新優先）
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

// --- Listener ---
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
          if (!ec)
            std::make_shared<Session>(std::move(sock), self->hub_)->start();
          self->do_accept();
        });
  }

  tcp::acceptor acceptor_;
  Hub &hub_;
};

// --- Frame packer ---
static std::shared_ptr<std::vector<uint8_t>> pack_frame(const Frame &f) {
  auto msg = std::make_shared<std::vector<uint8_t>>();
  const size_t npx = f.pixels.size();
  msg->resize(sizeof(FrameHeader) + npx * sizeof(uint16_t));
  std::memcpy(msg->data(), &f.hdr, sizeof(FrameHeader));
  std::memcpy(msg->data() + sizeof(FrameHeader), f.pixels.data(),
              npx * sizeof(uint16_t));
  return msg;
}

// --- Arg parsing ---
struct Args {
  std::string mode = "dummy"; // dummy | pt3
  uint16_t port = 8765;
  double fps = 9.0;            // 固定fps（>0）
  bool fps_auto = true;        // 既定でソース（デバイス上限）に追従
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
      a.port = (uint16_t)std::stoi(v);
    } else if (s == "--fps") {
      std::string v;
      next(v);
      if (v == "auto" || v == "max") {
        a.fps_auto = true;
      } else {
        a.fps = std::stod(v);
        a.fps_auto = false; // 数値指定時は固定fps
      }
    } else if (s == "--help" || s == "-h") {
      std::cout << "Usage: lepton_ws_server [--mode dummy|pt3] [--port 8765] "
                   "[--fps auto|NUM] (default: auto)\n"
                   "Binary frame protocol: 32-byte header + uint16 pixels "
                   "(little-endian)\n";
      std::exit(0);
    }
  }
  return a;
}

// --- Source monitor with auto-reconnect ---
class SourceMonitor : public IFrameSource {
public:
  SourceMonitor(std::string mode, double fps, bool fps_auto)
      : mode_(std::move(mode)), fps_(fps), fps_auto_(fps_auto) {}

  bool start() override {
    if (running_.exchange(true)) return true;
    th_ = std::thread([this]{ loop(); });
    return true; // 起動時にデバイスが無くても失敗にしない
  }

  void stop() override {
    if (!running_.exchange(false)) return;
    if (th_.joinable()) th_.join();
    std::unique_ptr<IFrameSource> old;
    {
      std::lock_guard<std::mutex> lk(mu_);
      old = std::move(cur_);
    }
    if (old) old->stop();
  }

  std::optional<Frame> latest() override {
    std::lock_guard<std::mutex> lk(mu_);
    if (cur_) return cur_->latest();
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
      return std::make_unique<DummySource>(160, 120, fps_);
    } else if (mode_ == "pt3") {
#ifdef USE_LIBUVC
      return std::make_unique<PT3Source>(fps_, fps_auto_);
#else
      // ビルド時にUVCが無効なら何もしない
      return nullptr;
#endif
    }
    return nullptr;
  }

  void loop() {
    using namespace std::chrono;
    // ベースのリトライ間隔と最大バックオフ
    const auto retry_interval_base = milliseconds(800);
    const auto retry_interval_max  = seconds(8);

    // ストール検出しきい値（fps_auto 時はやや長め）
    const auto stall_timeout = duration<double>(
        fps_auto_ ? 4.0 : std::max(3.0, 3.0 / std::max(1.0, fps_))
    ); // おおよそ3フレーム + マージン
    auto last_log_status = steady_clock::now();

    uint32_t last_frame_id = UINT32_MAX;
    auto last_frame_tp = steady_clock::now();

    // 連続失敗でバックオフを伸ばす
    int consecutive_failures = 0;
    // 連続でストール判定した回数（ノイズ抑制用）
    int consecutive_stalls = 0;

    while (running_.load()) {
      // 確実に現在のソースを保持
      std::unique_ptr<IFrameSource> local;
      {
        std::lock_guard<std::mutex> lk(mu_);
        if (cur_) local.reset(cur_.release()); // 移動してロック外で利用
      }

      if (!local) {
        // まだ接続されていない → 作成して接続試行
        auto candidate = make_source();
        if (!candidate) {
          log(LogLevel::WARN, "No source available for mode='" + mode_ + "'. Retrying...");
          // バックオフ
          {
            const int exp = 1 << std::min(consecutive_failures, 4);
            auto backoff_ms = std::min(duration_cast<milliseconds>(retry_interval_max),
                                       retry_interval_base * exp);
            std::this_thread::sleep_for(backoff_ms);
          }
          continue;
        }
        log(LogLevel::INFO, "Probing device (mode=" + mode_ + ")...");
        if (candidate->start()) {
          // 接続直後にフレームが流れてくるかを短時間確認（ウォームアップ）
          const auto warmup_deadline = steady_clock::now() + seconds(2);
          uint32_t warmup_last_id = UINT32_MAX;
          bool warmup_ok = false;
          while (steady_clock::now() < warmup_deadline) {
            auto opt = candidate->latest();
            if (opt) {
              if (warmup_last_id == UINT32_MAX) warmup_last_id = opt->hdr.frame_id;
              if (opt->hdr.frame_id != warmup_last_id) { warmup_ok = true; break; }
            }
            std::this_thread::sleep_for(milliseconds(50));
          }

          if (!warmup_ok && mode_ != "dummy") {
            log(LogLevel::WARN, "No frames during warmup; closing and retrying.");
            candidate->stop();
            consecutive_failures = std::min(consecutive_failures + 1, 16);
            const int exp = 1 << std::min(consecutive_failures, 4);
            auto backoff_ms = std::min(duration_cast<milliseconds>(retry_interval_max),
                                       retry_interval_base * exp);
            std::this_thread::sleep_for(backoff_ms);
            continue; // 再試行
          }

          // ウォームアップ通過 → 採用
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
          {
            const int exp = 1 << std::min(consecutive_failures, 4);
            auto backoff_ms = std::min(duration_cast<milliseconds>(retry_interval_max),
                                       retry_interval_base * exp);
            std::this_thread::sleep_for(backoff_ms);
          }
        }
      } else {
        // 稼働中の監視
        bool keep = true;
        // フレームの進み具合を確認
        auto opt = local->latest();
        if (opt) {
          // 更新
          if (opt->hdr.frame_id != last_frame_id) {
            last_frame_id = opt->hdr.frame_id;
            last_frame_tp = steady_clock::now();
            // 幅高さを記録
            last_w_ = opt->hdr.width;
            last_h_ = opt->hdr.height;
            consecutive_stalls = 0; // 新規フレームでリセット
          }
        }

        const auto now = steady_clock::now();
        if (now - last_frame_tp > stall_timeout && mode_ != "dummy") {
          // 一度の閾値越えでは切断せず、連続で検知したら再起動
          ++consecutive_stalls;
          if (consecutive_stalls >= 2) { // 少なくとも2回連続でストール
            log(LogLevel::WARN, "No frames received recently; restarting stream...");
            keep = false;
          }
        } else {
          // 回復したらカウンタを戻す
          consecutive_stalls = 0;
        }

        if (!keep) {
          local->stop();
          // 破棄して再接続へ
          log(LogLevel::INFO, "Device disconnected. Will try to reconnect.");
          consecutive_failures = std::min(consecutive_failures + 1, 16);
          // 共有スロットを空にしてバックオフ後に再試行
          {
            std::lock_guard<std::mutex> lk(mu_);
            // drop
          }
          {
            const int exp = 1 << std::min(consecutive_failures, 4);
            auto backoff_ms = std::min(duration_cast<milliseconds>(retry_interval_max),
                                       retry_interval_base * exp);
            std::this_thread::sleep_for(backoff_ms);
          }
          // ドロップ
        } else {
          // 状態ログ（5秒毎程度）
          if (now - last_log_status > seconds(5)) {
            const auto age_ms = duration_cast<milliseconds>(now - last_frame_tp).count();
            log(LogLevel::INFO, "Streaming OK. last_frame_age_ms=" + std::to_string((long long)age_ms));
            last_log_status = now;
          }
        }

        // 共有スロットへ返す or 破棄
        {
          std::lock_guard<std::mutex> lk(mu_);
          if (keep) cur_.reset(local.release()); // 返す
          // keep==false の場合はlocalはこのスコープを抜けて破棄される
        }

        std::this_thread::sleep_for(milliseconds(100));
      }
    }
  }

  std::string mode_;
  double fps_;
  bool fps_auto_{};
  std::atomic<bool> running_{false};
  std::thread th_;
  mutable std::mutex mu_;
  std::unique_ptr<IFrameSource> cur_;
  uint16_t last_w_{0}, last_h_{0};
};

int main(int argc, char **argv) {
  Args args;
  try {
    args = parse_args(argc, argv);
  } catch (const std::exception &e) {
    std::cerr << "Arg error: " << e.what() << "\n";
    return 1;
  }

  std::unique_ptr<IFrameSource> src;
  // 監視付きソース（初期未接続でも継続運転）
  src = std::make_unique<SourceMonitor>(args.mode, args.fps, args.fps_auto);
  (void)src->start();

  asio::io_context ioc{1};
  Hub hub;

  auto listener = std::make_shared<Listener>(
      ioc, tcp::endpoint{asio::ip::make_address("127.0.0.1"), args.port}, hub);
  listener->run();

  std::atomic<bool> running{true};

  // broadcast loop: 最新フレームを配信（固定fps or 自動）
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
      // 自動: 新しいフレームが来たら即送る（短いポーリングでスピン回避）
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

  log(LogLevel::INFO, std::string("WebSocket server on ws://127.0.0.1:") +
                         std::to_string(args.port) + " mode=" + args.mode +
                         (args.fps_auto ? " fps=auto" : (" fps=" + std::to_string(args.fps))));

  ioc.run();

  running.store(false);
  broadcaster.join();
  src->stop();
  return 0;
}
