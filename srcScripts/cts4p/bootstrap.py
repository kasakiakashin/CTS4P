"""Installs cts4p into the running game. Idempotent.

Without the native core (winmm.dll not installed) nothing is hooked, so the
game stays exactly the base game.
"""
import build_buy
import services
import sims4.core_services
import zone

from cts4p import log
from cts4p.game.inject import guarded, wrap
from cts4p.native.bridge import bridge
from cts4p.sync import commands, guest, host, lot_hash, presence, routing, savesync, session
from cts4p.sync.state import state
from cts4p.version import VERSION

_installed = False


def install():
    global _installed
    if _installed:
        return
    _installed = True
    if not bridge.attach():
        log.warn("cts4p {} scripts loaded without the native core: {}", VERSION, bridge.error)
        return
    state.native_ok = True
    routing.install()
    guest.install()
    savesync.install()
    _install_zone_hooks()
    _install_build_mode_callbacks()
    log.info("cts4p {} scripts attached to the native core ({})", VERSION, bridge.dll_path)


def _install_build_mode_callbacks():
    """The game's official Build/Buy enter/exit callbacks (called with no arguments)."""

    def on_enter():
        presence.on_build_mode(state.local_name or "?", True)
        if state.guest_active:
            log.notice("Aviso: construções do convidado ainda não são transmitidas; o lote do host prevalece.")

    def on_exit():
        presence.on_build_mode(state.local_name or "?", False)
        if state.is_host:
            lot_hash.request_check(delay=1.0)

    build_buy.register_build_buy_enter_callback(guarded(on_enter))
    build_buy.register_build_buy_exit_callback(guarded(on_exit))


def _in_running_zone():
    current = services.current_zone()
    return current is not None and current.is_zone_running


def _install_zone_hooks():
    # areaserver.c_api_server_tick calls sims4.core_services.on_tick() every tick, in a lot
    # or not. Outside a lot (main menu, loading) it is our only tick: without it a guest at
    # the main menu would never answer the host and the save would never be sent.
    @wrap(sims4.core_services, "on_tick")
    def _core_tick(original):
        result = original()
        if not _in_running_zone():
            session.pump()
        return result

    @wrap(zone.Zone, "update")
    def _update(original, self, absolute_ticks):
        result = original(self, absolute_ticks)
        session.pump()
        return result

    @wrap(zone.Zone, "on_loading_screen_animation_finished")
    def _ready(original, self):
        result = original(self)
        commands.install()  # all game modules are imported by now
        session.pump()      # apply any role change that arrived during loading
        if state.is_host:
            host.on_zone_ready()
        elif state.is_guest:
            guest.on_zone_ready()
        return result

    @wrap(zone.Zone, "on_teardown")
    def _teardown(original, self, client):
        if state.is_host:
            host.on_zone_teardown()
        elif state.is_guest:
            guest.on_zone_teardown()
        return original(self, client)
