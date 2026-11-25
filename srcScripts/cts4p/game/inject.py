"""Safe wrapping of game functions.

Every hook wraps the original (never copies EA code), keeps a reference to it,
and is idempotent: reloading the scripts does not stack wrappers. Wrappers
must check the multiplayer state first and call the original untouched when
no session is active, so single-player behaves exactly like the base game.
Exceptions inside a wrapper propagate like the game's own (falling back would
risk running the original twice).
"""
import functools

from cts4p import log

_installed = {}


def wrap(owner, name, key=None):
    """Decorator: replace owner.<name> with fn(original, *args, **kwargs)."""

    def decorator(fn):
        ident = key or "{}.{}".format(getattr(owner, "__name__", owner), name)
        original = getattr(owner, name)
        original = getattr(original, "__cts4p_original__", original)

        @functools.wraps(original)
        def wrapper(*args, **kwargs):
            try:
                return fn(original, *args, **kwargs)
            except _Passthrough:
                return original(*args, **kwargs)

        wrapper.__cts4p_original__ = original
        setattr(owner, name, wrapper)
        _installed[ident] = (owner, name, original)
        return fn

    return decorator


class _Passthrough(Exception):
    """Raise inside a wrapper to run the original instead."""


def original_of(owner, name):
    current = getattr(owner, name)
    return getattr(current, "__cts4p_original__", current)


def installed():
    return sorted(_installed)


def guarded(fn):
    """Runs fn, logging (not propagating) exceptions. For callbacks invoked by the game."""

    @functools.wraps(fn)
    def inner(*args, **kwargs):
        try:
            return fn(*args, **kwargs)
        except Exception:
            log.exception("cts4p error in {}", fn.__name__)
            return None

    return inner
