#!/usr/bin/env python3
"""Inspect or stop only this prototype's local QEMU instance."""
import argparse
import json
from pathlib import Path
import socket

REPO = Path(__file__).resolve().parents[1]


def execute(name, arguments=None):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
        sock.settimeout(10)
        sock.connect(str(REPO / "work/macos/qmp.sock"))
        stream = sock.makefile("rwb")
        json.loads(stream.readline())

        def request(command, args=None):
            payload = {"execute": command, "id": command}
            if args:
                payload["arguments"] = args
            stream.write(json.dumps(payload).encode() + b"\n")
            stream.flush()
            while True:
                line = stream.readline()
                if not line:
                    raise RuntimeError("QEMU disconnected before answering")
                reply = json.loads(line)
                if reply.get("id") == command:
                    if "error" in reply:
                        raise RuntimeError(reply["error"])
                    return reply.get("return")
        request("qmp_capabilities")
        return request(name, arguments)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("command", choices=["status", "quit", "screenshot"])
    parser.add_argument("path", nargs="?")
    args = parser.parse_args()
    try:
        if args.command == "screenshot":
            if not args.path:
                parser.error("screenshot requires a PNG file path")
            result = execute("screendump", {"filename": str(Path(args.path).resolve()), "format": "png"})
        else:
            result = execute("query-status" if args.command == "status" else "quit")
    except FileNotFoundError:
        raise SystemExit("The RX3 prototype is not running.") from None
    except (OSError, RuntimeError) as error:
        raise SystemExit(f"Prototype connection: {error}") from None
    print(json.dumps(result))
