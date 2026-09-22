#!/usr/bin/env python3
"""Generate a deterministic 32KB Game Boy test ROM for gbemu smoke tests.

Program behaviour:
  - draws a solid-color tile region into the BG map (screen non-blank)
  - main loop: ($C000)++; mirror to $C001; read joypad -> $C010; call sub
  - sub: ($C100) = ($C000) + 1; ret
Per loop iteration, exactly 14 instructions execute and $C000 increments by 1
(with interrupts disabled, timings are fully deterministic).

Usage: make_test_rom.py OUT.gb OUT.json [--sym OUT.sym]
OUT.json carries label addresses + constants for the smoke test.
"""
import json
import struct
import sys

NINTENDO_LOGO = bytes([
    0xCE, 0xED, 0x66, 0x66, 0xCC, 0x0D, 0x00, 0x0B, 0x03, 0x73, 0x00, 0x83,
    0x00, 0x0C, 0x00, 0x0D, 0x00, 0x08, 0x11, 0x1F, 0x88, 0x89, 0x00, 0x0E,
    0xDC, 0xCC, 0x6E, 0xE6, 0xDD, 0xDD, 0xD9, 0x99, 0xBB, 0xBB, 0x67, 0x63,
    0x6E, 0x0E, 0xEC, 0xCC, 0xDD, 0xDC, 0x99, 0x9F, 0xBB, 0xB9, 0x33, 0x3E,
])


class Asm:
    def __init__(self, buf, org):
        self.buf = buf
        self.pc = org
        self.labels = {}
        self.fixups = []  # (kind, pos, label)

    def org(self, org):
        self.pc = org

    def label(self, name):
        self.labels[name] = self.pc

    def b(self, *bs):
        for v in bs:
            self.buf[self.pc] = v & 0xFF
            self.pc += 1

    def w(self, v):
        self.b(v & 0xFF, (v >> 8) & 0xFF)

    def _addr(self, label):
        if isinstance(label, int):
            self.w(label)
        else:
            self.fixups.append(("w", self.pc, label))
            self.w(0)

    def _rel(self, label):
        if isinstance(label, int):
            off = label - (self.pc + 1)
            self.b(off)
        else:
            self.fixups.append(("r", self.pc, label))
            self.b(0)

    # instruction set used by the test rom
    def nop(self): self.b(0x00)
    def di(self): self.b(0xF3)
    def xor_a(self): self.b(0xAF)
    def ret(self): self.b(0xC9)
    def halt(self): self.b(0x76)
    def jp(self, a): self.b(0xC3); self._addr(a)
    def jr(self, a): self.b(0x18); self._rel(a)
    def jr_nz(self, a): self.b(0x20); self._rel(a)
    def call(self, a): self.b(0xCD); self._addr(a)
    def ld_sp(self, v): self.b(0x31); self.w(v)
    def ld_hl(self, v): self.b(0x21); self.w(v)
    def ld_a(self, v): self.b(0x3E); self.b(v)
    def ld_b(self, v): self.b(0x06); self.b(v)
    def ld_c(self, v): self.b(0x0E); self.b(v)
    def inc_a(self): self.b(0x3C)
    def inc_hli(self): self.b(0x34)  # inc (hl)
    def dec_b(self): self.b(0x05)
    def ld_hli_a(self): self.b(0x22)
    def ld_a_hli(self): self.b(0x2A)
    def ld_a_hl(self): self.b(0x7E)
    def ld_mem_a(self, a): self.b(0xEA); self._addr(a)
    def ld_a_mem(self, a): self.b(0xFA); self._addr(a)
    def ldh_mem_a(self, a): self.b(0xE0); self.b(a)
    def ldh_a_mem(self, a): self.b(0xF0); self.b(a)
    def ld_hl_mem_a(self): self.b(0x77)

    def finalize(self):
        for kind, pos, label in self.fixups:
            target = self.labels[label]
            if kind == "w":
                self.buf[pos] = target & 0xFF
                self.buf[pos + 1] = (target >> 8) & 0xFF
            else:
                off = target - (pos + 1)
                assert -128 <= off <= 127, f"jr out of range to {label}"
                self.buf[pos] = off & 0xFF


def build():
    rom = bytearray(0x8000)
    a = Asm(rom, 0)

    a.org(0x100)
    a.label("entry")
    a.nop()
    a.jp("main")

    # builtin-dmg boot ROM continues here after unmapping itself (see agent_ext.c)
    a.org(0x15)
    a.jp(0x100)

    # header
    a.org(0x104)
    rom[0x104:0x134] = NINTENDO_LOGO
    title = b"GBEMUTEST" + b"\0" * 6  # exactly 15 bytes (0x134-0x142)
    rom[0x134:0x143] = title
    rom[0x143] = 0x00  # no CGB
    rom[0x146] = 0x00  # no SGB
    rom[0x147] = 0x00  # ROM ONLY
    rom[0x148] = 0x00  # 32KB
    rom[0x149] = 0x00  # no cart RAM
    rom[0x14C] = 0x00

    a.org(0x150)
    a.label("main")
    a.di()
    a.ld_sp(0xE000)
    a.xor_a()
    a.ldh_mem_a(0x40)          # LCD off
    # tile 1 = solid colour-3 block at $8010
    a.ld_hl(0x8010)
    a.ld_b(16)
    a.label("tile_loop")
    a.ld_a(0xFF)
    a.ld_hli_a()
    a.dec_b()
    a.jr_nz("tile_loop")
    # first 4 rows of BG map ($9800) = tile 1
    a.ld_hl(0x9800)
    a.ld_b(0x80)
    a.label("map_loop")
    a.ld_a(1)
    a.ld_hli_a()
    a.dec_b()
    a.jr_nz("map_loop")
    a.ld_a(0xE4)
    a.ldh_mem_a(0x47)          # BGP
    a.ld_a(0x91)
    a.ldh_mem_a(0x40)          # LCD on, BG on
    a.xor_a()
    a.ld_hl(0xC000)
    a.ld_hl_mem_a()            # ($C000) = 0

    a.label("loop")
    a.ld_hl(0xC000)
    a.inc_hli()                # ($C000)++
    a.ld_a_hl()
    a.ld_mem_a(0xC001)         # mirror
    a.ld_a(0x20)
    a.ldh_mem_a(0x00)          # select dpad
    a.ldh_a_mem(0x00)
    a.ld_mem_a(0xC010)         # joypad state -> $C010
    a.call("sub")
    a.jp("loop")

    a.label("sub")
    a.ld_a_mem(0xC000)
    a.inc_a()
    a.ld_mem_a(0xC100)
    a.ret()

    a.finalize()

    # header checksum
    x = 0
    for i in range(0x134, 0x14D):
        x = (x - rom[i] - 1) & 0xFF
    rom[0x14D] = x
    # global checksum
    g = 0
    for i in range(0x8000):
        if i not in (0x14E, 0x14F):
            g = (g + rom[i]) & 0xFFFF
    rom[0x14E] = (g >> 8) & 0xFF
    rom[0x14F] = g & 0xFF

    return bytes(rom), a.labels


def main():
    out_gb = sys.argv[1] if len(sys.argv) > 1 else "test_rom.gb"
    out_json = sys.argv[2] if len(sys.argv) > 2 else "test_rom.json"
    out_sym = None
    if "--sym" in sys.argv:
        out_sym = sys.argv[sys.argv.index("--sym") + 1]

    rom, labels = build()
    with open(out_gb, "wb") as f:
        f.write(rom)

    meta = {
        "labels": {k: v for k, v in labels.items()},
        "loop_instr_count": 14,
        "counter_addr": 0xC000,
        "mirror_addr": 0xC001,
        "submirror_addr": 0xC100,
        "joy_addr": 0xC010,
    }
    with open(out_json, "w") as f:
        json.dump(meta, f, indent=2)

    if out_sym:
        with open(out_sym, "w") as f:
            for name, addr in sorted(labels.items(), key=lambda kv: kv[1]):
                f.write(f"00:{addr:04X} {name}\n")

    print(f"wrote {out_gb} ({len(rom)} bytes), {out_json}, labels: {labels}")


if __name__ == "__main__":
    main()
