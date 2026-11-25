"""Host side of a session: guests, their virtual clients, and their commands."""
import server.account
import services

from cts4p import log
from cts4p.native.bridge import bridge
from cts4p.net import messages
from cts4p.sync import cas, commands, presence, savesync
from cts4p.sync.state import SYNC_SLOT_ID, VACCOUNT_ID_BASE, VCLIENT_ID_BASE, GuestInfo, state

_next_index = 1


def _in_world():
    zone = services.current_zone()
    return zone is not None and zone.is_zone_running


def _host_client():
    manager = services.client_manager()
    if manager is None:
        return None
    return manager.get(state.host_client_id) if state.host_client_id is not None else manager.get_first_client()


# ------------------------------------------------------------------ peers
def on_peer_ready(peer_id, name):
    if peer_id not in state.guests:
        state.guests[peer_id] = GuestInfo(peer_id, name)
        log.info("guest {} ({}) connected", name, peer_id)


def on_peer_left(peer_id):
    guest = state.guests.pop(peer_id, None)
    if guest is None:
        return
    destroy_vclient(guest)
    log.notice("{} saiu da sessão".format(guest.name))
    savesync.release_pause_if_synced()


# ------------------------------------------------------------------ virtual clients
def create_vclient(guest):
    """Creates the guest's virtual client with the household's Sims, preferring a Sim the host isn't using."""
    global _next_index
    host_client = _host_client()
    if host_client is None:
        return False
    vclient_id = VCLIENT_ID_BASE + _next_index
    account = server.account.Account(VACCOUNT_ID_BASE + _next_index, guest.name)
    _next_index += 1

    state.vclients[vclient_id] = guest  # must be registered before create_client (routing hooks look it up)
    guest.synced = True
    try:
        vclient = services.client_manager().create_client(vclient_id, account, host_client.household_id)
    except Exception:
        state.vclients.pop(vclient_id, None)
        guest.synced = False
        log.exception("could not create virtual client for {}", guest.name)
        return False
    guest.vclient = vclient

    host_active = host_client.active_sim_info
    preferred = None
    for sim_info in host_client.selectable_sims:
        vclient.add_selectable_sim_info(sim_info, send_relationship_update=False)
        if preferred is None and sim_info is not host_active:
            preferred = sim_info
    if preferred is not None:
        vclient.set_active_sim_info(preferred)
    vclient.send_selectable_sims_update()
    vclient.resend_active_sim_info()
    return True


def destroy_vclient(guest):
    vclient, guest.vclient = guest.vclient, None
    guest.synced = False
    if vclient is None:
        return
    state.vclients.pop(vclient.id, None)
    manager = services.client_manager()
    if manager is not None and vclient.id in manager:
        try:
            manager.remove(vclient)
        except Exception:
            log.exception("removing virtual client of {}", guest.name)


def welcome(guest):
    if not create_vclient(guest):
        bridge.send_net(messages.REJECT, messages.encode_json({"reason": "o host não conseguiu criar seu cliente"}), target=guest.peer_id)
        return
    active = guest.vclient.active_sim_info
    bridge.send_net(messages.HOST_WELCOME, messages.encode_json({
        "vclient_id": guest.vclient.id,
        "active_sim_id": active.sim_id if active is not None else 0,
        "zone_id": services.current_zone_id(),
    }), target=guest.peer_id)
    log.notice("{} está jogando com você!".format(guest.name))
    savesync.release_pause_if_synced()


# ------------------------------------------------------------------ messages
def on_net(msg):
    guest = state.guests.get(msg.sender)
    if guest is None:
        return
    if msg.msg_type == messages.COMMAND:
        if guest.synced:
            commands.execute_for_guest(guest, msg.payload)
    elif msg.msg_type == messages.GUEST_HELLO:
        on_guest_hello(guest, messages.decode_json(msg.payload) or {})
    elif msg.msg_type == messages.RESYNC_REQUEST:
        reason = (messages.decode_json(msg.payload) or {}).get("reason", "?")
        log.info("{} asked for a resync ({})", guest.name, reason)
        if reason == "lot_hash":
            log.notice("Lote de {} divergiu do seu: reenviando o estado (auto-correção)".format(guest.name))
        destroy_vclient(guest)
        savesync.save_and_send(guest.peer_id)
    elif msg.msg_type == messages.CAS_COMMIT:
        on_cas_commit(guest, messages.decode_json(msg.payload) or {})
    elif msg.msg_type == messages.PRESENCE:
        presence.on_remote(messages.decode_json(msg.payload) or {})


def on_guest_hello(guest, data):
    guest.local_client_id = data.get("client_id")
    guest.in_world = bool(data.get("in_world"))
    if not _in_world():
        log.notice("{} conectou. Entre num lote para sincronizar.".format(guest.name))
        return
    same_save = (guest.in_world and data.get("slot_id") == SYNC_SLOT_ID and data.get("generation") == state.save_generation
                 and data.get("zone_id") == services.current_zone_id() and state.save_generation > 0)
    if same_save and guest.local_client_id:
        # Also after the guest's own zone reload (e.g. back from CAS): start a fresh stream.
        destroy_vclient(guest)
        welcome(guest)
    else:
        destroy_vclient(guest)
        savesync.save_and_send(guest.peer_id)


def on_cas_commit(guest, data):
    sim_info = services.sim_info_manager().get(data.get("sim_id", 0))
    fields = data.get("fields")
    if sim_info is None or not isinstance(fields, dict):
        return
    applied = cas.apply(sim_info, fields)
    log.notice("Aparência de {} atualizada por {}".format(sim_info.first_name, guest.name))
    log.info("CAS commit from {}: {} fields applied to {}", guest.name, applied, sim_info)


# ------------------------------------------------------------------ zone lifecycle
def on_zone_ready():
    """Host arrived in a lot (load, travel, back from CAS): resync every guest."""
    for guest in state.guests.values():
        guest.synced = False
    if state.guests:
        savesync.save_and_send("")


def on_zone_teardown():
    for guest in list(state.guests.values()):
        destroy_vclient(guest)
    state.vclients.clear()
    if state.guests:
        bridge.send_net(messages.ZONE_CHANGE, messages.encode_json({"phase": "leaving"}))


def request_save():
    savesync.save_and_send("")
