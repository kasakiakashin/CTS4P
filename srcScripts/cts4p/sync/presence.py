"""Where each player is acting (data for the Ghost Cursor).

Positions come from the game's own pick commands: every click in the world
sends interactions.has_choices / interactions.choices with the picked world
coordinates (x, y, z) and floor level. Entering/leaving Build mode is reported
too. The host knows every player's picks (guests' commands run on the host),
so it rebroadcasts them; each game shows the others in the overlay.
"""
import time

from cts4p.native.bridge import bridge
from cts4p.net import messages
from cts4p.sync.state import state

PICK_COMMANDS = frozenset(("interactions.has_choices", "interactions.choices"))
MIN_INTERVAL = 0.25  # seconds between broadcasts per player

_last_sent = {}


def _parse_pick(args):
    # has_choices/choices(target_id, pick_type, x, y, z, lot_id, level, ...)
    try:
        x, y, z = float(args[2]), float(args[3]), float(args[4])
        level = int(float(args[6])) if len(args) > 6 else 0
        return x, y, z, level
    except (IndexError, ValueError, TypeError):
        return None


def _publish(entry, target=""):
    bridge.request({"op": "presence", **entry})
    if state.is_host and state.guests:
        bridge.send_net(messages.PRESENCE, messages.encode_json(entry), target=target, reliable=False)
    elif state.is_guest and state.host_peer:
        bridge.send_net(messages.PRESENCE, messages.encode_json(entry), target=state.host_peer, reliable=False)


def on_pick(player_name, command, args):
    if command not in PICK_COMMANDS:
        return
    pick = _parse_pick(args)
    if pick is None:
        return
    now = time.monotonic()
    if now - _last_sent.get(player_name, 0.0) < MIN_INTERVAL:
        return
    _last_sent[player_name] = now
    x, y, z, level = pick
    _publish({"name": player_name, "mode": "vivo", "x": round(x, 2), "y": round(y, 2), "z": round(z, 2), "level": level})


def on_build_mode(player_name, entering):
    _publish({"name": player_name, "mode": "construção" if entering else "vivo"})


def on_remote(data):
    """Presence received from another player (host relays guests to each other)."""
    if not isinstance(data, dict) or not data.get("name"):
        return
    bridge.request({"op": "presence", **data})
    if state.is_host:
        bridge.send_net(messages.PRESENCE, messages.encode_json(data), reliable=False)
