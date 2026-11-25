"""Third-party code bundled with CTS4P.

ctypes/ - the pure-Python part of CPython 3.7.0's ctypes package
          (https://github.com/python/cpython/tree/v3.7.0/Lib/ctypes),
          Python Software Foundation License. The Sims 4 ships the C half
          (_ctypes_x64.pyd) but not this package, so `import ctypes` fails in
          the game. Only change: two absolute `ctypes.` imports made relative so
          it can live under cts4p.vendor without shadowing anything global.
"""
