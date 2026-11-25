"""ctypes binding to the native core exported by the cts4p winmm.dll.

The game loads Game\\Bin\\winmm.dll (our proxy) before Python starts, so here we
only bind to the module that is already in memory. If the DLL is missing (mod
not installed, or the real System32 winmm.dll was loaded instead) every call
becomes a no-op and `available` stays False.

All methods must be called from the game's Python thread (single producer /
single consumer rings). No game imports here: this file is unit-tested on the
game's Python 3.7 outside the game.
"""
import os
import sys

try:
    import ctypes
except ImportError:  # The Sims 4 ships _ctypes but not the ctypes package
    from cts4p.vendor import ctypes

from cts4p.native import frames
from cts4p.version import NATIVE_API_VERSION, VERSION

_MAX_POLL_FRAMES = 512


class NativeBridge:
    def __init__(self):
        self._lib = None
        self._buf = None
        self.error = None
        self.dll_path = None

    @property
    def available(self):
        return self._lib is not None

    # ------------------------------------------------------------------ setup
    @staticmethod
    def default_dll_path():
        override = os.environ.get("CTS4P_DLL")
        if override:
            return override
        # GetModuleFileNameW(NULL) is the game exe (sys.executable may not be set in the game).
        buf = ctypes.create_unicode_buffer(1024)
        ctypes.windll.kernel32.GetModuleFileNameW(None, buf, len(buf))
        exe = buf.value or sys.executable or ""
        return os.path.join(os.path.dirname(exe), "winmm.dll")

    def attach(self, dll_path=None):
        """Binds to the native core. Returns True on success; sets self.error otherwise."""
        if self._lib is not None:
            return True
        path = dll_path or self.default_dll_path()
        self.dll_path = path
        if not os.path.isfile(path):
            self.error = "cts4p winmm.dll not found at {}".format(path)
            return False
        try:
            lib = ctypes.WinDLL(path)  # already loaded by the game: returns the same module
            lib.cts4p_api_version.restype = ctypes.c_uint32
            lib.cts4p_api_version.argtypes = []
            lib.cts4p_ipc_send.restype = ctypes.c_int32
            lib.cts4p_ipc_send.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
            lib.cts4p_ipc_recv.restype = ctypes.c_int64
            lib.cts4p_ipc_recv.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
            lib.cts4p_status_json.restype = ctypes.c_uint32
            lib.cts4p_status_json.argtypes = [ctypes.c_char_p, ctypes.c_uint32]
        except (OSError, AttributeError) as exc:
            self.error = "winmm.dll is not the cts4p core ({})".format(exc)
            return False
        try:  # optional (native >= 0.4)
            lib.cts4p_build_pump.restype = ctypes.c_int32
            lib.cts4p_build_pump.argtypes = []
            self._build_pump = lib.cts4p_build_pump
        except AttributeError:
            self._build_pump = None
        api = lib.cts4p_api_version()
        if api != NATIVE_API_VERSION:
            self.error = "native API v{} but scripts need v{}".format(api, NATIVE_API_VERSION)
            return False
        self._lib = lib
        self._buf = ctypes.create_string_buffer(256 * 1024)
        self.request({"op": "hello", "python": sys.version.split()[0], "mod": VERSION})
        return True

    # ------------------------------------------------------------------ sending
    def _send_frame(self, frame):
        if self._lib is None:
            return False
        return self._lib.cts4p_ipc_send(frame, len(frame)) == 1

    def request(self, obj):
        """Control request to the native core (JSON)."""
        return self._send_frame(frames.encode_request(obj))

    def send_net(self, msg_type, payload, target="", reliable=True):
        """Gameplay payload to a peer ("" = everyone reachable)."""
        return self._send_frame(frames.encode_net_send(msg_type, payload, target, reliable))

    # ------------------------------------------------------------------ receiving
    def poll(self, max_frames=_MAX_POLL_FRAMES):
        """Returns decoded frames (NetRecv / Event) waiting from the native side."""
        out = []
        if self._lib is None:
            return out
        for _ in range(max_frames):
            n = self._lib.cts4p_ipc_recv(self._buf, len(self._buf))
            if n == 0:
                break
            if n < 0:  # frame larger than our buffer: grow and retry
                self._buf = ctypes.create_string_buffer(-n)
                continue
            decoded = frames.decode(ctypes.string_at(self._buf, n))
            if decoded is not None:
                out.append(decoded)
        return out

    def build_pump(self):
        """Applies queued remote build operations (no-op unless on the game's dispatch thread)."""
        pump = getattr(self, "_build_pump", None)
        return pump() if pump is not None else 0

    def status(self):
        """Native status as a JSON string (for diagnostics)."""
        if self._lib is None:
            return "{}"
        buf = ctypes.create_string_buffer(64 * 1024)
        need = self._lib.cts4p_status_json(buf, len(buf))
        if need >= len(buf):
            buf = ctypes.create_string_buffer(need + 1)
            self._lib.cts4p_status_json(buf, len(buf))
        return buf.value.decode("utf-8", "replace")


bridge = NativeBridge()
