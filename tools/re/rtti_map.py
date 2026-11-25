"""Static reverse-engineering helper for Simulation_x64.dll (MSVC x64 RTTI).

  * RTTI: TypeDescriptor name -> CompleteObjectLocator -> vtable(s) -> method RVAs
  * xrefs: code locations that reference a string with RIP-relative LEA
  * functions: boundaries from the .pdata exception directory; disassembly with capstone

usage:
  python rtti_map.py <dll> classes <regex>           list classes + vtables + methods
  python rtti_map.py <dll> xref <ascii string>       functions referencing the string
  python rtti_map.py <dll> disasm <rva hex> [n]      disassemble the function at/containing rva
  python rtti_map.py <dll> vcalls <rva hex>          calls made by a function (direct targets)
"""
import bisect
import re
import struct
import sys

import capstone
import pefile


class Image:
    def __init__(self, path):
        self.pe = pefile.PE(path, fast_load=True)
        self.pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXCEPTION"]])
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        self.data = self.pe.get_memory_mapped_image()
        self.sections = {s.Name.rstrip(b"\0").decode(): s for s in self.pe.sections}
        text = self.sections[".text"]
        self.text_lo, self.text_hi = text.VirtualAddress, text.VirtualAddress + text.Misc_VirtualSize
        self.funcs = sorted((e.struct.BeginAddress, e.struct.EndAddress) for e in getattr(self.pe, "DIRECTORY_ENTRY_EXCEPTION", []))
        self._starts = [f[0] for f in self.funcs]
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
        self.md.detail = True

    def section_range(self, name):
        s = self.sections[name]
        return s.VirtualAddress, s.VirtualAddress + s.Misc_VirtualSize

    def u32(self, rva):
        return struct.unpack_from("<I", self.data, rva)[0]

    def u64(self, rva):
        return struct.unpack_from("<Q", self.data, rva)[0]

    def func_containing(self, rva):
        i = bisect.bisect_right(self._starts, rva) - 1
        if i >= 0 and self.funcs[i][0] <= rva < self.funcs[i][1]:
            # chained unwind entries split one function; walk back while contiguous
            begin, end = self.funcs[i]
            return begin, end
        return None

    # ------------------------------------------------------------------ RTTI
    def type_descriptors(self, pattern):
        rx = re.compile(pattern)
        out = {}
        for m in re.finditer(rb"\.\?A[VU][A-Za-z0-9_@?$]+@@\0", self.data):
            name = m.group()[:-1].decode()
            if rx.search(name):
                out[name] = m.start() - 16  # TypeDescriptor = {pVFTable, spare, name}
        return out

    def complete_object_locators(self, td_rva):
        lo, hi = self.section_range(".rdata")
        needle = struct.pack("<I", td_rva)
        cols = []
        start = lo
        while True:
            i = self.data.find(needle, start, hi)
            if i < 0:
                break
            col = i - 12
            if col >= lo and self.u32(col) == 1 and self.u32(col + 20) == col:
                cols.append((col, self.u32(col + 4)))  # (rva, offset of this vtable in the object)
            start = i + 1
        return cols

    def vtables_for(self, col_rva):
        lo, hi = self.section_range(".rdata")
        needle = struct.pack("<Q", self.base + col_rva)
        out, start = [], lo
        while True:
            i = self.data.find(needle, start, hi)
            if i < 0:
                break
            vt = i + 8
            methods = []
            while True:
                ptr = self.u64(vt + 8 * len(methods)) - self.base
                if not (self.text_lo <= ptr < self.text_hi) or len(methods) > 400:
                    break
                methods.append(ptr)
            out.append((vt, methods))
            start = i + 1
        return out

    # ------------------------------------------------------------------ xrefs
    def string_rvas(self, text):
        b = text.encode() + b"\0"
        return [m.start() for m in re.finditer(re.escape(b), self.data)]

    def lea_xrefs(self, target_rvas):
        targets = set(target_rvas)
        lo, hi = self.text_lo, self.text_hi
        refs = []
        for m in re.finditer(rb"[\x48\x4c]\x8d[\x05\x0d\x15\x1d\x25\x2d\x35\x3d]", self.data[lo:hi]):
            at = lo + m.start()
            disp = struct.unpack_from("<i", self.data, at + 3)[0]
            if at + 7 + disp in targets:
                refs.append(at)
        return refs

    # ------------------------------------------------------------------ disassembly
    def disasm(self, rva, limit=200):
        f = self.func_containing(rva)
        begin, end = f if f else (rva, rva + 0x200)
        code = self.data[begin:end]
        lines = []
        for ins in self.md.disasm(code, self.base + begin):
            note = ""
            if ins.op_str.startswith("qword ptr [rip") or "[rip " in ins.op_str:
                for op in ins.operands:
                    if op.type == capstone.x86.X86_OP_MEM and op.mem.base == capstone.x86.X86_REG_RIP:
                        tgt = ins.address + ins.size + op.mem.disp - self.base
                        s = self.cstring(tgt)
                        note = "  ; rva {:#x}{}".format(tgt, " \"{}\"".format(s) if s else "")
            lines.append("{:#x}: {:<8} {}{}".format(ins.address - self.base, ins.mnemonic, ins.op_str, note))
            if len(lines) >= limit:
                break
        return begin, end, lines

    def calls(self, rva):
        f = self.func_containing(rva)
        begin, end = f if f else (rva, rva + 0x400)
        out = []
        for ins in self.md.disasm(self.data[begin:end], self.base + begin):
            if ins.mnemonic in ("call", "jmp") and ins.operands and ins.operands[0].type == capstone.x86.X86_OP_IMM:
                out.append((ins.address - self.base, ins.mnemonic, ins.operands[0].imm - self.base))
        return out

    def cstring(self, rva):
        if not (0 < rva < len(self.data)):
            return None
        end = self.data.find(b"\0", rva, rva + 120)
        if end <= rva:
            return None
        raw = self.data[rva:end]
        if len(raw) >= 4 and all(32 <= c < 127 for c in raw):
            return raw.decode()
        return None


def main():
    img = Image(sys.argv[1])
    cmd = sys.argv[2]
    if cmd == "classes":
        for name, td in sorted(img.type_descriptors(sys.argv[3]).items()):
            for col, offset in img.complete_object_locators(td):
                for vt, methods in img.vtables_for(col):
                    print("{}  vtable rva {:#x} (this+{:#x}) {} methods".format(name, vt, offset, len(methods)))
                    for i, m in enumerate(methods[:int(sys.argv[4]) if len(sys.argv) > 4 else 12]):
                        f = img.func_containing(m)
                        print("    [{:2d}] {:#x}  size {}".format(i, m, (f[1] - f[0]) if f else "?"))
    elif cmd == "xref":
        rvas = img.string_rvas(sys.argv[3])
        print("string at", [hex(r) for r in rvas])
        for ref in img.lea_xrefs(rvas):
            f = img.func_containing(ref)
            print("  lea at {:#x} in function {}".format(ref, "{:#x}-{:#x}".format(*f) if f else "?"))
    elif cmd == "disasm":
        begin, end, lines = img.disasm(int(sys.argv[3], 16), int(sys.argv[4]) if len(sys.argv) > 4 else 200)
        print("function {:#x}-{:#x}".format(begin, end))
        print("\n".join(lines))
    elif cmd == "vcalls":
        for at, kind, tgt in img.calls(int(sys.argv[3], 16)):
            print("  {:#x}: {} {:#x}".format(at, kind, tgt))


if __name__ == "__main__":
    main()
