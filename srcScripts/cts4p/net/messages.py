"""Gameplay message types (32..127) carried opaquely by the native core.

Control messages are compact JSON; GAME_MSG carries the game's own protobuf
bytes (the same bytes the host's simulation sends to its own UI), prefixed by
the 32-bit message id. Payloads larger than one DataChannel message are split
into GAME_MSG_PART fragments and reassembled in order.

Pure Python (no game imports) so it can be unit-tested outside the game.
"""
import json
import struct

GUEST_HELLO = 32     # guest -> host  {client_id, name, in_world, zone_id, slot_id, generation}
HOST_WELCOME = 33    # host -> guest  {vclient_id, active_sim_id, zone_id}
GAME_MSG = 34        # host -> guest  u32 msg_id | protobuf bytes   (mirror of the host UI stream)
COMMAND = 35         # guest -> host  {name, args:[str]}             (forwarded console/UI command)
CLIENT_CHEAT = 36    # host -> guest  {cmd}                          (client-side command for the guest exe)
ZONE_CHANGE = 37     # host -> guest  {phase: leaving|arrived|save_coming, zone_id, generation}
CAS_COMMIT = 38      # guest -> host  {sim_id, fields}               (appearance edited in the guest's CAS)
RESYNC_REQUEST = 39  # guest -> host  {reason}
SAVE_INFO = 40       # host -> guest  {generation, slot_id, label, zone_id}
REJECT = 41          # host -> guest  {reason}
GAME_MSG_PART = 42   # host -> guest  u32 seq | u16 index | u16 count | chunk
LOT_HASH = 43        # host -> guest  {hash, zone_id, reason, summary}        (Lot State Hash)
PRESENCE = 44        # any -> any     {name, mode, x, y, z, level}           (where each player is acting)

MAX_PAYLOAD = 200 * 1024  # stays below the 256 KiB DataChannel message limit with headers

_GAME_HEADER = struct.Struct("<I")
_PART_HEADER = struct.Struct("<IHH")


def encode_json(obj):
    return json.dumps(obj, separators=(",", ":"), ensure_ascii=False).encode("utf-8")


def decode_json(payload):
    try:
        obj = json.loads(payload.decode("utf-8"))
    except (UnicodeDecodeError, ValueError):
        return None
    return obj if isinstance(obj, dict) else None


def encode_game_msg(msg_id, data, seq):
    """Returns a list of (msg_type, payload) ready to send, fragmenting if needed."""
    body = _GAME_HEADER.pack(msg_id) + bytes(data)
    if len(body) <= MAX_PAYLOAD:
        return [(GAME_MSG, body)]
    chunk = MAX_PAYLOAD - _PART_HEADER.size
    count = (len(body) + chunk - 1) // chunk
    if count > 0xFFFF:
        raise ValueError("game message too large ({} bytes)".format(len(body)))
    return [(GAME_MSG_PART, _PART_HEADER.pack(seq, i, count) + body[i * chunk:(i + 1) * chunk]) for i in range(count)]


def decode_game_msg(payload):
    if len(payload) < _GAME_HEADER.size:
        return None
    (msg_id,) = _GAME_HEADER.unpack_from(payload)
    return msg_id, payload[_GAME_HEADER.size:]


class Reassembler:
    """Collects GAME_MSG_PART fragments (reliable + ordered channel, one sender)."""

    def __init__(self):
        self._seq = None
        self._parts = []
        self._count = 0

    def feed(self, payload):
        """Returns (msg_id, data) when a message is complete, else None."""
        if len(payload) < _PART_HEADER.size:
            return None
        seq, index, count = _PART_HEADER.unpack_from(payload)
        chunk = payload[_PART_HEADER.size:]
        if index == 0:
            self._seq, self._parts, self._count = seq, [], count
        if seq != self._seq or index != len(self._parts) or count != self._count:
            self._seq, self._parts, self._count = None, [], 0  # out of sync: drop
            return None
        self._parts.append(chunk)
        if len(self._parts) < count:
            return None
        body = b"".join(self._parts)
        self._seq, self._parts, self._count = None, [], 0
        return decode_game_msg(body)
