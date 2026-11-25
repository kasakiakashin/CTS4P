"""Logging: the game's own logger plus the native log file (Mods/cts4p/cts4p_native.log).

No file I/O from Python: lines go to the native core through the IPC bridge.
"""
import traceback

from cts4p.native.bridge import bridge

try:
    import sims4.log

    _game_logger = sims4.log.Logger("cts4p", default_owner="cts4p")
except ImportError:  # outside the game (unit tests)
    _game_logger = None


def _emit(level, message, args):
    text = message.format(*args) if args else message
    if bridge.available:
        bridge.request({"op": "log", "level": level, "msg": text})
    if _game_logger is not None:
        # Pass the text as an argument so braces in it are never re-formatted.
        getattr(_game_logger, "warn" if level == "warn" else level, _game_logger.info)("{}", text)


def debug(message, *args):
    _emit("debug", message, args)


def info(message, *args):
    _emit("info", message, args)


def warn(message, *args):
    _emit("warn", message, args)


def error(message, *args):
    _emit("error", message, args)


def exception(message, *args):
    _emit("error", message + "\n" + traceback.format_exc(), args)


def notice(text):
    """Toast in the overlay (both players see their own)."""
    if bridge.available:
        bridge.request({"op": "notice", "text": text})
