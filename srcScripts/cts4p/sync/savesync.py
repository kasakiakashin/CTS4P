"""Host: save the current game to the dedicated cts4p slot and send it to guests.

Used when a guest joins and after every zone change (travel, returning from
CAS). The clock is paused under a named pause handle from just before the save
until every guest has loaded it and been welcomed, so nothing happens in the
host's world that the guests' copy would miss.

Automatic sync ("Sincronizar automaticamente", on by default): every time the
host's game saves (the player's Save or the game's autosave), the sync slot is
saved again and sent as a *background* copy. Guests keep playing on the live
stream; the copy only keeps their file current (e.g. if the connection drops).
Background copies never pause the clock and never change save_generation, so
the "guest already has this save" check is unaffected.
"""
import os
import time

import services
from clock import ClockSpeedMode, GameSpeedChangeSource
from services.persistence_service import SaveGameData

from cts4p import log
from cts4p.game.inject import wrap
from cts4p.native.bridge import bridge
from cts4p.net import messages
from cts4p.sync.state import SYNC_SLOT_ID, state

PAUSE_HANDLE = "cts4p: sincronizando convidados"
PAUSE_TIMEOUT_SECONDS = 300
PENDING_TIMEOUT_SECONDS = 180  # the save file never showed up: give up and unpause
FILE_SETTLE_SECONDS = 0.5

saves_dir = ""          # filled from the native 'hello' event
_pending = None         # save waiting for the file to settle before sending
_pause_started = None
_pause_request = None   # our own clock request (UI_MODAL, like Build mode's pause)
_background_due = False  # host saved: refresh the guests' copy when idle
_own_save = False        # True while cts4p itself is saving into the sync slot


def slot_path():
    return os.path.join(saves_dir, "Slot_{:08x}.save".format(SYNC_SLOT_ID))


def label():
    return "CTS4P - {}".format(state.local_name or "host")


def pause_for_sync():
    """Holds the clock with our *own* request. Speed buttons (clock.setspeed) only clear
    GAMEPLAY requests, and removing exactly this request never touches the player's pauses."""
    global _pause_started, _pause_request
    if _pause_started is None:
        _pause_request = services.game_clock_service().push_speed(
            ClockSpeedMode.PAUSED, source=GameSpeedChangeSource.UI_MODAL, reason=PAUSE_HANDLE)
        _pause_started = time.monotonic()
        log.info("clock paused while guests sync")


def _unpause():
    global _pause_started, _pause_request
    clock_service = services.game_clock_service()
    if clock_service is not None:
        if _pause_request is not None:
            clock_service.remove_request(_pause_request, source=GameSpeedChangeSource.UI_MODAL, reason=PAUSE_HANDLE)
        else:
            clock_service.pop_speed(ClockSpeedMode.PAUSED, source=GameSpeedChangeSource.UI_MODAL, reason=PAUSE_HANDLE)
    _pause_started = None
    _pause_request = None


def reset():
    """Session ended or changed role: forget pending sends and release our pause."""
    global _pending, _background_due
    _pending = None
    _background_due = False
    if _pause_started is not None:
        _unpause()
        log.info("clock released (session ended)")


def release_pause_if_synced(force=False):
    global _pause_started
    if _pause_started is None:
        return
    waiting = [g for g in state.guests.values() if not g.synced]
    if waiting and not force and time.monotonic() - _pause_started < PAUSE_TIMEOUT_SECONDS:
        return
    _unpause()
    log.info("clock released ({})", "timeout" if waiting else "all guests synced")


def install():
    from services.persistence_service import PersistenceService

    @wrap(PersistenceService, "save_game_gen")
    def _save_game_gen(original, self, timeline, save_game_data, *args, **kwargs):
        result = yield from original(self, timeline, save_game_data, *args, **kwargs)
        _on_game_saved(save_game_data, result)
        return result


def _on_game_saved(save_game_data, result):
    global _background_due
    if _own_save or not state.is_host or not state.auto_sync_save:
        return
    slot_id = getattr(save_game_data, "slot_id", 0) or 0
    if slot_id <= 1 or slot_id == SYNC_SLOT_ID:
        return  # scratch saves (e.g. travel) and our own slot
    if result is not None and int(result) != 0:  # SaveGameResult.SUCCESS == 0
        return
    if any(g.synced for g in state.guests.values()):
        _background_due = True
        log.info("host saved slot {:08x}: guests' copy will be refreshed", slot_id)


def set_auto_sync(enabled):
    global _background_due
    state.auto_sync_save = bool(enabled)
    if not enabled:
        _background_due = False
    log.info("automatic save sync {}", "on" if enabled else "off")


def save_and_send(target="", background=False, only=None):
    """Starts a save into the sync slot; the file is sent once it is written."""
    global _own_save
    if not saves_dir:
        log.warn("save sync: saves folder unknown (native hello not received)")
        return False
    zone = services.current_zone()
    if zone is None or not zone.is_zone_running:
        log.info("save sync deferred: host is not in a lot yet")
        return False
    if background:
        generation = state.save_generation
    else:
        state.save_generation += 1
        generation = state.save_generation
        pause_for_sync()
    bridge.send_net(messages.SAVE_INFO, messages.encode_json({
        "generation": generation, "slot_id": SYNC_SLOT_ID, "label": label(), "zone_id": services.current_zone_id(),
        "background": background,
    }), target=target)

    started = time.time()
    persistence = services.get_persistence_service()

    def save_gen(timeline):
        global _pending, _own_save
        _own_save = True
        # The game writes slot_id/slot_name into its *live* save-slot record while saving.
        # Restore them afterwards so the player's own "Save" keeps going to their own slot.
        slot = persistence.get_save_slot_proto_buff()
        original = (slot.slot_id, slot.slot_name) if slot is not None else None
        try:
            result = yield from persistence.save_game_gen(timeline, SaveGameData(SYNC_SLOT_ID, label(), True, None),
                                                          send_save_message=False, check_cooldown=False, ignore_callback=True)
        finally:
            _own_save = False
            if original is not None:
                slot = persistence.get_save_slot_proto_buff()
                if slot is not None:
                    slot.slot_id, slot.slot_name = original
        if result is not None and int(result) != 0:  # SaveGameResult.SUCCESS == 0 (locked, disk full...)
            log.warn("save sync #{} failed (result {}); guests not updated", generation, result)
            if not background:
                log.notice("Não foi possível salvar para sincronizar agora. Tente de novo em instantes.")
                release_pause_if_synced(force=True)
            return
        _pending = {"generation": generation, "target": target, "since": started, "size": -1, "stable_at": None,
                    "background": background, "only": only, "started_mono": time.monotonic()}

    persistence.save_using(save_gen)
    if not background:
        log.notice("Salvando o jogo para sincronizar os convidados...")
    log.info("save sync #{} started (slot {:08x}{})", generation, SYNC_SLOT_ID, ", background" if background else "")
    return True


def tick():
    """Called every game tick: sends the save once the file is fully written."""
    global _pending, _background_due
    release_pause_if_synced()
    if _pending is not None and time.monotonic() - _pending["started_mono"] > PENDING_TIMEOUT_SECONDS:
        log.warn("save sync #{}: file never settled; giving up", _pending["generation"])
        _pending = None
        release_pause_if_synced(force=True)
    if _pending is None:
        if _background_due and not _own_save and _pause_started is None:
            _background_due = False
            synced = [g.peer_id for g in state.guests.values() if g.synced]
            if synced:
                save_and_send(synced[0] if len(synced) == 1 else "", background=True, only=synced)
        return
    path = slot_path()
    try:
        st = os.stat(path)
    except OSError:
        return
    if st.st_mtime + 2 < _pending["since"]:
        return  # still the previous file
    now = time.monotonic()
    if st.st_size != _pending["size"]:
        _pending["size"], _pending["stable_at"] = st.st_size, now
        return
    if now - _pending["stable_at"] < FILE_SETTLE_SECONDS:
        return
    pending, _pending = _pending, None
    targets = pending.get("only") or [pending["target"]]
    if pending.get("only") and len(targets) > 1:
        targets = [t for t in targets if t in state.guests]  # one transfer per still-connected synced guest
    for to in targets:
        bridge.request({"op": "send_save", "path": path, "slot_id": SYNC_SLOT_ID, "label": label(), "to": to,
                        "background": pending.get("background", False)})
    log.info("save sync #{}: {} bytes handed to the native core", pending["generation"], st.st_size)
