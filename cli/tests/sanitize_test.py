#!/usr/bin/env python3
"""sanitize_test.py — sandbox tests for the sanitize.* runtime sanitizer.

Every class gets a deliberately buggy snippet (must fire) and, where the
class has a correct idiom, a clean control (must stay 0).

Usage: sanitize_test.py PATH_TO_GBEMU
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "py"))
from gbemu import GBEmu  # noqa: E402

FAILS = []


def check(name, cond, detail=""):
    print(f"  [{'PASS' if cond else 'FAIL'}] {name}{'' if cond else ' -- ' + str(detail)}")
    if not cond:
        FAILS.append(name)


def run(g, code, steps, at=0x0150, classes=None, **kw):
    g.cpu_init(model="dmg", seed=7)
    p = {"selfid": False}
    if classes:
        p["classes"] = classes
    p.update(kw)
    g.cmd("sanitize.start", **p)
    g.cpu_exec(at=at, data=bytes(code), steps=steps, bound=False, sp="$dff0")
    r = g.cmd("sanitize.report", max_sites=50, max_events=20)
    g.cmd("sanitize.stop")
    return r


def main():
    g = GBEmu(sys.argv[1])
    print("== sanitize ==")

    # VRAM: tight ldi loop with the LCD on -> some writes land in mode 3
    burst = [0x21, 0x00, 0x80, 0x06, 0x00,          # ld hl,$8000 ; ld b,0
             0x22, 0x05, 0x20, 0xFC,                # .l: ld [hl+],a ; dec b ; jr nz,.l
             0x18, 0xFE]                            # jr $
    r = run(g, burst, 2000)
    check("vram_write_blocked fires on an unguarded burst",
          r["counts"]["vram_write_blocked"] > 0, r["counts"])
    s = [x for x in r["sites"] if x["class"] == "vram_write_blocked"]
    check("vram site attributed to the writing pc ($0155)",
          bool(s) and s[0]["pc"] == "0155" and s[0]["mode"] == 3, s[:1])

    # control: STAT mode-0/1 wait per byte (the game's $0450 idiom)
    guarded = [0x21, 0x00, 0x80, 0x06, 0x00,        # ld hl,$8000 ; ld b,0
               0xF3,                                # .l: di
               0xF0, 0x41, 0xCB, 0x4F, 0x20, 0xFA,  # .w: ldh a,[$41]; bit 1,a; jr nz,.w
               0x3E, 0x55, 0x22, 0xFB,              # ld a,$55; ld [hl+],a; ei
               0x05, 0x20, 0xF1,                    # dec b ; jr nz,.l
               0x18, 0xFE]
    r = run(g, guarded, 6000)
    check("STAT-guarded copy is clean", r["counts"]["vram_write_blocked"] == 0, r["counts"])

    # VRAM read in mode 3
    rd = [0x21, 0x00, 0x80, 0x06, 0x00, 0x2A, 0x05, 0x20, 0xFC, 0x18, 0xFE]
    r = run(g, rd, 2000)
    check("vram_read_blocked fires on an unguarded read burst",
          r["counts"]["vram_read_blocked"] > 0, r["counts"])

    # MBC stray / echo / unusable
    stray = [0x3E, 0x01, 0xEA, 0x00, 0x60,          # ld [$6000],a
             0xEA, 0x00, 0x30,                      # ld [$3000],a (bit 8 set)
             0xEA, 0x10, 0xE0,                      # ld [$E010],a (echo)
             0xEA, 0xB0, 0xFE,                      # ld [$FEB0],a (unusable)
             0x18, 0xFE]
    r = run(g, stray, 12)
    check("mbc_stray: $6000 + $3000 writes", r["counts"]["mbc_stray"] == 2, r["counts"])
    check("echo_write: echo + unusable", r["counts"]["echo_write"] == 2, r["counts"])

    # ret_mismatch: callee overwrites its own return address in place
    #   $0150: call $0160 ; jr $
    #   $0160: inc sp; inc sp; ld hl,$0170; push hl; ret     -> resumes $0170
    #   $0170: jr $
    code = bytearray(0x30)
    code[0:5] = bytes([0xCD, 0x60, 0x01, 0x18, 0xFE])
    code[0x10:0x18] = bytes([0x33, 0x33, 0x21, 0x70, 0x01, 0xE5, 0xC9, 0x00])
    code[0x20:0x22] = bytes([0x18, 0xFE])
    r = run(g, code, 12)
    s = [x for x in r["sites"] if x["class"] == "ret_mismatch"]
    check("ret_mismatch: overwritten return address",
          r["counts"]["ret_mismatch"] == 1 and s and s[0]["addr"] == "0153"
          and s[0]["value"] == "0170", (r["counts"], s))

    # control: balanced call/ret
    code = bytearray(0x20)
    code[0:5] = bytes([0xCD, 0x60, 0x01, 0x18, 0xFE])
    code[0x10:0x13] = bytes([0xC5, 0xC1, 0xC9])      # push bc; pop bc; ret
    r = run(g, code, 12)
    check("balanced call/ret is clean", r["counts"]["ret_mismatch"] == 0
          and r["rets_unmatched"] == 0, r)

    # exec_ram + illegal opcode
    r = run(g, [0xC3, 0x00, 0xC0], 3)   # jp $C000 (WRAM = zeros -> nops)
    check("exec_ram: jump into WRAM", r["counts"]["exec_ram"] >= 1, r["counts"])
    r = run(g, [0x00, 0xD3], 3)
    check("illegal_opcode: $D3", r["counts"]["illegal_opcode"] == 1, r["counts"])

    # sp_range
    r = run(g, [0x31, 0x00, 0xC1, 0x18, 0xFE], 4, sp_lo="$d000", sp_hi="$dfff")
    check("sp_range: stack moved outside the window", r["counts"]["sp_range"] == 1, r["counts"])

    # stop_on: the run halts at the first event with a detail record
    g.cpu_init(model="dmg", seed=7)
    g.cmd("sanitize.start", stop_on=["mbc_stray"])
    x = g.cpu_exec(at=0x0150, data=bytes(stray), steps=12, bound=False)
    check("stop_on halts the run", "sanitizer" in str(x.get("reason", "")), x)
    g.cmd("sanitize.stop")

    # class filter
    r = run(g, stray, 12, classes=["echo_write"])
    check("class filter", r["counts"]["mbc_stray"] == 0 and r["counts"]["echo_write"] == 2, r["counts"])

    # freeze: once a stop_on event fires, later run commands execute nothing
    g.cpu_init(model="dmg", seed=7)
    g.cmd("sanitize.start", stop_on=["mbc_stray"], freeze=True)
    x1 = g.cpu_exec(at=0x0150, data=bytes(stray), steps=12, bound=False)
    pc1 = g.cmd("regs.get")["pc"]
    x2 = g.cmd("step", n=5)
    rep = g.cmd("sanitize.report", max_sites=1, max_events=1)
    check("freeze: the second run executes nothing and re-reports the stop",
          x2.get("ran_instructions") == 0 and x2.get("stopped")
          and "frozen" in str(x2.get("reason")) and g.cmd("regs.get")["pc"] == pc1
          and rep["frozen"], (x1.get("reason"), x2, pc1))
    g.cmd("sanitize.clear")
    g.cmd("step", n=1)
    check("freeze: sanitize.clear releases the session (execution advances)",
          g.cmd("regs.get")["pc"] != pc1, pc1)
    g.cmd("sanitize.stop")

    # trace.dump tail/offset windows
    g.cpu_init(model="dmg", seed=7)
    g.cmd("trace.start", max=64)
    g.cpu_exec(at=0x0150, data=bytes([0x00] * 20 + [0x18, 0xFE]), steps=10, bound=False)
    head = g.cmd("trace.dump", limit=2)["entries"]
    tail = g.cmd("trace.dump", limit=2, tail=True)["entries"]
    tail_off = g.cmd("trace.dump", limit=2, tail=True, offset=2)["entries"]
    check("trace.dump default = oldest entries", [e["pc"] for e in head] == ["0150", "0151"], head)
    check("trace.dump tail = newest entries", [e["pc"] for e in tail] == ["0158", "0159"], tail)
    check("trace.dump tail+offset pages back", [e["pc"] for e in tail_off] == ["0156", "0157"], tail_off)
    g.cmd("trace.stop")

    # count-only PC breakpoint: counts every hit, never halts
    g.cpu_init(model="dmg", seed=7)
    loop = [0x3C, 0x18, 0xFD]                      # .l: inc a ; jr .l
    g.cpu_load(0x0150, loop)
    g.cmd("regs.set", pc="$0150")
    g.cmd("break.add", addr="$0150", stop=False)
    r = g.cmd("step", n=30)
    bl = g.cmd("break.list")["breakpoints"][0]
    check("count-only PC bp does not stop", r.get("ran_instructions") == 30 and not r.get("stopped"), r)
    check("count-only PC bp counts hits", bl["hits"] == 15, bl)
    g.cmd("break.clear")
    g.cmd("regs.set", pc="$0150")
    g.cmd("break.add", addr="$0150")
    r = g.cmd("step", n=30)
    check("default PC bp still stops", r.get("stopped") and r.get("ran_instructions", 99) < 30, r)
    g.cmd("break.clear")

    try:
        g.cmd("sanitize.start", classes=["nope"])
        check("unknown class rejected", False, "no error")
    except Exception:
        check("unknown class rejected", True)
    g.quit()
    print("\nALL SANITIZE TESTS PASSED" if not FAILS else f"\nFAILED: {FAILS}")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
