# gbemu — agent-facing headless SameBoy

A CLI that turns the SameBoy Game Boy emulator core into a **stateful dynamic-analysis
service for AI agents and scripts**. One long-lived process reads JSON commands (one per
line) on stdin and answers in JSON on stdout — step/run execution, breakpoints,
watchpoints, execution+memory-access tracing, call backtraces, ROM coverage, snapshots,
time-travel rewind, memory diff/search, symbol-aware disassembly, scripted input, and
PNG screenshots.

Built on the unmodified SameBoy 1.0.3 Core (vendored under `SameBoy/` — see
`VENDORED.md`). See `PROTOCOL.md` for the full command reference and `SKILL.md`
for agent-oriented analysis recipes.

## Build

> macOS toolchain trap (observed 2026-09-28, CLT clang 17 + MacOSX27.0 SDK):
> the link fails with `tapi error: ... unknown architecture` in the SDK's
> `.tbd` stubs, and the failed link DELETES the existing `build/gbemu` that
> downstream harnesses run. Link against an older installed SDK instead:
> `SDKROOT=/Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk make`.

```sh
make              # builds build/gbemu from SameBoy/Core + cli/src (needs clang/gcc + libz)
make bootroms     # vendors boot ROM binaries into cli/bootroms (RGBDS, or download fallback)
make test         # builds a deterministic test ROM and runs the smoke + sanitizer suites (python3)
```

Boot ROMs: `dmg_boot.bin` etc. are looked up in `cli/bootroms/` (override with
`--bootrom-dir` or `GBEMU_BOOTROM_DIR`). For tests/CI, `boot:"builtin"` uses the
embedded minimal DMG boot ROM — no files needed.

## Usage

```sh
./build/gbemu                     # JSON-lines REPL on stdin
./build/gbemu --script run.jsonl  # replay a command script, then exit
```

Example session (each line sent to stdin):

```json
{"id":1,"cmd":"load_rom","params":{"path":"pokemon.gb","model":"dmg","symbols":"pokemon.sym","seed":42}}
{"id":2,"cmd":"run.frames","params":{"n":600}}
{"id":3,"cmd":"break.add","params":{"addr":"$3d7e"}}
{"id":4,"cmd":"run.until","params":{"expr":"$c3e0 > 0","max_instructions":5000000}}
{"id":5,"cmd":"backtrace"}
{"id":6,"cmd":"mem.read","params":{"addr":"$c100","len":64}}
{"id":7,"cmd":"screen.capture","params":{"path":"shot.png"}}
{"id":8,"cmd":"quit"}
```

### Python

```python
import sys; sys.path.insert(0, "cli/py")
from gbemu import GBEmu

with GBEmu("build/gbemu") as g:
    g.load_rom("game.gb", model="dmg", boot="builtin")
    g.run_frames(60)
    g.watch_add(addr=0xC000, access="w")
    r = g.run_frames(10)
    if r["stopped"]:
        print(r["reason"], g.backtrace()["entries"])
```

## Analysis features cheat sheet

- **step / run.frames / run.until** — run control with instruction/frame budget and timeouts.
- **break.add** (addr/bank/condition), **watch.add** (r/w/rw, ranges, value match, count-only mode).
- **trace.start/stop/dump** — ring of executed instructions with optional registers and up to 4 memory accesses per instruction.
- **coverage.get/reset** — which ROM bytes any executed instruction started at (per-bank, ranges).
- **backtrace** — Core-maintained call/ret stack (works through function calls and interrupts).
- **snapshot.save/load/delete/list** — named in-memory states or files; **rewind.set/pop** — rewind N frames.
- **mem.read/write/regions** — CPU view, bank-aware; raw hardware regions (vram/wram/oam/hram/cart_ram/rom/io...).
- **mem.capture + mem.diff** — "what changed between these two moments".
- **mem.search** — progressive value filtering (`"new != old"`, `"new < 5"`) over RAM/HRAM/cart RAM (SameBoy cheat engine).
- **input.set/press/tap/script** — frame-exact joypad control.
- **cpu.init / cpu.load / cpu.exec / cpu.reset** — bare-CPU sandbox for validating
  standalone ASM: no ROM or boot ROM needed, exact T-cycle accounting, PC bounds
  checking, HALT/interrupt detection.
- **screen.capture / video.tilemap / video.tiles / video.sprites / screen.palette** — visual and VRAM introspection.
- **symbols / eval / disasm** — rgbds `.sym` support; addresses everywhere accept symbol names.

## Layout

```
Makefile                 build entry point
cli/src/*.c              gbemu server (json, exec, mem, history, video, input, png)
cli/py/gbemu.py          Python client wrapper (stdlib only)
cli/tests/               make_test_rom.py (deterministic ROM generator), smoke.py
cli/bootroms/            vendored boot ROM binaries (see cli/bootroms/README.md)
tools/fetch_bootroms.sh  boot ROM provisioning
SameBoy/                 vendored SameBoy subset (see VENDORED.md)
```

## License

The gbemu CLI, Python client and tests are released under the Expat (MIT)
License — see [`LICENSE`](LICENSE).

The vendored SameBoy emulator core and its boot ROMs are copyright
(c) 2015-2026 Lior Halphon, also under the Expat License — see
[`SameBoy/LICENSE`](SameBoy/LICENSE) and [`VENDORED.md`](VENDORED.md) for
provenance and the scope of the vendored subset. Boot ROM provenance details
are in [`cli/bootroms/README.md`](cli/bootroms/README.md).
