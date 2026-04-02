# purethermal-websocket

A minimal WebSocket server that captures 16‑bit grayscale (Y16) frames from UVC devices (e.g., PureThermal + FLIR Lepton) via libuvc and streams them to clients.

Wire format: a fixed 32‑byte header followed by pixel data as little‑endian `uint16` values (`width × height`). Clients should read the header first, then consume `width × height` `uint16` samples and interpret them as temperature/intensity; the scaling factor is provided in the header’s `scale` field. See `FrameHeader` in `main.cpp` for the exact layout.

Networking is implemented with Boost.Asio/Beast. Use `--mode pt3` for real hardware (libuvc) and `--mode dummy` for the synthetic frames.

## Build

```sh
cmake . & make
```

## Run

```sh
sudo ./lepton_ws_server --mode pt3
```

### Run (scale=10)

```sh
./lepton_ws_server --mode dummy --scale 10 --fps 9
```

### Dummy Mode

```sh
./lepton_ws_server --mode dummy
```

### Options

- `--mode dummy|pt3` (default: `dummy`)
- `--port <num>` (default: `8765`)
- `--fps auto|NUM` (default: `auto`)
- `--scale NUM` (default: `100`, `Kelvin = value / scale`)

## Client Decode Example (Python)

```python
import asyncio
import struct
import websockets

async def main():
    async with websockets.connect("ws://127.0.0.1:8765") as ws:
        buf = await ws.recv()
        hdr = struct.unpack("<4sHHHHHHHQLH", buf[:32])
        magic, version, header_bytes, width, height, fmt, scale, _, ts_us, frame_id, _ = hdr
        assert magic == b"L3R1" and header_bytes == 32
        assert version == 1
        pixels = struct.unpack("<" + "H" * (width * height), buf[32:32 + width * height * 2])
        kelvin0 = pixels[0] / scale
        print(version, width, height, scale, frame_id, kelvin0)

asyncio.run(main())
```
