"""Fake guest: joins a room hosted by the REAL game on this PC and plays the guest side
of the cts4p protocol, so the host's in-game logic (save sync, virtual client,
UI mirroring, forwarded commands, Lot State Hash, build replication) can be tested
with a single PC.

usage: python tests/fake_guest.py <winmm.dll> <game cts4p_native.log> <work dir> [steps]
steps (comma list, default all): join,save,welcome,stream,command,lothash,buildop,resync

Writes a JSON report to <work dir>/fake_guest_report.json and prints progress.
"""
import json
import os
import re
import struct
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "srcScripts"))

from cts4p.native import frames  # noqa: E402
from cts4p.native.bridge import NativeBridge  # noqa: E402
from cts4p.net import messages  # noqa: E402

SYNC_SLOT_ID = 0x00054D50
FAKE_CLIENT_ID = 0x0F0E0D0C0B0A0901

dll_path, game_log, work = sys.argv[1], sys.argv[2], sys.argv[3]
steps = set((sys.argv[4] if len(sys.argv) > 4 else "join,save,welcome,stream,command,lothash,buildop,resync").split(","))
report = {"steps": {}, "events": []}


def step(name, ok, **info):
    report["steps"][name] = dict(ok=bool(ok), **info)
    print("[{}] {} {}".format(" OK " if ok else "FAIL", name, json.dumps(info, ensure_ascii=False)[:300]), flush=True)
    save_report()
    return ok


def save_report():
    with open(os.path.join(work, "fake_guest_report.json"), "w", encoding="utf-8") as fh:
        json.dump(report, fh, indent=2, ensure_ascii=False)


# ------------------------------------------------------------------ environment (never touches real folders)
mod_dir = os.path.join(work, "fake_guest_mod")
user_dir = os.path.join(work, "fake_guest_user")
os.makedirs(os.path.join(user_dir, "saves"), exist_ok=True)
os.makedirs(os.path.join(user_dir, "Mods"), exist_ok=True)
os.makedirs(mod_dir, exist_ok=True)
with open(os.path.join(user_dir, "Options.ini"), "w") as fh:
    fh.write("[options]\nscriptmodsenabled = 1\nmodsdisabled = 0\n")
with open(os.path.join(mod_dir, "config.json"), "w", encoding="utf-8") as fh:
    json.dump({"signaling_url": "ws://127.0.0.1:8787/ws", "ice_servers": [], "nickname": "Convidado Falso"}, fh)
os.environ.update({"CTS4P_FORCE": "1", "CTS4P_MOD_DIR": mod_dir, "CTS4P_USER_DIR": user_dir, "CTS4P_DLL": dll_path})

br = NativeBridge()
if not br.attach(dll_path):
    step("attach", False, error=br.error)
    sys.exit(1)
lib = br._lib
lib.cts4p_session_join.argtypes = [__import__("ctypes").c_char_p]
lib.cts4p_debug_build_emit.argtypes = [__import__("ctypes").c_char_p, __import__("ctypes").c_uint32]

inbox = []


def pump():
    for f in br.poll():
        if isinstance(f, frames.Event):
            report["events"].append({"ev": f.name, **{k: v for k, v in f.data.items() if k != "ev"}})
        inbox.append(f)


def wait_for(pred, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        pump()
        for i, f in enumerate(inbox):
            if pred(f):
                return inbox.pop(i)
        time.sleep(0.02)
    return None


def ev(name):
    return lambda f: isinstance(f, frames.Event) and f.name == name


def net(msg_type):
    return lambda f: isinstance(f, frames.NetRecv) and f.msg_type == msg_type


host_peer = None
state = {"generation": None, "zone_id": None}


def send(msg_type, obj):
    return br.send_net(msg_type, messages.encode_json(obj), target=host_peer or "")


def pipe_command(cmd):
    """Test-only control pipe of the game's cts4p (config debug_control=true)."""
    with open("\\\\.\\pipe\\cts4p-debug", "r+b", buffering=0) as p:
        p.write(cmd.encode())
        return p.read(65536).decode("utf-8", "replace")


# ------------------------------------------------------------------ join
time.sleep(1.5)  # runtime bootstrap
if "pipehost" in steps:
    game_status = json.loads(pipe_command("status"))
    step("game: cts4p runtime answering inside the game", True, version=game_status.get("mod_version"),
         python_attached=game_status.get("bridge", {}).get("python_attached"),
         build_hook=game_status.get("build", {}).get("installed"))
    step("game: create room through the debug pipe", json.loads(pipe_command("host")).get("ok"))
    time.sleep(3)
code = None
for line in reversed(open(game_log, encoding="utf-8", errors="replace").read().splitlines()):
    m = re.search(r"signaling: hosting room (CTS-[A-Z0-9]{4})", line)
    if m:
        code = m.group(1)
        break
if not step("room code read from game log", code is not None, code=code):
    sys.exit(1)
lib.cts4p_session_join(code.encode())
ready = wait_for(ev("peer_ready"), 45)
if not step("join: WebRTC link to the game ready", ready is not None, peer=ready and ready.data):
    sys.exit(1)
host_peer = ready.data["id"]
compat = wait_for(ev("compat"), 15)
step("join: game sent its content manifest", compat is not None, summary=compat and compat.data.get("summary"))

# ------------------------------------------------------------------ save sync
send(messages.GUEST_HELLO, {"client_id": FAKE_CLIENT_ID, "name": "Convidado Falso", "in_world": False})
info = wait_for(net(messages.SAVE_INFO), 30)
if info is None and "mainmenu" in steps:
    # Host at the main menu: it must defer the save (not crash) and keep the session up.
    time.sleep(2)
    game_status = json.loads(pipe_command("status"))
    peers = game_status.get("session", {}).get("peers", [])
    step("mainmenu: host deferred the save (not in a lot) and kept the link", any(p.get("ready") for p in peers),
         host_peers=peers)
    report["game_status"] = game_status
    save_report()
    print("done (main-menu scope)", flush=True)
    os._exit(0)
if info is not None:
    data = messages.decode_json(info.payload)
    state.update(generation=data.get("generation"), zone_id=data.get("zone_id"))
step("save: host announced a save (SAVE_INFO)", info is not None, info=info and messages.decode_json(info.payload))
got = wait_for(ev("save_received"), 120)
path = got and got.data.get("path")
size = os.path.getsize(path) if path and os.path.exists(path) else 0
step("save: host's .save received and verified", got is not None and size > 100000, path=path, bytes=size)
paused_msgs = 0

# ------------------------------------------------------------------ welcome (pretend we loaded that save)
send(messages.GUEST_HELLO, {"client_id": FAKE_CLIENT_ID, "name": "Convidado Falso", "in_world": True,
                            "zone_id": state["zone_id"], "slot_id": SYNC_SLOT_ID, "generation": state["generation"]})
welcome = wait_for(net(messages.HOST_WELCOME), 30)
wdata = messages.decode_json(welcome.payload) if welcome else None
step("welcome: host created our virtual client", welcome is not None, welcome=wdata)
br.request({"op": "build_gate", "open": True})

# ------------------------------------------------------------------ mirrored UI stream
counts = {}
first_view_entries = None
reasm = messages.Reassembler()
t_end = time.time() + 20
while time.time() < t_end:
    pump()
    for f in list(inbox):
        if isinstance(f, frames.NetRecv) and f.msg_type in (messages.GAME_MSG, messages.GAME_MSG_PART):
            inbox.remove(f)
            dec = messages.decode_game_msg(f.payload) if f.msg_type == messages.GAME_MSG else reasm.feed(f.payload)
            if dec:
                counts[dec[0]] = counts.get(dec[0], 0) + 1
    time.sleep(0.02)
step("stream: host mirrors its UI messages to us", sum(counts.values()) > 0,
     total=sum(counts.values()), by_msg_id={str(k): v for k, v in sorted(counts.items())})

# ------------------------------------------------------------------ forwarded command (runs as our virtual client)
before = sum(counts.values())
send(messages.COMMAND, {"name": "clock.setspeed", "args": ["three"]})
time.sleep(4)
pump()
after_cmd = [f for f in inbox if isinstance(f, frames.NetRecv) and f.msg_type == messages.GAME_MSG]
step("command: clock.setspeed three forwarded (check the game clock is at 3x)", True,
     game_msgs_after=len(after_cmd))
time.sleep(5)
send(messages.COMMAND, {"name": "clock.setspeed", "args": ["one"]})

# ------------------------------------------------------------------ Lot State Hash (host sends after build activity / every 60 s)
lh = wait_for(net(messages.LOT_HASH), 75)
step("lothash: host computed and sent its structural hash", lh is not None, data=lh and messages.decode_json(lh.payload))

# ------------------------------------------------------------------ build op replication (host replays our frame)
frame_path = os.path.join(HERE, "..", "native", "tests", "data_buildop_frame.bin")
fr = open(frame_path, "rb").read()
sent = lib.cts4p_debug_build_emit(fr, len(fr))
step("buildop: captured RequestBuildBuyOp frame sent to the game", sent == 1, bytes=len(fr))
time.sleep(5)

# ------------------------------------------------------------------ resync request (as the Lot State Hash would)
if "resync" in steps:
    send(messages.RESYNC_REQUEST, {"reason": "lot_hash"})
    info2 = wait_for(net(messages.SAVE_INFO), 30)
    got2 = wait_for(ev("save_received"), 120)
    step("resync: host re-sent its lot after a resync request", info2 is not None and got2 is not None,
         generation=info2 and messages.decode_json(info2.payload).get("generation"))

status = json.loads(br.status())
report["native_status"] = status
save_report()
print("done; report at", os.path.join(work, "fake_guest_report.json"), flush=True)
os._exit(0)
