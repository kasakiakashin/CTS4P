"""Dispatch of frames coming from the native core to the host/guest logic."""
from cts4p import log
from cts4p.native import frames
from cts4p.native.bridge import bridge
from cts4p.sync import guest, host, lot_hash, savesync
from cts4p.sync.state import ROLE_GUEST, ROLE_HOST, ROLE_IDLE, state

_MODES = {"host": ROLE_HOST, "guest": ROLE_GUEST, "idle": ROLE_IDLE}


def _set_role(mode):
    role = _MODES.get(mode, ROLE_IDLE)
    if role != state.role:
        log.info("session role: {} -> {}", state.role, role)
        if state.is_host:
            for g in list(state.guests.values()):
                host.destroy_vclient(g)
        savesync.reset()
        state.reset_session()
        state.role = role


def _on_peer_ready(data):
    peer_id, name, role = data.get("id", ""), data.get("name", "?"), data.get("role", "")
    if state.is_host and role == "guest":
        host.on_peer_ready(peer_id, name)
    elif state.is_guest and role == "host":
        guest.on_host_ready(peer_id)


def handle_event(event):
    name, data = event.name, event.data
    if name == "hello":
        savesync.saves_dir = data.get("saves_dir", "")
        savesync.set_auto_sync(data.get("auto_sync_save", True))
        state.local_name = data.get("local_name", "")
        _set_role(data.get("mode", "idle"))
        for peer in data.get("peers", []):
            _on_peer_ready(peer)
    elif name == "session":
        state.local_name = data.get("local_name", state.local_name)
        state.room = data.get("room", "")
        _set_role(data.get("mode", "idle"))
    elif name == "peer_ready":
        _on_peer_ready(data)
    elif name == "peer_left":
        if state.is_host:
            host.on_peer_left(data.get("id", ""))
        elif state.is_guest and data.get("id") == state.host_peer:
            guest.on_host_left()
    elif name == "save_received":
        guest.on_save_received(data)
    elif name == "settings":
        if "auto_sync_save" in data:
            savesync.set_auto_sync(data["auto_sync_save"])
    elif name == "request_save":
        if state.is_host:
            host.request_save()
    elif name == "bb_msg":
        lot_hash.request_check()  # a build operation reached the simulation
    elif name == "bb_replayed":
        lot_hash.request_check()
    elif name == "bb_replay_failed":
        log.warn("a replicated build operation failed here: {}", data)
        if state.is_guest and state.guest_active:
            guest.request_resync()  # host's lot wins
        elif state.is_host:
            lot_hash.request_check(delay=0.5)
    elif name == "compat":
        log.info("content check with {}: {}", data.get("name"), data.get("summary"))
    elif name in ("save_delivered", "save_sending", "pong", "status"):
        log.debug("native event {}", name)


def pump(max_frames=512):
    """Drains native frames. Called every game tick from the zone update hook."""
    if not bridge.available:
        return
    bridge.build_pump()
    for frame in bridge.poll(max_frames):
        try:
            if isinstance(frame, frames.Event):
                handle_event(frame)
            elif state.is_host:
                host.on_net(frame)
            elif state.is_guest:
                guest.on_net(frame)
        except Exception:
            log.exception("cts4p failed handling {!r}", frame)
    if state.is_host:
        savesync.tick()
        lot_hash.tick()
    _update_build_gate()


_gate_open = None


def _update_build_gate():
    """Build replication only while both games show the same lot."""
    global _gate_open
    if state.is_host:
        want = any(g.synced for g in state.guests.values())
    elif state.is_guest:
        want = state.guest_active
    else:
        want = False
    if want != _gate_open:
        _gate_open = want
        bridge.request({"op": "build_gate", "open": want})
