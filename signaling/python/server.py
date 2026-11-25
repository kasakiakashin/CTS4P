#!/usr/bin/env python3
"""cts4p signaling server - standard library only (Python 3.8+).

Same protocol as signaling/cloudflare/worker.js, for playing on a LAN, on a
machine with a public IP/port forward, or for local testing:

    python server.py                 # listens on 0.0.0.0:8787
    python server.py --port 9000 --host 127.0.0.1

Clients connect to  ws://<ip>:<port>/ws?room=CTS-ABCD&role=host|guest&name=Dani&proto=1
It only relays small JSON signaling messages; game traffic is peer-to-peer.
"""
import argparse
import asyncio
import base64
import hashlib
import json
import logging
import re
import secrets
import struct
from urllib.parse import parse_qs, urlsplit

ROOM_RE = re.compile(r"^CTS-[A-HJ-NP-Z2-9]{4}$")
MAX_GUESTS = 7
MAX_MESSAGE = 64 * 1024
MAX_NAME = 40
WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

log = logging.getLogger("cts4p-signal")


class Peer:
    def __init__(self, reader, writer, room, role, name):
        self.reader = reader
        self.writer = writer
        self.room = room
        self.role = role
        self.name = name
        self.id = secrets.token_hex(5)
        self.closed = False

    async def send_json(self, obj):
        await self.send_frame(0x1, json.dumps(obj, ensure_ascii=False).encode("utf-8"))

    async def send_frame(self, opcode, payload=b""):
        if self.closed:
            return
        header = bytes([0x80 | opcode])
        n = len(payload)
        if n < 126:
            header += bytes([n])
        elif n < 1 << 16:
            header += bytes([126]) + struct.pack(">H", n)
        else:
            header += bytes([127]) + struct.pack(">Q", n)
        try:
            self.writer.write(header + payload)
            await self.writer.drain()
        except (ConnectionError, RuntimeError):
            self.closed = True

    async def close(self, code=1000, reason=""):
        if self.closed:
            return
        await self.send_frame(0x8, struct.pack(">H", code) + reason.encode()[:120])
        self.closed = True
        try:
            self.writer.close()
        except Exception:  # noqa: BLE001 - best effort
            pass


rooms = {}  # room code -> list[Peer]


async def read_frame(reader):
    """Returns (opcode, payload). Handles masking and continuation frames."""
    message = b""
    first_opcode = None
    while True:
        b0, b1 = await reader.readexactly(2)
        fin, opcode = b0 & 0x80, b0 & 0x0F
        masked, n = b1 & 0x80, b1 & 0x7F
        if n == 126:
            (n,) = struct.unpack(">H", await reader.readexactly(2))
        elif n == 127:
            (n,) = struct.unpack(">Q", await reader.readexactly(8))
        if n > MAX_MESSAGE:
            raise ValueError("frame too large")
        mask = await reader.readexactly(4) if masked else b""
        data = await reader.readexactly(n)
        if masked:
            data = bytes(c ^ mask[i % 4] for i, c in enumerate(data))
        if opcode >= 0x8:  # control frames are never fragmented
            return opcode, data
        if first_opcode is None:
            first_opcode = opcode
        message += data
        if len(message) > MAX_MESSAGE:
            raise ValueError("message too large")
        if fin:
            return first_opcode, message


async def handshake(reader, writer):
    request = await asyncio.wait_for(reader.readuntil(b"\r\n\r\n"), timeout=10)
    lines = request.decode("latin-1").split("\r\n")
    method, target, _ = lines[0].split(" ", 2)
    headers = {}
    for line in lines[1:]:
        if ":" in line:
            k, v = line.split(":", 1)
            headers[k.strip().lower()] = v.strip()
    url = urlsplit(target)
    if method != "GET":
        raise ValueError("bad method")
    if url.path in ("/", "/health") and "upgrade" not in headers:
        writer.write(b"HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 19\r\n\r\ncts4p signaling ok\n")
        await writer.drain()
        return None
    if url.path != "/ws" or headers.get("upgrade", "").lower() != "websocket" or "sec-websocket-key" not in headers:
        writer.write(b"HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n")
        await writer.drain()
        return None
    accept = base64.b64encode(hashlib.sha1((headers["sec-websocket-key"] + WS_GUID).encode()).digest()).decode()
    writer.write(
        (
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\nConnection: Upgrade\r\n"
            f"Sec-WebSocket-Accept: {accept}\r\n\r\n"
        ).encode()
    )
    await writer.drain()
    return {k: v[0] for k, v in parse_qs(url.query).items()}


async def handle(reader, writer):
    peer = None
    try:
        params = await handshake(reader, writer)
        if params is None:
            writer.close()
            return
        room = params.get("room", "").upper()
        role = params.get("role", "")
        name = params.get("name", "Jogador")[:MAX_NAME]
        peer = Peer(reader, writer, room, role, name)

        members = rooms.get(room, [])
        host = next((p for p in members if p.role == "host"), None)
        error = None
        if not ROOM_RE.match(room):
            error = ("bad_request", "invalid room code")
        elif role == "host" and host:
            error = ("room_taken", "room already has a host")
        elif role == "guest" and not host:
            error = ("no_host", "room not found")
        elif role == "guest" and sum(p.role == "guest" for p in members) >= MAX_GUESTS:
            error = ("room_full", "room is full")
        elif role not in ("host", "guest"):
            error = ("bad_request", "role must be host or guest")
        if error:
            await peer.send_json({"t": "error", "code": error[0], "message": error[1]})
            await peer.close(4000, error[0])
            return

        rooms.setdefault(room, []).append(peer)
        await peer.send_json({"t": "welcome", "id": peer.id, "room": room, "host": None if role == "host" else host.id})
        if role == "guest":
            await host.send_json({"t": "peer_joined", "id": peer.id, "name": name})
        log.info("%s joined %s as %s (%s)", peer.id, room, role, name)

        while True:
            opcode, payload = await read_frame(reader)
            if opcode == 0x8:
                break
            if opcode == 0x9:
                await peer.send_frame(0xA, payload)
                continue
            if opcode != 0x1:
                continue
            try:
                msg = json.loads(payload.decode("utf-8"))
            except (UnicodeDecodeError, json.JSONDecodeError):
                continue
            if not isinstance(msg, dict) or msg.get("t") != "signal" or not isinstance(msg.get("to"), str):
                continue
            target = next((p for p in rooms.get(room, []) if p.id == msg["to"]), None)
            if target is None or (peer.role == "guest" and target.role != "host"):
                continue
            await target.send_json({"t": "signal", "from": peer.id, "data": msg.get("data")})
    except (asyncio.IncompleteReadError, ConnectionError, asyncio.TimeoutError, ValueError) as exc:
        log.debug("connection ended: %r", exc)
    finally:
        if peer is not None:
            await leave(peer)


async def leave(peer):
    members = rooms.get(peer.room)
    if not members or peer not in members:
        await peer.close()
        return
    members.remove(peer)
    log.info("%s left %s", peer.id, peer.room)
    for other in list(members):
        if peer.role == "host":
            await other.send_json({"t": "room_closed"})
            await other.close(4002, "room_closed")
        elif other.role == "host":
            await other.send_json({"t": "peer_left", "id": peer.id})
    if peer.role == "host" or not members:
        rooms.pop(peer.room, None)
    await peer.close()


async def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=8787)
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()
    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO, format="%(asctime)s %(message)s")
    server = await asyncio.start_server(handle, args.host, args.port)
    log.info("cts4p signaling listening on ws://%s:%d/ws", args.host, args.port)
    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
