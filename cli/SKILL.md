# SKILL: Game Boy dynamic analysis with gbemu

`gbemu` is a headless SameBoy emulator exposed as a JSON-lines stdin/stdout server.
It supports full emulator control and deep runtime analysis. Read `cli/PROTOCOL.md`
for exact schemas; this guide covers recipes and gotchas.

## Quick start (from the repository root)

```sh
make                     # (once) builds build/gbemu
python3 cli/tests/make_test_rom.py build/t.gb build/t.json --sym build/t.sym  # demo ROM
```

Drive it with raw JSON lines via subprocess piping, or use the Python wrapper
`cli/py/gbemu.py`:

```python
import sys; sys.path.insert(0, "cli/py")
from gbemu import GBEmu
with GBEmu("build/gbemu") as g:
    g.load_rom("build/t.gb", model="dmg", boot="builtin", symbols="build/t.sym", seed=1)
    g.run_frames(60)
    print(g.mem_read(0xC000, 8).hex())
```

## Recipes

### Find where a value lives (e.g. health, score, lives)
1. `mem.search.reset`, then play a bit (`run.frames`).
2. `mem.search {"filter": "new == 3"}` (known value) or relative filters:
   lose a life → `mem.search {"filter": "new == old - 1"}`; money increased → `"new > old"`.
   Repeat; `count` shrinks each time. (`wide=true` for 16-bit values.)
3. Verify: `watch.add {"addr": hit, "access": "w"}` then play; it should stop on change.

### ROM literal search (citations, record bytes, art blocks)
```
rom.search {"bytes": "f2 8c 8d fd"}            # whole cart, file order
rom.search {"bytes": "deadbeef", "mask": "ff00ffff"}   # de??beef
rom.search {"bytes": "f3 89 8a", "bank_lo": 24, "bank_hi": 27}
```
Returns `{count, results:[{off, bank, addr}]}` (file offsets). NOTE for
stream-record hunting: served text streams carry inline controls (F0-F2
sticky page prefixes, FA/FC segment markers, FD terminators, FB/E8
fill-fields) — a raw byte needle is the *mechanism*; the caller owns
page-state/context interpretation. Mask (`0`-nibbles) covers single-byte
uncertainty (`f2 8? 8d`).

### Trace what code touches WRAM $C100
```
watch.add {"addr": "$c100", "access": "w", "stop": false}     # count hits, continue
trace.start {"with_mem": true, "max": 1000000}
run.frames {"n": 300}
trace.stop; trace.dump {"limit": 5000}
```
Filter dumped entries for mem writes to c100; entries carry `pc`, `bank`, `symbol`,
`regs`.

### Who calls the routine at $1F3A?
```
break.add {"addr": "$1f3a"}
run.frames {"n": 600}        # runs until the breakpoint fires
backtrace                    # callers, symbolized
regs.get                     # arguments in registers
rewind.pop {"frames": 2}     # time-travel backwards if you overshot
```

### What does this address region's code do?
```
disasm {"addr": "$1f3a", "n": 60}
```
Loads `.sym` via `load_rom {"symbols": ...}` or `symbols.load` for names everywhere
(address params accept symbol strings too: `run.until {"addr": "VBlank"}`).

### Did anything change in memory between A and B?
```
mem.capture {"name": "before"}
run.frames {"n": 120}
mem.diff {"a": "before", "b": "live"}
```
Answers per-region (vram/wram/oam/hram/cart_ram) changed-byte lists.

### Deterministic reproduction
Always pass `seed` to `load_rom` and replay identical command streams; two sessions
with the same seed + inputs are verified bit-identical (except RTC games, which read
wall-clock).

### Screenshots & tiles
`screen.capture {"path": "out.png"}` (or base64), `video.tilemap`, `video.tiles`,
`video.sprites`, `screen.palette` for VRAM-level introspection.

### Validate an ASM routine in CI (no ROM needed)
Use sandbox mode — a bare CPU on a flat 32KB ROM buffer, deterministic and fast:

```python
g.cpu_init(model="dmg", seed=7)                         # fresh bare machine
r = g.cpu_exec(at=0xC000,                               # load bytes + pc=here
               data=bytes([0x3E, 0x0A, 0x06, 0x05, 0x80]),  # ld a,10 / ld b,5 / add a,b
               steps=3)
assert int(r["regs"]["af"], 16) == 0x0F00               # a=15, no flags
assert r["cycles_t"] == 20                              # 8+8+4 T-cycles — timing-exact
```

- `bound` defaults to the written region: escapes stop with `reason == "left_bounds"`.
- HALT stops with `reason == "halted"` (unless `detect_halt=False` — useful with `ime`/`ie` set to test ISRs; RST/int vectors are `RET` sleds by default, `cpu.init(vectors_ret=False)` disables).
- `max_cycles8` caps runaway loops (`reason == "cycle_limit"`); `until_addr` stops at a PC.
- Registers/flags/memory are fully inspectable afterwards via `regs.get` / `mem.read` /
  `trace.dump` (trace stays armed across sandbox execs if started).
- `cpu.reset()` re-zeroes RAM + regs, keeps the loaded code; `cpu.reset(clear_rom=True)` wipes it.

## Gotchas

- Boot ROMs: `boot:"builtin"` needs no files (DMG only). Real boots live in
  `cli/bootroms/`; per-model defaults are auto-selected (`dmg_boot.bin`, ...).
- CGB games on `model:"cgb"` run the full CGB boot animation (~120 frames before game code).
- Watchpoints fire **after** the executing instruction; breakpoints **before**. Combine
  a count-only watchpoint (`stop:false`) with the trace ring to log writes; combine
  breakpoints with `backtrace` for provenance.
- `mem.read` on the CPU view is side-effect-free (safe for IO registers);
  `mem.write` applies immediate effect.
- Trace entries' `bank` is the fetch bank (ROM); `mem` entries are capped at 4/instruction.
- `run.until {"expr": ...}` evaluates a SameBoy expression every instruction — slow but
  precise; prefer `addr` when possible.
- Snapshot slots (32) & mem captures (8) are in-memory; use `path` variants to persist.
- Symbols: rgbds `.sym` files (`BB:AAAA name`). After new symbols load, re-run `trace.dump`
  to see names on existing entries.
