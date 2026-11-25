"""Fake host: the REAL game joins (as guest) a room hosted by this script, so the guest
side of the in-game Python can be tested on one PC: GUEST_HELLO from the main menu,
SAVE_INFO handling, and the save transfer written by the game into its saves folder.

usage: python tests/fake_host.py <winmm.dll> <game cts4p_native.log> <work dir> <save to send> <game saves dir>
"""
import hashlib
import json
import os
import struct
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "srcScripts"))

from cts4p.native import frames  # noqa: E402
from cts4p.native.bridge import NativeBridge  # noqa: E402
from cts4p.net import messages  # noqa: E402

SYNC_SLOT_ID = 0x00054D50
dll_path, game_log, work, save_src, game_saves = sys.argv[1:6]
report = {"steps": {}}


def step(name, ok, **info):
    report["steps"][name] = dict(ok=bool(ok), **info)
    print("[{}] {} {}".format(" OK " if ok else "FAIL", name, json.dumps(info, ensure_ascii=False)[:300]), flush=True)
    with open(os.path.join(work, "fake_host_report.json"), "w", encoding="utf-8") as fh:
        json.dump(report, fh, indent=2, ensure_ascii=False)
    return ok


def pipe_command(cmd):
    with open("\\\\.\\pipe\\cts4p-debug", "r+b", buffering=0) as p:
        p.write(cmd.encode())
        return p.read(65536).decode("utf-8", "replace")


mod_dir = os.path.join(work, "fake_host_mod")
user_dir = os.path.join(work, "fake_host_user")
for d in (mod_dir, os.path.join(user_dir, "saves"), os.path.join(user_dir, "Mods")):
    os.makedirs(d, exist_ok=True)
with open(os.path.join(user_dir, "Options.ini"), "w") as fh:
    fh.write("[options]\nscriptmodsenabled = 1\nmodsdisabled = 0\n")
with open(os.path.join(mod_dir, "config.json"), "w", encoding="utf-8") as fh:
    json.dump({"signaling_url": "ws://127.0.0.1:8787/ws", "ice_servers": [], "nickname": "Host Falso"}, fh)
os.environ.update({"CTS4P_FORCE": "1", "CTS4P_MOD_DIR": mod_dir, "CTS4P_USER_DIR": user_dir, "CTS4P_DLL": dll_path})

br = NativeBridge()
if not br.attach(dll_path):
    step("attach", False, error=br.error)
    sys.exit(1)
lib = br._lib
inbox = []


def wait_for(pred, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        inbox.extend(br.poll())
        for i, f in enumerate(inbox):
            if pred(f):
                return inbox.pop(i)
        time.sleep(0.02)
    return None


time.sleep(1.5)
lib.cts4p_session_host()
room = None
deadline = time.time() + 15
while time.time() < deadline and not room:
    st = json.loads(br.status())
    if st.get("session", {}).get("signaling_open"):
        room = st["session"].get("room")
    time.sleep(0.2)
if not step("fake host: room created", room, room=room):
    sys.exit(1)

step("game: join requested through the debug pipe", json.loads(pipe_command("join " + room)).get("ok"))
ready = wait_for(lambda f: isinstance(f, frames.Event) and f.name == "peer_ready", 45)
if not step("game joined as guest (WebRTC ready)", ready is not None, peer=ready and ready.data):
    sys.exit(1)
guest = ready.data["id"]

hello = wait_for(lambda f: isinstance(f, frames.NetRecv) and f.msg_type == messages.GUEST_HELLO, 30)
hdata = messages.decode_json(hello.payload) if hello else None
step("game's Python sent GUEST_HELLO from the main menu", hello is not None and hdata.get("in_world") is False, hello=hdata)

# Send the save the way the real host does: SAVE_INFO first, then the file.
br.send_net(messages.SAVE_INFO, messages.encode_json({"generation": 1, "slot_id": SYNC_SLOT_ID, "label": "cts4p - teste",
                                                      "zone_id": 1234}), target=guest)
br.request({"op": "send_save", "path": save_src, "slot_id": SYNC_SLOT_ID, "label": "cts4p - teste", "to": guest})
delivered = wait_for(lambda f: isinstance(f, frames.Event) and f.name == "save_delivered", 120)
step("game received, verified and saved the file (SaveResult ok)", delivered is not None and delivered.data.get("ok"),
     result=delivered and delivered.data)

dest = os.path.join(game_saves, "Slot_{:08x}.save".format(SYNC_SLOT_ID))
same = os.path.exists(dest) and hashlib.sha256(open(dest, "rb").read()).hexdigest() == hashlib.sha256(open(save_src, "rb").read()).hexdigest()
step("file in the game's saves folder is byte-identical", same, path=dest)

time.sleep(2)
st = json.loads(pipe_command("status"))
report["game_status"] = {"transfers": st.get("transfers"), "session": st.get("session", {}).get("mode")}
step("game shows the transfer as finished", any(t.get("finished") and t.get("ok") for t in st.get("transfers", [])),
     transfers=st.get("transfers"))

# ---------------------------------------------------------------- 0.4.3
# A host may only write the cts4p slot: any other slot (e.g. the player's own save) is refused.
other_slot = 0x00012345
br.request({"op": "send_save", "path": save_src, "slot_id": other_slot, "label": "nao deveria gravar", "to": guest})
refused = wait_for(lambda f: isinstance(f, frames.Event) and f.name == "save_delivered", 60)
step("game refuses a save aimed at another slot", refused is not None and not refused.data.get("ok"), result=refused and refused.data)
step("nothing written in that slot", not os.path.exists(os.path.join(game_saves, "Slot_{:08x}.save".format(other_slot))))

# "Criar juntos" game -> host: the game sends the newest household of its real Library.
game_tray = os.path.join(os.path.dirname(game_saves), "Tray")
host_tray = os.path.join(user_dir, "Tray")
if os.path.isdir(host_tray):
    for f in os.listdir(host_tray):
        os.remove(os.path.join(host_tray, f))
sent = json.loads(pipe_command("send_tray"))
step("game: send_tray (newest household of the real Library)", sent.get("ok"), result=sent)
got = wait_for(lambda f: isinstance(f, frames.Event) and f.name == "tray_received", 60)
step("fake host received the household", got is not None, event=got and got.data)
copied = sorted(os.listdir(host_tray)) if os.path.isdir(host_tray) else []
identical = bool(copied) and all(
    open(os.path.join(host_tray, f), "rb").read() == open(os.path.join(game_tray, f), "rb").read() for f in copied)
step("household files are byte-identical to the game's Library", identical, files=copied)

# "Criar juntos" host -> game: a small test household lands in the game's Library, then is removed.
test_files = {
    "0x00000001!0x00f7e57000000010.trayitem": b"\0\0\0\0" + struct.pack("<I", 17) + b"\x08\x10\x10\x01\x22\x0bcts4p Teste",
    "0x00000000!0x00f7e57000000010.householdbinary": b"H" * 2000,
}
src_dir = os.path.join(work, "fake_host_tray_src")
os.makedirs(src_dir, exist_ok=True)
for name, data in test_files.items():
    with open(os.path.join(src_dir, name), "wb") as fh:
        fh.write(data)
os.makedirs(host_tray, exist_ok=True)  # the fake host's own Library: put the test household there to send it
for name, data in test_files.items():
    with open(os.path.join(host_tray, name), "wb") as fh:
        fh.write(data)
os.utime(os.path.join(host_tray, "0x00000001!0x00f7e57000000010.trayitem"), None)
br.request({"op": "send_tray", "id": 0x00f7e57000000010})
dl = wait_for(lambda f: isinstance(f, frames.Event) and f.name == "save_delivered", 60)
step("game accepted the household from the host", dl is not None and dl.data.get("ok"), result=dl and dl.data)
landed = all(os.path.exists(os.path.join(game_tray, n)) for n in test_files)
step("test household written in the game's Library", landed)
for n in test_files:  # cleanup: only the files this test created
    try:
        os.remove(os.path.join(game_tray, n))
    except OSError:
        pass
step("test household removed again", not any(os.path.exists(os.path.join(game_tray, n)) for n in test_files))
pipe_command("leave")
print("done", flush=True)
os._exit(0)
