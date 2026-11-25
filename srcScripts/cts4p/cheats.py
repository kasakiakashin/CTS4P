"""Console commands (Ctrl+Shift+C). No cheats needed."""
import json

import sims4.commands

from cts4p.native.bridge import bridge
from cts4p.sync import commands, guest, host
from cts4p.sync.state import state
from cts4p.version import VERSION


@sims4.commands.Command("cts4p.status", command_type=sims4.commands.CommandType.Live)
def cts4p_status(_connection=None):
    out = sims4.commands.CheatOutput(_connection)
    out("CTS4P {} | native: {} | role: {} | room: {}".format(VERSION, "ok" if bridge.available else bridge.error, state.role, state.room or "-"))
    if state.is_host:
        for g in state.guests.values():
            out("  guest {} synced={} msgs={}".format(g.name, g.synced, g.game_msgs_sent))
    if state.is_guest:
        out("  host={} active={} vclient={}".format(state.host_peer, state.guest_active, state.vclient_id))
    out("  forwarded commands: {} ({} unavailable)".format(len(commands._invokers), len(commands.unavailable)))
    for name, reason in commands.unavailable[:10]:
        out("    - {}: {}".format(name, reason[:120]))
    if bridge.available:
        status = json.loads(bridge.status() or "{}")
        out("  peers: {}".format(", ".join("{} ({})".format(p.get("name"), p.get("state")) for p in status.get("session", {}).get("peers", [])) or "-"))


@sims4.commands.Command("cts4p.resync", command_type=sims4.commands.CommandType.Live)
def cts4p_resync(_connection=None):
    out = sims4.commands.CheatOutput(_connection)
    if state.is_host:
        host.request_save()
        out("CTS4P: enviando o save atual para os convidados")
    elif state.is_guest:
        guest.request_resync()
        out("CTS4P: pedi um save novo ao host")
    else:
        out("CTS4P: nenhuma sessão ativa (INSERT para abrir o painel)")
