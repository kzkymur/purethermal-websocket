# uvc-y16-websocket

A minimal WebSocket server that captures 16‑bit grayscale (Y16) frames from UVC devices (e.g., PureThermal + FLIR Lepton) via libuvc and streams them to clients.

Wire format: a fixed 32‑byte header followed by pixel data as little‑endian `uint16` values (`width × height`). Clients should read the header first, then consume `width × height` `uint16` samples and interpret them as temperature/intensity; the scaling factor is provided in the header’s `scale` field. See `FrameHeader` in `main.cpp` for the exact layout.

Networking is implemented with Boost.Asio/Beast. Use `--mode pt3` for real hardware (libuvc) and `--mode dummy` for the synthetic frames.

## Build

```sh
cmake . & make
```

### Run

```sh
sudo ./lepton_ws_server --mode pt3 --port 8765
```

#### Dummy Mode

```sh
./lepton_ws_server --mode dummy --port 8765
```

### Options

- `--mode dummy|pt3` (default: `dummy`)
- `--port <num>` (default: `8765`)
- `--fps auto|NUM` (default: `auto`)
