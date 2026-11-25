"""IPC frame encoding shared with native/src/bridge/bridge.h.

Pure Python (no game imports) so it can be unit-tested outside the game.

Python -> native
    0x01 NET_SEND  u8 flags(bit0 reliable) | u8 msg_type | u8 target_len | target | payload
    0x02 REQUEST   UTF-8 JSON {"op": ...}
native -> Python
    0x81 NET_RECV  u8 msg_type | u8 from_len | from | payload
    0x82 EVENT     UTF-8 JSON {"ev": ...}
"""
import json
import struct

KIND_NET_SEND = 0x01
KIND_REQUEST = 0x02
KIND_NET_RECV = 0x81
KIND_EVENT = 0x82

GAMEPLAY_FIRST = 32
GAMEPLAY_LAST = 127


class NetRecv:
    __slots__ = ("msg_type", "sender", "payload")

    def __init__(self, msg_type, sender, payload):
        self.msg_type = msg_type
        self.sender = sender
        self.payload = payload

    def __repr__(self):
        return "<NetRecv type={} from={} {} bytes>".format(self.msg_type, self.sender, len(self.payload))


class Event:
    __slots__ = ("name", "data")

    def __init__(self, name, data):
        self.name = name
        self.data = data

    def __repr__(self):
        return "<Event {}>".format(self.name)


def encode_request(obj):
    return bytes([KIND_REQUEST]) + json.dumps(obj, separators=(",", ":"), ensure_ascii=False).encode("utf-8")


def encode_net_send(msg_type, payload, target="", reliable=True):
    if not GAMEPLAY_FIRST <= msg_type <= GAMEPLAY_LAST:
        raise ValueError("gameplay message types are 32..127, got {}".format(msg_type))
    t = target.encode("utf-8")
    if len(t) > 255:
        raise ValueError("target id too long")
    return struct.pack("BBBB", KIND_NET_SEND, 1 if reliable else 0, msg_type, len(t)) + t + bytes(payload)


def decode(frame):
    """Returns NetRecv, Event, or None for anything malformed."""
    if not frame:
        return None
    kind = frame[0]
    if kind == KIND_EVENT:
        try:
            data = json.loads(frame[1:].decode("utf-8"))
        except (UnicodeDecodeError, ValueError):
            return None
        if not isinstance(data, dict):
            return None
        return Event(data.get("ev", ""), data)
    if kind == KIND_NET_RECV:
        if len(frame) < 3:
            return None
        msg_type = frame[1]
        flen = frame[2]
        if len(frame) < 3 + flen:
            return None
        sender = frame[3:3 + flen].decode("utf-8", "replace")
        return NetRecv(msg_type, sender, bytes(frame[3 + flen:]))
    return None
