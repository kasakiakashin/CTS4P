"""Host-side routing of what the simulation sends to clients.

Everything the host's simulation sends to the host UI goes through
server.client.Client.send_message. We:

  * mirror it to every synced guest (needs, moods, interaction queues, money,
    clock, object changes...), dropping the ops that target the host's own
    client object (host camera / active Sim / skewer);
  * route ops owned by a guest's virtual client (its active Sim, skewer) only
    to that guest, rewriting the client id to the guest's local client id;
  * while a guest's command runs (routing context), route its personal
    responses (pie menus, phone menu, dialogs, picker UIs) only to that guest;
  * send modal dialogs owned by a guest's active Sim only to that guest;
  * forward client-side commands (e.g. "enter CAS") addressed to a virtual
    client to that guest's game.

With no guests connected every wrapper calls the original immediately.
"""
import contextlib

import distributor.system
import server.client
import server.clientmanager
import sims4.commands
import ui.ui_dialog
from protocolbuffers import Consts_pb2, Distributor_pb2

from cts4p import log
from cts4p.game.inject import wrap
from cts4p.native.bridge import bridge
from cts4p.net import messages
from cts4p.sync.state import state

# Events that everyone should see even when triggered by one player's command.
BROADCAST_EVENTS = frozenset((Consts_pb2.MSG_UI_NOTIFICATION_SHOW,))

_route_stack = []


@contextlib.contextmanager
def routed_to(guest):
    """Personal responses produced inside this block go only to `guest`."""
    _route_stack.append(guest)
    try:
        yield
    finally:
        _route_stack.pop()


def current_route():
    return _route_stack[-1] if _route_stack else None


def _hosting_with_guests():
    return state.is_host and bool(state.guests)


def send_game_msg(guest, msg_id, data):
    """Delivers one game protobuf message to a guest's UI (fragmenting if needed)."""
    if not guest.synced:
        return
    state.game_msg_seq = (state.game_msg_seq + 1) & 0xFFFFFFFF
    for msg_type, payload in messages.encode_game_msg(msg_id, data, state.game_msg_seq):
        bridge.send_net(msg_type, payload, target=guest.peer_id)
    guest.game_msgs_sent += 1


def _strip_host_client_entries(data, host_client_id):
    """Removes view-update entries addressed to the host's own client object."""
    view = Distributor_pb2.ViewUpdate()
    view.ParseFromString(data)
    keep = [e for e in view.entries
            if not (e.primary_channel.id.manager_id == Consts_pb2.MGR_CLIENT and e.primary_channel.id.object_id == host_client_id)]
    if len(keep) == len(view.entries):
        return data
    if not keep:
        return None
    out = Distributor_pb2.ViewUpdate()
    for entry in keep:
        out.entries.add().CopyFrom(entry)
    return out.SerializeToString()


def mirror_to_guests(host_client, msg_id, data):
    for guest in list(state.guests.values()):
        if not guest.synced:
            continue
        payload = data
        if msg_id == Consts_pb2.MSG_OBJECTS_VIEW_UPDATE:
            payload = _strip_host_client_entries(data, host_client.id)
            if payload is None:
                continue
        send_game_msg(guest, msg_id, payload)


def send_op_to_guest(guest, dist, obj, op):
    """Builds a one-op view update exactly like the distributor would, for one guest."""
    journal = dist.journal
    entry = journal._build_journal_entry(journal._build_journal_seed(op, obj, None))
    if entry is None:
        return
    (obj_id, operation, _payload_type, manager_id, _name) = entry
    view = Distributor_pb2.ViewUpdate()
    out = view.entries.add()
    out.primary_channel.id.manager_id = manager_id
    if obj is not None and obj is guest.vclient and guest.local_client_id is not None:
        obj_id = guest.local_client_id  # the guest's UI knows its own client by its local id
    out.primary_channel.id.object_id = obj_id
    out.operation_list.operations.append(operation)
    send_game_msg(guest, Consts_pb2.MSG_OBJECTS_VIEW_UPDATE, view.SerializeToString())


def _guest_of(obj):
    if isinstance(obj, server.client.Client):
        return state.vclients.get(obj.id)
    return None


def _guest_controlling_sim(sim_id):
    for guest in state.guests.values():
        vclient = guest.vclient
        if vclient is not None and vclient.active_sim_info is not None and vclient.active_sim_info.sim_id == sim_id:
            return guest
    return None


def install():
    @wrap(server.client.Client, "send_message")
    def _send_message(original, self, msg_id, msg):
        if not state.is_host:
            return original(self, msg_id, msg)
        guest = state.vclients.get(self.id)
        if guest is not None:  # message for a virtual client: only that guest's UI exists
            send_game_msg(guest, msg_id, msg.SerializeToString())
            return None
        result = original(self, msg_id, msg)
        if state.guests and self.id == state.host_client_id:
            try:
                mirror_to_guests(self, msg_id, msg.SerializeToString())
            except Exception:
                log.exception("mirroring message {} failed", msg_id)
        return result

    @wrap(distributor.system.Distributor, "add_op")
    def _add_op(original, self, obj, op):
        if _hosting_with_guests():
            guest = _guest_of(obj)
            if guest is not None:
                send_op_to_guest(guest, self, obj, op)
                return None
        return original(self, obj, op)

    @wrap(distributor.system.Distributor, "add_op_with_no_owner")
    def _add_op_with_no_owner(original, self, op):
        guest = current_route()
        if guest is not None and _hosting_with_guests():
            send_op_to_guest(guest, self, None, op)
            return None
        return original(self, op)

    @wrap(distributor.system.Distributor, "add_event")
    def _add_event(original, self, msg_id, msg, immediate=False):
        guest = current_route()
        if guest is not None and _hosting_with_guests() and msg_id not in BROADCAST_EVENTS:
            send_game_msg(guest, msg_id, msg.SerializeToString())
            return None
        return original(self, msg_id, msg, immediate)

    @wrap(distributor.system.Distributor, "add_object")
    def _add_object(original, self, obj):
        if _guest_of(obj) is not None:
            obj.visible_to_client = True  # the guest's UI already has its own client object
            return None
        return original(self, obj)

    @wrap(distributor.system.Distributor, "remove_object")
    def _remove_object(original, self, obj, **kwargs):
        if _guest_of(obj) is not None:
            return None
        return original(self, obj, **kwargs)

    @wrap(server.client.Client, "on_add")
    def _client_on_add(original, self):
        if self.id not in state.vclients:
            state.host_client_id = self.id  # the real (local) client
            return original(self)
        # Virtual client: everything the original does except registering with the
        # host's singleton Distributor (the host UI must not see the guest's client).
        if self._account is not None:
            self._account.register_client(self)
        for sim_info in self._selectable_sims:
            self.on_sim_added_to_skewer(sim_info)
        self.selectable_sims.add_watcher(self, self.send_selectable_sims_update)
        return None

    @wrap(server.client.Client, "on_remove")
    def _client_on_remove(original, self):
        if self.id not in state.vclients:
            return original(self)
        if self.active_sim is not None:
            self._set_active_sim_without_field_distribution(None)
        if self._account is not None:
            self._account.unregister_client(self)
        for sim_info in self._selectable_sims:
            self.on_sim_removed_from_skewer(sim_info)
        self.selectable_sims.remove_watcher(self)
        self._selectable_sims = None
        self.active = False
        return None

    @wrap(server.client.Client, "send_selectable_sims_update")
    def _send_selectable_sims_update(original, self):
        guest = state.vclients.get(self.id)
        if guest is None:
            return original(self)
        with routed_to(guest):
            return original(self)

    @wrap(server.clientmanager.ClientManager, "get_first_client")
    def _get_first_client(original, self):
        if not state.vclients:
            return original(self)
        for client in self._objects.values():
            if client.id not in state.vclients:
                return client
        return None

    @wrap(server.clientmanager.ClientManager, "get_first_client_id")
    def _get_first_client_id(original, self):
        if not state.vclients:
            return original(self)
        for client in self._objects.values():
            if client.id not in state.vclients:
                return client.id
        return None

    @wrap(ui.ui_dialog.UiDialogBase, "distribute_dialog")
    def _distribute_dialog(original, self, dialog_type, dialog_msg, immediate=False):
        if (_hosting_with_guests() and current_route() is None and dialog_type != Consts_pb2.MSG_UI_NOTIFICATION_SHOW):
            guest = _guest_controlling_sim(getattr(dialog_msg, "owner_id", 0))
            if guest is not None:
                with routed_to(guest):
                    return original(self, dialog_type, dialog_msg, immediate)
        return original(self, dialog_type, dialog_msg, immediate)

    @wrap(sims4.commands, "client_cheat")
    def _client_cheat(original, s, context):
        guest = state.vclients.get(context) if state.is_host else None
        if guest is not None:
            bridge.send_net(messages.CLIENT_CHEAT, messages.encode_json({"cmd": s}), target=guest.peer_id)
            return None
        return original(s, context)
