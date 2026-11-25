"""cts4p - multiplayer co-op for The Sims 4 (Python side).

The game imports the modules of every .ts4script in Mods; importing this
package installs the hooks once (see cts4p.bootstrap). The heavy lifting
(network, overlay, save transfer) is done by the native core, winmm.dll.
"""
from cts4p.version import VERSION  # noqa: F401


def _start():
    from cts4p import bootstrap, cheats  # noqa: F401  (cheats registers console commands)

    bootstrap.install()


try:
    import sims4  # noqa: F401  (only present inside the game)
except ImportError:
    pass  # imported by unit tests outside the game
else:
    _start()
