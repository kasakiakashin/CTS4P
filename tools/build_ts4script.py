"""Builds cts4p.ts4script with The Sims 4's own Python 3.7 (run through py37host).

usage: py37host <Game\\Bin> <base.zip> "" tools/build_ts4script.py <srcScripts> <out.ts4script>

  * byte-compiles every module with the game's interpreter (same .pyc magic the
    game expects); any syntax error fails the build;
  * writes an uncompressed zip (the layout the game's script loader reads);
  * re-opens the archive and imports the game-independent modules from it, so a
    broken package layout or a module that fails at import time is caught here.
"""
import importlib._bootstrap_external as bootstrap_external
import os
import sys
import time
import zipfile

src_root, out_path = sys.argv[1], sys.argv[2]
SKIP_DIRS = {"__pycache__", "tests"}

modules = []
for dirpath, dirnames, files in os.walk(src_root):
    dirnames[:] = sorted(d for d in dirnames if d not in SKIP_DIRS)
    for name in sorted(files):
        if name.endswith(".py"):
            modules.append(os.path.join(dirpath, name))

errors = []
compiled = []
now = int(time.time())
for path in modules:
    rel = os.path.relpath(path, src_root).replace(os.sep, "/")
    with open(path, "rb") as fh:
        source = fh.read()
    try:
        code = compile(source, rel, "exec", dont_inherit=True, optimize=0)
    except SyntaxError as exc:
        errors.append("{}:{}: {}".format(rel, exc.lineno, exc.msg))
        continue
    data = bootstrap_external._code_to_timestamp_pyc(code, now, len(source))
    compiled.append((rel[:-3] + ".pyc", bytes(data)))

if errors:
    for e in errors:
        print("SYNTAX ERROR (Python {}): {}".format(sys.version.split()[0], e))
    raise SystemExit(1)

os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
tmp = out_path + ".tmp"
with zipfile.ZipFile(tmp, "w", compression=zipfile.ZIP_STORED) as z:
    for arcname, data in compiled:
        info = zipfile.ZipInfo(arcname, date_time=time.localtime(now)[:6])
        z.writestr(info, data)
os.replace(tmp, out_path)

# --- verify the archive the way the game would load it
for name in list(sys.modules):
    if name == "cts4p" or name.startswith("cts4p."):
        del sys.modules[name]
sys.path.insert(0, out_path)
problems = []
for mod in ("cts4p", "cts4p.version", "cts4p.native.frames", "cts4p.net.messages", "cts4p.vendor.ctypes",
            "cts4p.native.bridge", "cts4p.sync.state", "cts4p.sync.command_table"):
    try:
        m = __import__(mod, fromlist=["*"])
        archive = os.path.normcase(os.path.abspath(out_path))
        if not os.path.normcase(getattr(m, "__file__", "") or "").startswith(archive + os.sep):
            problems.append("{} imported from outside the archive ({})".format(mod, getattr(m, "__file__", "?")))
    except Exception as exc:  # noqa: BLE001 - report everything
        problems.append("{}: {!r}".format(mod, exc))
from cts4p.version import VERSION  # noqa: E402
from cts4p.sync.command_table import FORWARDED_COMMANDS  # noqa: E402

for p in problems:
    print("IMPORT ERROR:", p)
size = os.path.getsize(out_path)
print("built {} - cts4p {} - {} modules compiled with Python {} - {:.1f} KB - {} forwarded commands".format(
    out_path, VERSION, len(compiled), sys.version.split()[0], size / 1024.0, len(FORWARDED_COMMANDS)))
raise SystemExit(1 if problems else 0)
