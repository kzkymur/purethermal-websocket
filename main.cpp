#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/endian/conversion.hpp>

#include <atomic>
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

      // moving hotspot
      int cx = (int)(fid % w_);
      int cy = (int)((fid / 2) % h_);
      int r = 10;

      for (int y = 0; y < (int)h_; ++y) {
        for (int x = 0; x < (int)w_; ++x) {
          int dx = x - cx, dy = y - cy;
          if (dx * dx + dy * dy <= r * r) {
            f.pixels[(size_t)y * w_ + x] = hotK;
          }
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
  PT3Source(double fps = 9.0) : fps_(fps) {}

  bool start() override {
    if (running_.exchange(true)) return true;

    uvc_error_t res = uvc_init(&ctx_, nullptr);
    if (res != UVC_SUCCESS) {
      std::cerr << "uvc_init failed: " << uvc_strerror(res) << "\n";
      running_ = false;
      return false;
    }

    // Find the first UVC device (you can specialize with VID/PID later)
    res = uvc_find_device(ctx_, &dev_, 0, 0, nullptr);
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

    // Negotiate stream. Prefer 160x120 or 80x60. Fallback: enumerate sizes.
    uvc_stream_ctrl_t ctrl{};

    auto try_any = [&](int w, int h, int fps) -> bool {
      uvc_error_t r = uvc_get_stream_ctrl_format_size(
          devh_, &ctrl, UVC_FRAME_FORMAT_ANY, w, h, fps);
      return r == UVC_SUCCESS;
    };

    bool ok = false;
    const int rfps = static_cast<int>(fps_ > 0 ? std::lround(fps_) : 9);
    if (!ok) ok = try_any(160, 120, rfps);
    if (!ok) ok = try_any(80, 60, rfps);

#ifdef UVC_FRAME_FORMAT_GRAY16
    if (!ok) {
      // Some libuvc builds expose explicit GRAY16; try that too.
      if (uvc_get_stream_ctrl_format_size(devh_, &ctrl, UVC_FRAME_FORMAT_GRAY16,
                                          160, 120, rfps) == UVC_SUCCESS)
        ok = true;
      else if (uvc_get_stream_ctrl_format_size(devh_, &ctrl, UVC_FRAME_FORMAT_GRAY16,
                                               80, 60, rfps) == UVC_SUCCESS)
        ok = true;
    }
#endif

    if (!ok) {
      // Enumerate advertised frame sizes and pick the first that works.
      const uvc_format_desc_t* fmt = uvc_get_format_descs(devh_);
      for (const uvc_format_desc_t* f = fmt; f && !ok; f = f->next) {
        for (const uvc_frame_desc_t* fd = f->frame_descs; fd && !ok; fd = fd->next) {
          int w = fd->wWidth;
          int h = fd->wHeight;
          if (w <= 0 || h <= 0) continue;
          if (try_any(w, h, rfps)) ok = true;
        }
      }
    }

    if (!ok) {
      std::cerr << "uvc_get_stream_ctrl_format_size failed for all tried modes\n";
      uvc_print_diag(devh_, stderr);
      cleanup();
      return false;
    }

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
      latest_ = std::move(f);
    }
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
  double fps = 9.0;
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
      a.fps = std::stod(v);
    } else if (s == "--help" || s == "-h") {
      std::cout << "Usage: lepton_ws_server [--mode dummy|pt3] [--port 8765] "
                   "[--fps 9]\n"
                   "Binary frame protocol: 32-byte header + uint16 pixels "
                   "(little-endian)\n";
      std::exit(0);
    }
  }
  return a;
}

int main(int argc, char **argv) {
  Args args;
  try {
    args = parse_args(argc, argv);
  } catch (const std::exception &e) {
    std::cerr << "Arg error: " << e.what() << "\n";
    return 1;
  }

  std::unique_ptr<IFrameSource> src;

  if (args.mode == "dummy") {
    src = std::make_unique<DummySource>(160, 120, args.fps);
  } else if (args.mode == "pt3") {
#ifdef USE_LIBUVC
    src = std::make_unique<PT3Source>(args.fps);
#else
    std::cerr << "pt3 mode requires -DUSE_LIBUVC=ON at build time.\n";
    return 1;
#endif
  } else {
    std::cerr << "Unknown mode: " << args.mode << "\n";
    return 1;
  }

  if (!src->start()) {
    std::cerr << "Failed to start source.\n";
    return 1;
  }

  asio::io_context ioc{1};
  Hub hub;

  auto listener = std::make_shared<Listener>(
      ioc, tcp::endpoint{asio::ip::make_address("127.0.0.1"), args.port}, hub);
  listener->run();

  std::atomic<bool> running{true};

  // broadcast loop: 最新フレームを一定周期で配信
  std::thread broadcaster([&] {
    uint32_t last_id = UINT32_MAX;
    const auto period = std::chrono::duration<double>(1.0 / args.fps);
    auto next = std::chrono::steady_clock::now();

    while (running.load()) {
      next += std::chrono::duration_cast<std::chrono::steady_clock::duration>(
          period);

      auto opt = src->latest();
      if (opt && opt->hdr.frame_id != last_id) {
        last_id = opt->hdr.frame_id;
        hub.broadcast(pack_frame(*opt));
      }

      std::this_thread::sleep_until(next);
    }
  });

  std::cout << "WebSocket server on ws://127.0.0.1:" << args.port
            << " mode=" << args.mode << " fps=" << args.fps << "\n";

  ioc.run();

  running.store(false);
  broadcaster.join();
  src->stop();
  return 0;
}
