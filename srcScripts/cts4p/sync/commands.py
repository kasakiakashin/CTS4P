"""Forwarding of the player's UI commands from guests to the host.

For every Live command in command_table.FORWARDED_COMMANDS we rebuild the
game's own command handler ("invoker", which parses the raw string arguments
exactly like the game does) by running the original function through
sims4.commands.Command with `register` temporarily captured. Then we register
a forwarder under the same name:

  * not a synced guest  -> runs the invoker locally (identical to the base game)
  * synced guest        -> sends {name, raw args} to the host
  * host                -> execute_for_guest() runs the invoker with the guest's
                           virtual client as _connection, inside a routing context
                           so personal responses go back only to that guest.

No string parsing of our own and no eval: the host only runs commands from
this table, with the game's own argument parser.
"""
import importlib

import sims4.commands

from cts4p import log
from cts4p.native.bridge import bridge
from cts4p.net import messages
from cts4p.sync import presence, routing
from cts4p.sync.command_table import FORWARDED_COMMANDS
from cts4p.sync.state import state

_invokers = {}
_installed = False
unavailable = []  # (command name, reason) - shown in the log and by cts4p.status

MAX_ARGS = 64
MAX_ARG_CHARS = 4096


def _decorator_kwargs(opts):
    """Rebuilds the original Command(...) keyword arguments (restrictions, pack, console type)."""
    kwargs = {"command_type": sims4.commands.CommandType.Live}
    flags = opts.get("command_restrictions")
    if flags:
        value = sims4.commands.CommandRestrictionFlags.UNRESTRICTED
        for flag in flags:
            value |= getattr(sims4.commands.CommandRestrictionFlags, flag)
        kwargs["command_restrictions"] = value
    if opts.get("pack"):
        import sims4.common
        kwargs["pack"] = getattr(sims4.common.Pack, opts["pack"])
    if opts.get("console_type"):
        kwargs["console_type"] = getattr(sims4.commands.CommandType, opts["console_type"])
    return kwargs


def _capture(func, names, opts):
    captured = {}
    real_register = sims4.commands.register

    def capture(name, restrictions, handler, description, usage, command_type):
        captured[name] = (restrictions, handler, description, usage, command_type)

    sims4.commands.register = capture
    try:
        # Same decorator arguments as the game used, so restrictions (e.g. save-locked CAS)
        # and pack gating behave exactly like the base game.
        sims4.commands.Command(*names, **_decorator_kwargs(opts))(func)
    finally:
        sims4.commands.register = real_register
    return captured


def _make_forwarder(name, invoker):
    def forward(*args, _session_id=0, **kwargs):
        if state.guest_active and not kwargs:
            send_to_host(name, args)
            return True
        if state.is_host and state.guests:
            presence.on_pick(state.local_name or "host", name, args)
        return invoker(*args, _session_id=_session_id, **kwargs)

    forward.__name__ = "cts4p_forward ({})".format(name)
    return forward


def install():
    """Re-registers forwarders. Done on the first zone load, when every game module is imported."""
    global _installed
    if _installed:
        return
    _installed = True
    ok = 0
    unavailable.clear()
    for module_name, func_name, names, opts in FORWARDED_COMMANDS:
        try:
            module = importlib.import_module(module_name)
        except Exception as exc:
            unavailable.append((names[0], "import {}: {}: {}".format(module_name, type(exc).__name__, exc)))
            continue
        func = getattr(module, func_name, None)
        if func is None:
            # sims4.commands.Command returns None for commands whose pack isn't installed.
            unavailable.append((names[0], "pacote não instalado"))
            continue
        if not callable(func):
            unavailable.append((names[0], "{}.{} is {}".format(module_name, func_name, type(func).__name__)))
            continue
        try:
            captured = _capture(func, names, opts)
        except Exception as exc:
            unavailable.append((names[0], "wrap failed: {}: {}".format(type(exc).__name__, exc)))
            continue
        if not captured:  # e.g. the command needs a pack that isn't installed
            unavailable.append((names[0], "not registered by the game (pack-restricted?)"))
        for name, (restrictions, invoker, description, usage, command_type) in captured.items():
            _invokers[name] = invoker
            sims4.commands.register(name, restrictions, _make_forwarder(name, invoker), description, usage, command_type)
            ok += 1
    log.info("command forwarding ready: {} commands ({} unavailable)", ok, len(unavailable))
    for name, reason in unavailable:
        log.info("  unavailable: {} - {}", name, reason)


def send_to_host(name, args):
    raw = [str(a) for a in args][:MAX_ARGS]
    bridge.send_net(messages.COMMAND, messages.encode_json({"name": name, "args": raw}), target=state.host_peer or "")


def execute_for_guest(guest, payload):
    """Host side: runs one forwarded command on behalf of `guest`."""
    data = messages.decode_json(payload)
    if data is None or guest.vclient is None:
        return
    name = data.get("name")
    args = data.get("args") or []
    invoker = _invokers.get(name)
    if invoker is None:
        log.warn("guest {} sent a command that is not forwardable: {!r}", guest.name, name)
        return
    if not isinstance(args, list) or len(args) > MAX_ARGS or any(not isinstance(a, str) or len(a) > MAX_ARG_CHARS for a in args):
        log.warn("guest {} sent malformed args for {}", guest.name, name)
        return
    presence.on_pick(guest.name, name, args)
    with routing.routed_to(guest):
        try:
            invoker(*args, _session_id=guest.vclient.id)
        except Exception:
            log.exception("command {} from {} failed", name, guest.name)


def forwardable(name):
    return name in _invokers
