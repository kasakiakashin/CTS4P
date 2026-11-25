"""Static check of every game API the cts4p scripts touch, against the decompiled game.

Code that imports game modules can't run outside The Sims 4, so we verify it
statically against _decompiled_ref (the scripts of the installed game version):

  * every `import x.y` / `from x import name` of a game module resolves
    (module file exists; `name` is a top-level def/class/assignment or submodule);
  * every attribute chain on an imported game module (services.current_zone,
    server.client.Client.send_message, ...) resolves to a module, class or member;
  * every wrap(owner, "name") target exists;
  * every cts4p.* import exists in srcScripts.

usage: python tools/check_game_imports.py <srcScripts> <_decompiled_ref> [base.zip] [extracted pyc root]
"""
import ast
import os
import re
import sys
import zipfile

STDLIB_ZIP_HINT = "base.zip"
ROOTS = ("simulation", "core", "generated")


def _bytecode_names(pyc_path):
    """Module-level names and class members read from a .pyc with xdis (for partial decompiles)."""
    try:
        from xdis.load import load_module
    except ImportError:
        return None
    code = load_module(pyc_path)[3]
    names = set(code.co_names)
    classes = {}
    for const in code.co_consts:
        if hasattr(const, "co_name") and hasattr(const, "co_names") and const.co_name[:1].isupper():
            members = set(const.co_names) | {c.co_name for c in const.co_consts if hasattr(c, "co_name")}
            classes[const.co_name] = members
    return names | set(classes), classes


class GameIndex:
    def __init__(self, ref, pyc_root=None):
        self.ref = ref
        self.pyc_root = pyc_root
        self._cache = {}
        self.partial = set()
        for root in ROOTS:
            report = os.path.join(ref, root, "_decompile_report.tsv")
            if os.path.isfile(report):
                for line in open(report, encoding="utf-8"):
                    status, _, rel = line.strip().partition("\t")
                    if status != "ok":
                        self.partial.add((root, rel.replace("\\", "/")))

    def _pyc_for(self, dotted):
        if not self.pyc_root:
            return None
        rel = dotted.replace(".", "/")
        for root in ROOTS:
            for cand in (rel + ".pyc", rel + "/__init__.pyc"):
                if (root, cand) in self.partial:
                    path = os.path.join(self.pyc_root, root, cand)
                    if os.path.isfile(path):
                        return path
        return None

    def module_file(self, dotted):
        rel = dotted.replace(".", os.sep)
        for root in ROOTS:
            for cand in (os.path.join(self.ref, root, rel + ".py"), os.path.join(self.ref, root, rel, "__init__.py")):
                if os.path.isfile(cand):
                    return cand
        return None

    def is_package(self, dotted):
        rel = dotted.replace(".", os.sep)
        return any(os.path.isdir(os.path.join(self.ref, root, rel)) for root in ROOTS)

    def top_level(self, dotted):
        """Names defined at module level (best effort over decompiler output)."""
        if dotted in self._cache:
            return self._cache[dotted]
        path = self.module_file(dotted)
        names, classes = set(), {}
        pyc = self._pyc_for(dotted)
        if pyc:  # decompiler output is incomplete: trust the bytecode instead
            result = _bytecode_names(pyc)
            if result is not None:
                self._cache[dotted] = result
                return result
        if path:
            text = open(path, encoding="utf-8", errors="replace").read()
            names |= set(re.findall(r"^(?:def|class) ([A-Za-z_]\w*)", text, re.M))
            names |= set(re.findall(r"^([A-Za-z_]\w*)\s*[:=]", text, re.M))
            names |= set(re.findall(r"^\s{4}([A-Za-z_]\w*)\s*=\s*", text, re.M))  # try/except module-level defs
            names |= set(re.findall(r"^(?:from \S+ )?import (.+)$", text, re.M) and
                         [n.strip().split(" as ")[-1] for line in re.findall(r"^(?:from \S+ )?import (.+)$", text, re.M)
                          for n in line.split(",")])
            # class bodies: methods and class attributes, including inherited-by-name not resolved
            for m in re.finditer(r"^class ([A-Za-z_]\w*)\b.*?:\n(.*?)(?=^\S)", text + "\nEND\n", re.M | re.S):
                body = m.group(2)
                members = set(re.findall(r"^    (?:def )?([A-Za-z_]\w*)\s*(?:\(|=|:)", body, re.M))
                members |= set(re.findall(r"^    @(\w+)\.setter", body, re.M))
                classes[m.group(1)] = members
            # protobuf modules: generated names are assigned dynamically
            if os.sep + "protocolbuffers" + os.sep in path:
                names |= set(re.findall(r"name='([A-Za-z_]\w*)'", text))
                names |= set(re.findall(r"\b([A-Z][A-Z0-9_]+)\s*=\s*\d+", text))
        self._cache[dotted] = (names, classes)
        return names, classes


def stdlib_names(base_zip):
    names = set()
    if base_zip and os.path.isfile(base_zip):
        with zipfile.ZipFile(base_zip) as z:
            for n in z.namelist():
                if n.startswith("lib/"):
                    top = n[4:].split("/")[0].replace(".pyc", "")
                    names.add(top)
    return names | {"_ctypes", "sys", "builtins", "time", "math", "_thread", "zlib", "nt", "marshal"}


def main(src, ref, base_zip=None, pyc_root=None):
    game = GameIndex(ref, pyc_root)
    stdlib = stdlib_names(base_zip)
    errors, checked = [], 0
    src_modules = set()
    for dirpath, _, files in os.walk(src):
        for f in files:
            if f.endswith(".py"):
                rel = os.path.relpath(os.path.join(dirpath, f), src)[:-3].replace(os.sep, ".")
                src_modules.add(rel[: -len(".__init__")] if rel.endswith(".__init__") else rel)

    def is_game(mod):
        top = mod.split(".")[0]
        return top not in stdlib and top not in ("cts4p", "cts4p_loader") and (game.module_file(mod) or game.is_package(top))

    for dirpath, _, files in os.walk(src):
        if os.sep + "vendor" in dirpath:
            continue  # third-party code, validated by running it
        for f in sorted(files):
            if not f.endswith(".py") or f == "command_table.py":
                continue
            path = os.path.join(dirpath, f)
            rel = os.path.relpath(path, src)
            tree = ast.parse(open(path, encoding="utf-8").read(), filename=path, feature_version=(3, 7))
            aliases = {}  # local name -> dotted game module
            # Imports inside `try: ... except ImportError:` are optional by design.
            guarded = set()
            for node in ast.walk(tree):
                if isinstance(node, ast.Try) and any(
                        getattr(h.type, "id", None) in ("ImportError", "ModuleNotFoundError") for h in node.handlers):
                    for child in node.body:
                        guarded.add(id(child))

            for node in ast.walk(tree):
                if isinstance(node, ast.Import) and id(node) in guarded:
                    continue
                if isinstance(node, ast.Import):
                    for a in node.names:
                        checked += 1
                        if a.name.startswith("cts4p"):
                            if a.name not in src_modules:
                                errors.append("{}:{} cts4p module not found: {}".format(rel, node.lineno, a.name))
                        elif is_game(a.name):
                            if not game.module_file(a.name) and not game.is_package(a.name):
                                errors.append("{}:{} game module not found: {}".format(rel, node.lineno, a.name))
                            if a.asname:
                                aliases[a.asname] = a.name
                            else:
                                parts = a.name.split(".")
                                for i in range(1, len(parts) + 1):
                                    aliases.setdefault(".".join(parts[:i]), ".".join(parts[:i]))
                        elif a.name.split(".")[0] not in stdlib:
                            errors.append("{}:{} unknown module (not game, not stdlib): {}".format(rel, node.lineno, a.name))
                elif isinstance(node, ast.ImportFrom) and node.module and node.level == 0:
                    mod = node.module
                    if mod.startswith("cts4p"):
                        for a in node.names:
                            checked += 1
                            if mod not in src_modules:
                                errors.append("{}:{} cts4p module not found: {}".format(rel, node.lineno, mod))
                            elif mod + "." + a.name not in src_modules:
                                src_path = os.path.join(src, mod.replace(".", os.sep))
                                src_file = src_path + ".py" if os.path.isfile(src_path + ".py") else os.path.join(src_path, "__init__.py")
                                text = open(src_file, encoding="utf-8").read()
                                if not re.search(r"^(?:def|class) {0}\b|^{0}\s*=|import .*\b{0}\b".format(re.escape(a.name)), text, re.M):
                                    errors.append("{}:{} {} has no {}".format(rel, node.lineno, mod, a.name))
                        continue
                    if not is_game(mod):
                        if mod.split(".")[0] not in stdlib:
                            errors.append("{}:{} unknown module: {}".format(rel, node.lineno, mod))
                        continue
                    names, _ = game.top_level(mod)
                    for a in node.names:
                        checked += 1
                        if a.name not in names and not game.module_file(mod + "." + a.name):
                            errors.append("{}:{} game module {} has no '{}'".format(rel, node.lineno, mod, a.name))
                        elif game.module_file(mod + "." + a.name):
                            aliases[a.asname or a.name] = mod + "." + a.name
                        else:
                            aliases[a.asname or a.name] = ("member", mod, a.name)

            def resolve(expr):
                """Returns a description of what a dotted expression points to, or None if unknown root."""
                chain = []
                while isinstance(expr, ast.Attribute):
                    chain.append(expr.attr)
                    expr = expr.value
                if not isinstance(expr, ast.Name) or expr.id not in aliases:
                    return None
                chain.reverse()
                target = aliases[expr.id]
                if isinstance(target, tuple):  # from-imported class/function
                    _, mod, name = target
                    return ("class", mod, name, chain)
                return ("module", target, chain)

            def verify(expr, lineno, extra=None):
                nonlocal checked
                info = resolve(expr)
                if info is None:
                    return
                if info[0] == "class":
                    _, mod, cls, chain = info
                    chain = chain + ([extra] if extra else [])
                    if chain:
                        checked += 1
                        _, classes = game.top_level(mod)
                        if cls in classes and chain[0] not in classes[cls]:
                            errors.append("{}:{} {}.{} has no member '{}'".format(rel, lineno, mod, cls, chain[0]))
                    return
                _, mod, chain = info
                chain = chain + ([extra] if extra else [])
                while chain and game.module_file(mod + "." + chain[0]) is not None:
                    mod = mod + "." + chain.pop(0)
                if not chain:
                    return
                checked += 1
                names, classes = game.top_level(mod)
                if chain[0] not in names:
                    errors.append("{}:{} game module {} has no '{}'".format(rel, lineno, mod, chain[0]))
                elif len(chain) > 1 and chain[0] in classes and chain[1] not in classes[chain[0]]:
                    errors.append("{}:{} {}.{} has no member '{}'".format(rel, lineno, mod, chain[0], chain[1]))

            for node in ast.walk(tree):
                if isinstance(node, ast.Attribute) and isinstance(node.ctx, ast.Load):
                    verify(node, node.lineno)
                if (isinstance(node, ast.Call) and getattr(node.func, "id", None) == "wrap" and len(node.args) >= 2
                        and isinstance(node.args[1], ast.Constant) and isinstance(node.args[1].value, str)):
                    verify(node.args[0], node.lineno, extra=node.args[1].value)

    for e in sorted(set(errors)):
        print("ERROR", e)
    print("check_game_imports: {} references checked, {} problem(s)".format(checked, len(set(errors))))
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1], sys.argv[2], sys.argv[3] if len(sys.argv) > 3 else None,
                  sys.argv[4] if len(sys.argv) > 4 else None))
