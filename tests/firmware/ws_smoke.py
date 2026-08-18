#!/usr/bin/env python3
"""
Board smoke test for the WebSocket /ws endpoint at splatoon.local.

Asserts that the firmware's WS dispatcher covers the full serial
protocol surface (HELLO/INFO/STATUS/PING/STOP/STREAM/STREAM_END/
START*/SCRIPT/R-frame) instead of treating every command as a raw
R frame (the regression the firmware had pre-fix).

Usage:
    tests/firmware/ws_smoke.py
    tests/firmware/ws_smoke.py --host 192.168.8.118

Exit code:
    0 if every expected reply matches; 1 otherwise.
"""
import argparse
import base64
import json
import os
import socket
import struct
import sys
import time


def ws_exchange(host: str, port: int, path: str, payload: str,
                timeout: float = 5.0) -> str:
    """Open a single-shot WS handshake, send payload, read one text frame."""
    s = socket.create_connection((host, port), timeout=timeout)
    key = base64.b64encode(os.urandom(16)).decode()
    req = (
        f"GET {path} HTTP/1.1\r\n"
        f"Host: {host}:{port}\r\n"
        f"Upgrade: websocket\r\n"
        f"Connection: Upgrade\r\n"
        f"Sec-WebSocket-Key: {key}\r\n"
        f"Sec-WebSocket-Version: 13\r\n"
        f"\r\n"
    )
    s.sendall(req.encode())
    buf = b""
    while b"\r\n\r\n" not in buf:
        chunk = s.recv(4096)
        if not chunk:
            break
        buf += chunk
    head, _, rest = buf.partition(b"\r\n\r\n")
    if b"101" not in head:
        s.close()
        raise RuntimeError(f"WS handshake failed: {head.decode(errors='replace')}")
    data = payload.encode()
    mask = os.urandom(4)
    masked = bytes(b ^ mask[i % 4] for i, b in enumerate(data))
    frame = bytes([0x81, 0x80 | len(data)]) + mask + masked
    s.sendall(frame)
    hdr = s.recv(2)
    if len(hdr) < 2:
        s.close()
        return ""
    ln = hdr[1] & 0x7f
    if ln == 126:
        ln = struct.unpack("!H", s.recv(2))[0]
    elif ln == 127:
        ln = struct.unpack("!Q", s.recv(8))[0]
    body = b""
    s.settimeout(timeout)
    while len(body) < ln:
        chunk = s.recv(ln - len(body))
        if not chunk:
            break
        body += chunk
    s.close()
    return body.decode(errors="replace").strip()


def expect_ok(label: str, got: str, want: str) -> bool:
    ok = got == want
    flag = "OK " if ok else "FAIL"
    print(f"  {flag} {label:18s} got={got!r:36s} want={want!r}")
    return ok


def expect_json_field(label: str, got: str, field: str,
                      want: object) -> bool:
    try:
        parsed = json.loads(got)
    except json.JSONDecodeError:
        print(f"  FAIL {label:18s} not JSON: {got!r}")
        return False
    if not isinstance(parsed, dict) or field not in parsed:
        print(f"  FAIL {label:18s} no field {field!r} in {parsed!r}")
        return False
    got_field = parsed[field]
    ok = got_field == want
    flag = "OK " if ok else "FAIL"
    print(f"  {flag} {label:18s} {field}={got_field!r}")
    return ok


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="splatoon.local")
    parser.add_argument("--port", type=int, default=80)
    parser.add_argument("--path", default="/ws")
    args = parser.parse_args()

    results = []
    print(f"\n[ws-smoke] target = {args.host}:{args.port}{args.path}\n")

    # Probe 1: PING -> PONG (single token, no JSON)
    print("[probe] PING")
    results.append(expect_ok("PING -> PONG",
                             ws_exchange(args.host, args.port, args.path, "PING"),
                             "PONG"))

    # Probe 2: HELLO -> info JSON with type=info, ok=true, firmware set
    print("\n[probe] HELLO")
    reply = ws_exchange(args.host, args.port, args.path, "HELLO")
    results.append(expect_ok("HELLO is JSON",
                             "{" in reply and reply.endswith("}"),
                             True))
    if "{" in reply:
        parsed = json.loads(reply)
        results.append(expect_json_field("HELLO ok", reply, "ok", True))
        results.append(expect_json_field("HELLO type", reply, "type", "info"))
        results.append(expect_json_field("HELLO state", reply, "state", "idle"))

    # Probe 3: STATUS -> JSON with type=status
    print("\n[probe] STATUS")
    reply = ws_exchange(args.host, args.port, args.path, "STATUS")
    results.append(expect_json_field("STATUS type", reply, "type", "status"))

    # Probe 4: R-frame -> OK, AND it actually pushes Gamepad.write
    print("\n[probe] R frame")
    results.append(expect_ok("R 4 15 128 128 128 128 -> OK",
                             ws_exchange(args.host, args.port, args.path,
                                         "R 4 15 128 128 128 128"),
                             "OK"))

    # Probe 5: STREAM -> OK, then STREAM_END -> OK
    print("\n[probe] STREAM / STREAM_END")
    results.append(expect_ok("STREAM -> OK",
                             ws_exchange(args.host, args.port, args.path,
                                         "STREAM"),
                             "OK"))
    results.append(expect_ok("STREAM_END -> OK",
                             ws_exchange(args.host, args.port, args.path,
                                         "STREAM_END"),
                             "OK"))

    # Probe 6: SCRIPT -> {"script":"..."}
    print("\n[probe] SCRIPT")
    reply = ws_exchange(args.host, args.port, args.path, "SCRIPT")
    results.append(expect_ok("SCRIPT is script JSON",
                             '"script"' in reply and reply.startswith("{"),
                             True))

    # Probe 7: STOP while idle -> status JSON
    print("\n[probe] STOP")
    reply = ws_exchange(args.host, args.port, args.path, "STOP")
    results.append(expect_json_field("STOP type", reply, "type", "status"))
    results.append(expect_json_field("STOP state", reply, "state", "idle"))

    # Probe 8: garbage -> ERR (explicit error path)
    print("\n[probe] garbage")
    results.append(expect_ok("GARBAGE -> ERR",
                             ws_exchange(args.host, args.port, args.path,
                                         "NOT_A_REAL_CMD"),
                             "ERR"))

    failed = sum(1 for r in results if not r)
    print(f"\n[ws-smoke] {len(results) - failed}/{len(results)} passed")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
