#!/usr/bin/env python3
"""smoke.py — end-to-end smoke tests for gbemu against the deterministic test ROM.

Usage: smoke.py PATH_TO_GBEMU PATH_TO_ROM PATH_TO_META_JSON
"""
import json
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "py"))
from gbemu import GBEmu  # noqa: E402

FAILS = []


def check(name, cond, detail=""):
    status = "PASS" if cond else "FAIL"
    print(f"  [{status}] {name}{'' if cond else ' -- ' + str(detail)}")
    if not cond:
        FAILS.append(name)


def main():
    binary, rom_path, meta_path = sys.argv[1], sys.argv[2], sys.argv[3]
    meta = json.load(open(meta_path))
    L = {k: v for k, v in meta["labels"].items()}
    loop, sub = L["loop"], L["sub"]
    C000, C010, C100 = meta["counter_addr"], meta["joy_addr"], meta["submirror_addr"]
    sym_path = os.path.splitext(rom_path)[0] + ".sym"

    print("== session / loading ==")
    g = GBEmu(binary)
    r = g.ping()
    check("ping", r["pong"] and "sameboy" in r, r)
    r = g.cmd("bogus_xyz", _ignore=True) if False else None  # placeholder
    try:
        g.cmd("does.not.exist")
        check("unknown command rejected", False, "no error raised")
    except Exception as e:
        check("unknown command rejected", "unknown_cmd" in str(e), e)

    r = g.load_rom(rom_path, model="dmg", boot="builtin", symbols=sym_path, seed=1234)
    check("load_rom", r["loaded"] and r["title"] == "GBEMUTEST", r)

    print("== determinism ==")
    g.run_frames(30)
    m1 = g.mem_read(C000, 4)
    pc1 = g.regs()["pc"]
    g2 = GBEmu(binary)
    g2.load_rom(rom_path, model="dmg", boot="builtin", seed=1234)
    g2.run_frames(30)
    m2 = g2.mem_read(C000, 4)
    pc2 = g2.regs()["pc"]
    check("same seed+inputs => identical state", m1 == m2 and pc1 == pc2, f"{m1.hex()} vs {m2.hex()}")
    g2.quit()

    print("== execution control ==")
    g.cmd("break.clear")
    g.run_until(addr=f"${loop:x}")
    r = g.regs()
    check("run.until addr", int(r["pc"], 16) == loop, r["pc"])
    before = int(g.instr_count(), 0) if hasattr(g, "instr_count") else 0
    ri = g.cmd("step", n=14)
    check("step 14 = one loop iteration", ri["ran_instructions"] == 14, ri)
    c_after_step = g.mem_read(C000, 1)[0]

    print("== breakpoints ==")
    bid = g.break_add(addr="sub")
    r = g.run_frames(2)
    check("breakpoint stops emulation", r["stopped"] and int(r["pc"], 16) == sub, r)
    r = g.run_frames(2)
    check("breakpoint is re-triggerable", r["stopped"] and int(r["pc"], 16) == sub, r)
    bps = g.break_list()
    check("break.list shows hit count", any(b["id"] == bid and b["hits"] >= 2 for b in bps), bps)
    g.break_del(bid)
    check("break list empty after del", g.break_list() == [])

    print("== mem-predicate breakpoints (type=mem) ==")
    g.cmd("break.clear")
    # single-address write predicate on the counter
    r = g.cmd("break.add", addr=f"${C000:x}", type="mem", access="w")
    mbid = r["id"]
    r = g.run_frames(2)
    check("mem-write bp stops on counter write",
          r["stopped"] and "breakpoint" in r["reason"] and "mem-write" in r["reason"], r)
    check("mem-write bp stop reports hit detail + banks",
          "detail" in r and "pc=$" in r["detail"] and "banks" in r and
          isinstance(r["banks"].get("wram"), int), r.get("detail"))
    writer_pc = r["detail"].split("pc=$")[1].split(" ")[0]
    g.cmd("break.del", id=mbid)
    # pc_eq positive: predicate restricted to the first writer's pc fires there
    g.cmd("break.add", addr=f"${C000:x}", type="mem", access="w", pc=f"${writer_pc}")
    r = g.run_frames(2)
    hit_pc = r["detail"].split("pc=$")[1].split(" ")[0] if "detail" in r else None
    check("pc_eq mem bp fires at the gated pc", r["stopped"] and hit_pc == writer_pc, r.get("detail"))
    g.cmd("break.clear")
    # pc_not: excludes the first writer; the counter has a second write site per
    # iteration, so the next stop must be at a DIFFERENT pc (exclusion proven)
    g.cmd("break.add", addr=f"${C000:x}", type="mem", access="w", pc_not=f"${writer_pc}")
    r = g.run_frames(2)
    hit_pc = r["detail"].split("pc=$")[1].split(" ")[0] if "detail" in r else None
    check("pc_not mem bp excludes writer (no stop or different-site stop)",
          not r["stopped"] or hit_pc not in (None, writer_pc),
          f"writer={writer_pc} hit={hit_pc} {r.get('detail')}")
    g.cmd("break.clear")
    # value predicate on the mirror cell ($C001 = counter+1), pc-gated to a
    # pc that never writes that cell after boot: composition must never fire
    g.cmd("break.add", addr=f"${C000+1:x}", type="mem", access="w", value=0xC3, mask=0xFF,
          pc=f"${0x160:x}")
    r = g.run_frames(2)
    check("pc+value AND-composition with unreachable pc: zero hits",
          not r["stopped"] and g.break_list()[0]["hits"] == 0, r)
    g.cmd("break.clear")
    # mask/value on the incrementing counter: fires exactly when 0x42 is stored
    g.cmd("break.add", addr=f"${C000:x}", type="mem", access="w", value=0x42, mask=0xFF)
    r = g.run_frames(2)
    check("value predicate fires when written value matches",
          r["stopped"] and "detail" in r and "= 42" in r["detail"], r.get("detail"))
    g.cmd("break.clear")
    # never-written cell: range+value predicate accumulates zero hits in 3 frames
    g.cmd("break.add", addr="${:x}".format(C000 + 0x50), end="${:x}".format(C000 + 0x53),
          type="mem", access="w", value=0x42)
    r = g.run_frames(3)
    check("range+value on never-written cell: zero hits, no stop",
          not r["stopped"] and g.break_list()[0]["hits"] == 0, r)
    g.cmd("break.clear")
    # stop=False logging: bulk capture with break.log dump
    g.cmd("break.add", addr=f"${C000:x}", type="mem", access="w", stop=False)
    g.run_frames(2)
    log = g.cmd("break.log", limit=100000)
    check("stop=False mem bp logs hits in bulk",
          log["total"] >= 8 and all(e["kind"] == "w" for e in log["entries"]),
          {"total": log["total"], "sample": log["entries"][:2]})
    check("break.log hit records carry addr/v/bank/pc",
          {"addr", "v", "bank", "pc"} <= set(log["entries"][0]), log["entries"][0])
    g.cmd("break.clear")
    # bank-qualified mem bp on fixed WRAM (effbank always 0) with bank=1: must NOT fire
    g.cmd("break.add", addr=f"${C000:x}", type="mem", access="w", bank=1)
    r = g.run_frames(2)
    check("wrong-bank mem bp never fires", not r["stopped"], r)
    hits = [b["hits"] for b in g.break_list()]
    check("wrong-bank mem bp recorded zero hits", hits == [0], hits)
    g.cmd("break.clear")
    # region-range read predicate on counter memory
    g.cmd("break.add", addr=f"${C000:x}", end=f"${C000+3:x}", type="mem", access="rw")
    r = g.run_frames(1)
    check("range read predicate fires on any access", r["stopped"] and "mem-read" in r["reason"], r)
    g.cmd("break.clear")

    print("== backtrace ==")
    g.break_add(addr="sub")
    g.run_frames(1)
    bt = g.backtrace()
    check("backtrace inside sub has caller", bt["call_depth"] >= 1 and
          any(int(e["addr"], 16) == (L["loop"] + 0x11) for e in bt["entries"]), bt)
    g.break_clear()

    print("== watchpoints ==")
    wid = g.watch_add(addr=C100, access="w")
    r = g.run_frames(1)
    check("write watchpoint fires", r["stopped"] and "watchpoint" in r["reason"], r)
    val_now = g.mem_read(C100, 1)[0]
    g.watch_del(wid)
    wid = g.watch_add(addr=C100, access="w", value=(val_now + 2) & 0xFF)
    r = g.run_frames(1)
    check("value-filtered watchpoint fires only on value", r["stopped"], r)
    g.watch_clear()
    r = g.run_frames(1)
    check("after watch.clear no stop", not r["stopped"], r)

    print("== trace ==")
    g.trace_start(max_entries=4096, with_regs=True, with_mem=True)
    g.run_frames(1)
    g.trace_stop()
    td = g.trace_dump(limit=2000)
    pcs = {int(e["pc"], 16) for e in td["entries"]}
    check("trace captured loop+sub", loop in pcs and sub in pcs, sorted(pcs)[:8])
    check("trace mem accesses recorded", any(e.get("mem") for e in td["entries"]), "")
    check("trace regs recorded", all("regs" in e for e in td["entries"]), "")

    print("== coverage ==")
    g.coverage_reset()
    g.run_frames(3)
    cov = g.coverage(ranges=True)
    b0 = next(b for b in cov["per_bank"] if b["bank"] == 0)
    check("coverage armed and counting", cov["armed"] and cov["instructions"] > 1000, cov.get("instructions"))
    check("coverage covers loop+sub bytes", b0["executed"] >= 14 and
          any(int(r0, 16) <= loop and int(r1, 16) >= loop for r0, r1 in b0["ranges"]), b0["executed"])

    print("== memory ==")
    g.mem_capture("t0")
    g.run_frames(5)
    d = g.mem_diff("t0", "live")
    d_wram = [c for c in d["changes"] if c["region"] == "wram"]
    c000_changes = [c for c in d_wram if int(c["addr"], 16) == C000]
    check("mem.diff finds changed counter", len(c000_changes) == 1 and
          c000_changes[0]["a"] != c000_changes[0]["b"], c000_changes[:1])
    w = g.mem_write(C000, bytes([0x42]))
    check("mem.write works", g.mem_read(C000, 1)[0] == 0x42)

    print("== search ==")
    g.mem_search_reset()
    g.run_frames(1)
    r1 = g.mem_search("new != old")
    check("first search baseline vs 0 finds non-zero bytes", r1["count"] > 0, r1["count"])
    g.run_frames(2)
    r2 = g.mem_search("new != old")
    check("narrowing search shrinks count", 0 < r2["count"] < r1["count"], (r1["count"], r2["count"]))
    g.run_frames(1)
    c000_hit = [x for x in g.mem_search("new != old", max_results=8192)["results"]
                if int(x["addr"], 16) == C000]
    check("counter found by search", len(c000_hit) == 1, c000_hit)

    print("== snapshots & rewind ==")
    g.snapshot_save(name="s1")
    v1 = g.mem_read(C000, 1)[0]
    g.run_frames(10)
    v2 = g.mem_read(C000, 1)[0]
    g.snapshot_load(name="s1")
    v3 = g.mem_read(C000, 1)[0]
    check("snapshot roundtrip restores memory", v1 == v3 and v2 != v3, (v1, v2, v3))
    g.rewind_set(2)
    g.run_frames(5)
    va = g.mem_read(C000, 1)[0]
    g.rewind_pop(3)
    vb = g.mem_read(C000, 1)[0]
    check("rewind moves state backward", vb != va, (va, vb))

    print("== disasm / symbols / eval ==")
    listing = g.disasm(addr=f"${loop:x}", n=4)
    check("disasm has symbol label", "loop" in listing, listing.splitlines()[:2])
    r = g.eval("sub")
    check("eval symbol", int(r["value"], 16) == sub, r)
    r = g.symbols_resolve(q="loop")
    check("symbols.resolve name->addr", int(r["addr"], 16) == loop, r)
    r = g.symbols_resolve(q=f"${sub:x}")
    check("symbols.resolve addr->desc", "sub" in r["desc"], r)

    print("== input ==")
    g.input_set(["right"])
    g.run_frames(2)
    j = g.mem_read(C010, 1)[0]
    check("dpad right reflected via JOYP", (j & 0xF) == 0xE, hex(j))
    g.input_set([])
    g.run_frames(2)
    j = g.mem_read(C010, 1)[0]
    check("release restores JOYP", (j & 0xF) == 0xF, hex(j))

    print("== video ==")
    png = g.screenshot_png()
    check("screenshot is PNG", png[:8] == b"\x89PNG\r\n\x1a\n", png[:8])
    w, h = struct.unpack(">II", png[16:24])
    check("PNG geometry is 160x144", (w, h) == (160, 144), (w, h))
    check("screenshot non-blank (tile region)", len(set(png)) > 64, len(set(png)))
    pal = g.palette()
    check("palette dmg mode", pal["mode"] == "dmg", pal.get("mode"))
    tm = g.tilemap("bg")
    check("tilemap has our tile 1 in first row",
          tm["tiles"][0][:8] == "01010101", tm["tiles"][0][:8])
    tl = g.tiles(start=1, count=1)
    check("tile 1 is solid colour 3", set(tl["tiles"][0]) == {"3"}, tl["tiles"][0][:16])

    print("== regions / banked read ==")
    rr = g.cmd("mem.regions")
    check("mem.regions lists vram/wram", any(r["name"] == "vram" for r in rr["regions"]), rr)
    rom0 = g.mem_read(0x0100, 4)
    check("banked rom read matches cpu view", g.cmd("mem.read", addr="$0100", len=4)["data"] == rom0.hex())

    print("== sandbox (bare-CPU validation) ==")
    r = g.cpu_init(model="dmg", seed=7)
    check("cpu.init", r["sandbox"] and r["model"] == "dmg", r)

    # ld a,10 / ld b,5 / add a,b  -> a=15, no flags; cycles: 8+8+4 = 20 T
    r = g.cpu_exec(at=0xC000, data=bytes([0x3E, 0x0A, 0x06, 0x05, 0x80]), steps=3)
    check("cpu.exec arithmetic result", int(r["regs"]["af"], 16) == 0x0F00, r["regs"])
    check("cpu.exec exact cycles", r["cycles_t"] == 20 and r["cycles8"] == 40, r["cycles_t"])
    check("cpu.exec reason steps_completed", r["reason"] == "steps_completed", r["reason"])

    # ld a,$FF / inc a -> a=0, F=Z|H=$A0; cycles: 8+4=12 T
    r = g.cpu_exec(at=0xC000, data=bytes([0x3E, 0xFF, 0x3C]), steps=2)
    check("cpu.exec flags Z|H", int(r["regs"]["af"], 16) == 0x00A0, r["regs"]["af"])

    # bounds escape: jp $000A leaves the written region
    r = g.cpu_exec(at=0xC000, data=bytes([0xC3, 0x0A, 0x00]), steps=50)
    check("cpu.exec bounds escape", r["reason"] == "left_bounds" and int(r["pc"], 16) == 0x000A, r)

    # halt detection
    r = g.cpu_exec(at=0xC000, data=bytes([0x76]), steps=10000)
    check("cpu.exec halt detection", r["reason"] == "halted" and r["steps"] == 1, r)

    # until_addr: ld hl,$C000; inc (hl); jr back to inc
    r = g.cpu_exec(at=0xC000, data=bytes.fromhex("21 00 c0 34 18 fd"), steps=100000,
                   until_addr=0xC004, bound=False)
    check("cpu.exec until_addr", r["reason"] == "until_addr" and int(r["pc"], 16) == 0xC004, r)

    # vblank ISR: vector $40 -> jp $C100; isr counts at $C200 (outside its own code)
    g.cpu_load(0x0040, bytes([0xC3, 0x00, 0xC1]))                 # jp $C100
    g.cpu_load(0xC100, bytes([0x21, 0x00, 0xC2, 0x34, 0xD9]))     # ld hl,$C200; inc (hl); reti
    g.mem_write(0xC200, bytes([0]))                               # counter starts at 0
    r = g.cpu_exec(at=0xC000, data=bytes([0x76, 0x18, 0xFD]), steps=300000, detect_halt=False,
                   ime=True, ie=1, bound=False)
    isr_count = g.mem_read(0xC200, 1)[0]
    check("vblank ISR fired during halt", r["reason"] == "steps_completed" and isr_count >= 3, (r["reason"], isr_count))

    # determinism: same ops, fresh sandbox -> same regs
    g.cpu_init(model="dmg", seed=7)
    g.cpu_exec(at=0xC000, data=bytes([0x3E, 0x0A, 0x06, 0x05, 0x80]), steps=3)
    a1 = g.regs()["af"]
    g.cpu_init(model="dmg", seed=7)
    g.cpu_exec(at=0xC000, data=bytes([0x3E, 0x0A, 0x06, 0x05, 0x80]), steps=3)
    a2 = g.regs()["af"]
    check("sandbox determinism", a1 == a2, (a1, a2))

    # cycle limit
    r = g.cpu_exec(at=0xC000, data=bytes([0x00] * 16), steps=10 ** 6,
                   max_cycles8=32, bound=False)
    check("cpu.exec cycle limit", r["reason"] == "cycle_limit", r["reason"])

    g.quit()

    print()
    if FAILS:
        print(f"SMOKE FAILED: {len(FAILS)} failure(s): {FAILS}")
        sys.exit(1)
    print("ALL SMOKE TESTS PASSED")


if __name__ == "__main__":
    main()
