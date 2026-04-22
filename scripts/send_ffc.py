#!/usr/bin/env python3
import argparse
import asyncio

import websockets


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description="Send an FFC trigger command to lepton_ws_server over WebSocket."
    )
    p.add_argument(
        "--url",
        default="ws://127.0.0.1:8765",
        help="WebSocket server URL (default: ws://127.0.0.1:8765)",
    )
    p.add_argument(
        "--binary",
        action="store_true",
        help="Send binary 0x01 instead of text 'ffc'.",
    )
    return p.parse_args()


async def main() -> None:
    args = parse_args()
    async with websockets.connect(args.url) as ws:
        if args.binary:
            await ws.send(b"\x01")
            print(f"sent binary FFC trigger (0x01) to {args.url}")
        else:
            await ws.send("ffc")
            print(f"sent text FFC trigger ('ffc') to {args.url}")


if __name__ == "__main__":
    asyncio.run(main())
