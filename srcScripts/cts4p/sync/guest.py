"""Guest side of a session.

Once welcomed, the guest's own simulation is frozen (TimeService.update does
nothing) and its UI is driven by the host's stream: every GAME_MSG is handed
to the local UI exactly as if the local simulation had produced it. UI
commands are forwarded to the host by sync.commands.
"""
import omega
import services
import sims4.commands
import time_service

from cts4p import log
from cts4p.game.inject import wrap
from cts4p.native.bridge import bridge
from cts4p.net import messages
from cts4p.sync import cas, lot_hash, presence
from cts4p.sync.state import SYNC_SLOT_ID, state

_reassembler = messages.Reassembler()


def install():
    @wrap(time_service.TimeService, "update")
    def _update(original, self, time_slice=True):
        if state.guest_frozen:
            return None  # the host simulates; we only render its stream
        return original(self, time_slice=time_slice)


def _local_client():
    manager = services.client_manager()
    return manager.get_first_client() if manager is not None else None


def _in_world():
    zone = services.current_zone()
    return zone is not None and zone.is_zone_running


def _loaded_slot_id():
    try:
        slot = services.get_persistence_service().get_save_slot_proto_buff()
        return slot.slot_id if slot is not None else None
    except Exception:
        return None


def send_hello():
    if not state.host_peer:
        return
    client = _local_client() if _in_world() else None
    state.local_client_id = client.id if client is not None else None
    slot = _loaded_slot_id() if client is not None else None
    bridge.send_net(messages.GUEST_HELLO, messages.encode_json({
        "client_id": state.local_client_id,
        "name": state.local_name,
        "in_world": client is not None,
        "zone_id": services.current_zone_id() if client is not None else None,
        "slot_id": slot,
        "generation": state.expected_generation if slot == SYNC_SLOT_ID else None,
    }), target=state.host_peer)


def on_host_ready(peer_id):
    state.host_peer = peer_id
    send_hello()


def on_host_left():
    was_active = state.guest_active
    state.guest_active = False
    state.host_peer = None
    if was_active:
        log.notice("Conexão com o host encerrada. Este lote agora está parado: volte ao menu principal.")


def on_save_received(event):
    state.received_save_slot = event.get("slot_id")
    if not event.get("background"):
        # Only now do we really have that generation on disk (SAVE_INFO alone is just an announcement).
        state.expected_generation = state.announced_generation
    log.info("save received: {} ({} bytes)", event.get("path"), event.get("bytes"))


def on_net(msg):
    if msg.sender != state.host_peer:
        return
    t = msg.msg_type
    if t == messages.GAME_MSG:
        decoded = messages.decode_game_msg(msg.payload)
        if decoded is not None:
            _deliver(*decoded)
    elif t == messages.GAME_MSG_PART:
        decoded = _reassembler.feed(msg.payload)
        if decoded is not None:
            _deliver(*decoded)
    elif t == messages.CLIENT_CHEAT:
        _client_cheat(messages.decode_json(msg.payload) or {})
    elif t == messages.HOST_WELCOME:
        data = messages.decode_json(msg.payload) or {}
        state.vclient_id = data.get("vclient_id")
        state.guest_active = True
        state.guest_frozen = True
        log.notice("Sincronizado! Você está jogando na partida do host.")
        log.info("welcomed by host (virtual client {}, active sim {})", state.vclient_id, data.get("active_sim_id"))
    elif t == messages.SAVE_INFO:
        data = messages.decode_json(msg.payload) or {}
        if data.get("background"):
            log.info("host is refreshing our copy of the save (background)")
            return  # keep playing on the live stream; only the file on disk is updated
        state.announced_generation = data.get("generation")
        if state.guest_active or _in_world():
            log.notice("O host está enviando um save novo. Quando chegar: menu principal > Carregar Jogo > '{}'".format(data.get("label", "CTS4P")))
        state.guest_active = False
    elif t == messages.ZONE_CHANGE:
        data = messages.decode_json(msg.payload) or {}
        if data.get("phase") == "leaving":
            state.guest_active = False
            log.notice("O host está mudando de lote. Aguarde o novo save.")
    elif t == messages.LOT_HASH:
        lot_hash.on_host_hash(messages.decode_json(msg.payload) or {})
    elif t == messages.PRESENCE:
        presence.on_remote(messages.decode_json(msg.payload) or {})
    elif t == messages.REJECT:
        data = messages.decode_json(msg.payload) or {}
        state.guest_active = False
        log.notice("O host recusou a sincronização: {}".format(data.get("reason", "?")))


def _deliver(msg_id, data):
    if not state.guest_active or state.local_client_id is None:
        return
    try:
        omega.send(state.local_client_id, msg_id, data)
    except KeyError:
        log.warn("local client {} is gone; stopping the stream", state.local_client_id)
        state.guest_active = False


def _client_cheat(data):
    cmd = data.get("cmd")
    if not isinstance(cmd, str) or state.local_client_id is None:
        return
    parts = cmd.split()
    if parts and parts[0].startswith("sims.exit2cas") and len(parts) > 1 and parts[1].isdigit():
        state.pending_cas_sim_id = int(parts[1])  # sent back to the host when we return from CAS
    sims4.commands.client_cheat(cmd, state.local_client_id)


def on_zone_ready():
    if state.pending_cas_sim_id is not None and state.host_peer:
        sim_info = services.sim_info_manager().get(state.pending_cas_sim_id)
        if sim_info is not None:
            bridge.send_net(messages.CAS_COMMIT, messages.encode_json({
                "sim_id": state.pending_cas_sim_id, "fields": cas.collect(sim_info),
            }), target=state.host_peer)
            log.info("sent CAS edits of {} to the host", sim_info)
        state.pending_cas_sim_id = None
    send_hello()


def on_zone_teardown():
    state.guest_active = False
    state.guest_frozen = False  # leaving the lot: the next one is loaded normally


def request_resync():
    if state.host_peer:
        bridge.send_net(messages.RESYNC_REQUEST, messages.encode_json({"reason": "player"}), target=state.host_peer)
