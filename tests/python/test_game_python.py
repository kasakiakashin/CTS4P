"""Runs on The Sims 4's own Python 3.7 through native/build/Release/py37host.exe.

usage: py37host <Game\\Bin> <base.zip> <srcScripts or cts4p.ts4script> test_game_python.py <winmm.dll> <scratch dir>

Checks, with the game's real interpreter and _ctypes:
  * pure modules (frames, messages) round-trip correctly;
  * the vendored ctypes package works on the game's _ctypes;
  * the ctypes bridge talks to the real winmm.dll (hello, ping/pong, status).
"""
import json
import os
import sys
import time

failures = []


def check(ok, what):
    print("  [{}] {}".format(" OK " if ok else "FAIL", what))
    if not ok:
        failures.append(what)


dll_path, scratch = sys.argv[1], sys.argv[2]
print("python {} at {}".format(sys.version.split()[0], sys.executable or "(embedded)"))

# ------------------------------------------------------------------ pure modules
from cts4p.native import frames  # noqa: E402
from cts4p.net import messages  # noqa: E402
from cts4p.version import VERSION  # noqa: E402

req = frames.encode_request({"op": "ping", "n": 1, "txt": "ação"})
check(req[0] == frames.KIND_REQUEST and json.loads(req[1:].decode("utf-8"))["txt"] == "ação", "request frame encodes UTF-8 JSON")
ns = frames.encode_net_send(messages.COMMAND, b"xyz", "peer1", reliable=False)
check(ns[:4] == bytes([1, 0, messages.COMMAND, 5]) and ns[4:9] == b"peer1" and ns[9:] == b"xyz", "net_send frame layout matches bridge.h")
nr = frames.decode(bytes([frames.KIND_NET_RECV, 34, 2]) + b"ab" + b"payload")
check(isinstance(nr, frames.NetRecv) and nr.msg_type == 34 and nr.sender == "ab" and nr.payload == b"payload", "net_recv frame decodes")
check(frames.decode(b"\x82{bad json") is None and frames.decode(b"") is None, "malformed frames are rejected")
try:
    frames.encode_net_send(5, b"")
    check(False, "non-gameplay type rejected")
except ValueError:
    check(True, "non-gameplay type rejected")

small = messages.encode_game_msg(11, b"hello", 1)
check(len(small) == 1 and small[0][0] == messages.GAME_MSG and messages.decode_game_msg(small[0][1]) == (11, b"hello"), "small game msg is one frame")
big_data = bytes(range(256)) * 4000  # ~1 MB
parts = messages.encode_game_msg(11, big_data, 7)
check(len(parts) > 1 and all(t == messages.GAME_MSG_PART and len(p) <= messages.MAX_PAYLOAD for t, p in parts), "1 MB game msg is fragmented")
r = messages.Reassembler()
result = None
for _, payload in parts:
    result = r.feed(payload) or result
check(result == (11, big_data), "fragments reassemble to the original message")
r2 = messages.Reassembler()
check(r2.feed(parts[1][1]) is None and r2.feed(parts[0][1]) is None, "out-of-order fragment is dropped safely")

# ------------------------------------------------------------------ vendored ctypes on the game's _ctypes
try:
    import ctypes  # noqa: F401
    check(False, "game has no ctypes package (expected; vendored copy needed)")
except ImportError:
    check(True, "game has no ctypes package (expected; vendored copy needed)")
from cts4p.vendor import ctypes as vct  # noqa: E402

buf = vct.create_string_buffer(b"cts4p", 16)
check(vct.string_at(buf, 5) == b"cts4p" and vct.sizeof(vct.c_uint64) == 8, "vendored ctypes works on the game's _ctypes")
check(vct.windll.kernel32.GetCurrentProcessId() == os.getpid(), "vendored ctypes calls Win32 (kernel32)")

# ------------------------------------------------------------------ bridge against the real DLL
os.environ["CTS4P_FORCE"] = "1"
os.environ["CTS4P_MOD_DIR"] = os.path.join(scratch, "py37_mod_dir")
os.environ["CTS4P_DLL"] = dll_path
from cts4p.native.bridge import bridge  # noqa: E402

check(bridge.attach(), "bridge attached to winmm.dll ({})".format(bridge.error or "ok"))


def wait_event(name, timeout=10.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        for f in bridge.poll():
            if isinstance(f, frames.Event) and f.name == name:
                return f.data
        time.sleep(0.02)
    return None


hello = wait_event("hello")
check(hello is not None and hello.get("native") == VERSION, "native answered hello with matching version ({})".format(hello and hello.get("native")))
check(hello is not None and "saves_dir" in hello and hello.get("mode") == "idle", "hello carries saves_dir and session mode")
t0 = time.perf_counter()
bridge.request({"op": "ping", "n": 42})
pong = wait_event("pong")
rtt_ms = (time.perf_counter() - t0) * 1000
check(pong is not None and pong.get("n") == 42, "ping/pong through the IPC rings ({:.2f} ms)".format(rtt_ms))

lat = []
for i in range(200):
    t0 = time.perf_counter()
    bridge.request({"op": "ping", "n": i})
    got = None
    while got is None and time.perf_counter() - t0 < 2:
        for f in bridge.poll():
            if isinstance(f, frames.Event) and f.name == "pong" and f.data.get("n") == i:
                got = f
    lat.append((time.perf_counter() - t0) * 1000)
lat.sort()
check(len(lat) == 200 and lat[-1] < 2000, "200 IPC round trips: median {:.3f} ms, p99 {:.3f} ms".format(lat[100], lat[197]))

status = json.loads(bridge.status())
br = status.get("bridge", {})
check(br.get("python_attached") is True and br.get("python_version", "").startswith("3.7"), "native sees the Python side attached (python {})".format(br.get("python_version")))
check(bridge.send_net(messages.COMMAND, b"{}"), "net_send frame accepted with no session (dropped natively)")

print("test_game_python: {} ({} failure{})".format("FAILED" if failures else "PASSED", len(failures), "" if len(failures) == 1 else "s"))
sys.stdout.flush()
raise SystemExit(1 if failures else 0)
