# gbemu JSON-lines protocol

`gbemu` is a headless SameBoy Game Boy emulator driven over stdin/stdout. One JSON
request per input line; exactly one JSON response per output line. No other output is
ever written to stdout (emulator logs go to stderr).

## Envelope

Request:
```json
{"id": 1, "cmd": "mem.read", "params": {"addr": "$c000", "len": 16}}
```
Response (success):
```json
{"id": 1, "ok": true, "result": {"addr": "c000", "bank": -1, "len": 16, "data": "3e00..."}}
```
Response (error):
```json
{"id": 1, "ok": false, "error": "bad 'addr'"}
```

- `id` — integer or string, echoed. Must be present for correlation.
- `params` — object, optional.
- Responses are single-line, in-request order. Commands are processed serially.

## Conventions

- **Addresses**: anywhere an address is taken, it may be a JSON number, or a string
  `"$c000"` / `"0xc000"` / `"49152"` / a **debugger expression or symbol name**
  (evaluated via SameBoy's evaluator, e.g. `"hl + 4"` or `"VBlank"` when symbols loaded).
- **Hex strings** are lowercase without `0x`.
- **Banks**: `-1` means "use the currently mapped bank". Banked reads support ROM
  (addr < $8000), VRAM ($8000-$9FFF), cart RAM ($A000-$BFFF), WRAM ($C000-$DFFF).
- **Bytes** are encoded as hex strings; screenshots as base64 PNG.
- Breakpoints fire **before** the instruction at the address executes and are
  re-triggerable (a resumed run does not immediately re-fire the same breakpoint).
  Watchpoints and **mem-predicate breakpoints** fire mid-instruction, at the
  memory access itself (a resumed run re-executes the stopped instruction —
  plan predicates to tolerate one re-fire of the same access).
- Steps/frames stop early on any breakpoint/watchpoint/until condition; responses
  include `"stopped": true, "reason": "..."`. On any breakpoint/watchpoint stop
  the response additionally carries: `detail` (human-readable hit reason: which
  predicate fired, addr, value, effective bank, accessing pc) and `banks`
  (`{rom0, rom, vram, wram, sram}` live at the stop) — the hit-reason record
  every bank-qualified leg should archive.
- Mem-predicate pc gating (`pc`/`pc_not`/`pc_lo`/`pc_hi`) uses the pc of the
  instruction PERFORMING the access (latched at instruction entry; the pc
  register alone is already past the instruction by access time).

## Commands

### Session
| cmd | params | result |
|---|---|---|
| `ping` | — | `{pong, version, sameboy}` |
| `info` | — | session info: title, crc32, sizes, model, counters |
| `load_rom` | `path` (req), `model` (`dmg`\|`mgb`\|`sgb`\|`sgb2`\|`cgb`\|`agb`, default `dmg`), `boot` (boot ROM name in bootrom dir, absolute path, or `"builtin"` = embedded minimal DMG boot), `symbols` (.sym), `sav` (battery file), `seed` (uint) | session info incl. `bootrom`, `seed` |
| `quit` | — | `{bye}` and process exits |

All analysis state (breakpoints, watchpoints, trace, coverage, snapshots, captures) is
reset by `load_rom`.

### CPU / expressions
| cmd | params | result |
|---|---|---|
| `regs.get` | — | pc/sp/af../hl hex + bytes, ime/halted/stopped/double_speed, current banks, ie/if, pc_symbol |
| `regs.set` | any of `pc,sp,af,bc,de,hl,a..l` (address-syntax values) | updated regs (as regs.get) |
| `eval` | `expr` | `{value:"hex16", bank}` via SameBoy expression evaluator |
| `disasm` | `addr` (default pc), `n` (default 16, max 512) | `{addr, n, listing}` symbol-annotated listing (current live memory) |

### Execution control
| cmd | params | result |
|---|---|---|
| `step` | `n` (default 1) | `{ran_instructions, pc, sp, a, instructions_total, stopped, ...}` |
| `run.frames` | `n` (default 1) | as step + `frames_total` |
| `run.until` | `addr` xor `expr`, `max_instructions` (default 10M), `timeout_ms` (default 120s) | `{ran_instructions, stopped, reason}` |

### Breakpoints
| cmd | params | result |
|---|---|---|
| `break.add` | PC form: `addr`, `bank?` (effective bank at PC), `cond?` (debugger expression, evaluated on hit). Mem-predicate form: `type:"mem"`, `access` (`"r"`/`"w"`/`"rw"`, default `"w"`), `end?` (range end, inclusive), `mask?`+`value?` (fire when `(data & mask) == value`; `value` alone = full byte), `bank?` (effective bank OF THE ACCESS), `pc` / `pc_not` (accessing-pc equality/exclusion), `pc_lo`+`pc_hi` (accessing-pc window), `stop` (default true; false = count only), `cond?` — all clauses AND-composed | `{id, addr, kind}` |
| `break.list` | — | `{breakpoints:[{id,kind,enabled,addr,bank,hits,cond,end?,read?,write?,stop?,mask?,value?,pc?,pc_not?,pc_lo?,pc_hi?}]}` |
| `break.del` | `id` | `{deleted}` |
| `break.clear` | — | `{cleared}` |

### Watchpoints (memory access)
| cmd | params | result |
|---|---|---|
| `watch.add` | `addr`, `end?` (inclusive range), `access` (`"r"`,`"w"`,`"rw"`, default `"w"`), `value?` (byte, write-only match), `bank?`, `stop` (default true; false = count only), `cond?` | `{id, addr, end}` |
| `watch.list` / `watch.del` / `watch.clear` | as break.* | — |

### Trace (execution + memory access ring buffer)
| cmd | params | result |
|---|---|---|
| `trace.start` | `max` (ring capacity, default 65536, max 4M), `with_regs`, `with_mem` (up to 4 accesses/instr), `rom_only`, `pc_lo`,`pc_hi` | `{tracing, capacity}` |
| `trace.stop` | — | `{tracing:false, total, dropped}` |
| `trace.dump` | `limit` (default 1000, max 100000) | `{total, dropped, entries:[{pc,bank,op,regs?,mem?,symbol?}]}` |

`mem` entries: `["r"|"w", "addr_hex", "value_hex"]`. Note `bank` for ROM fetches;
`symbol` resolves via loaded .sym.

### Coverage (executed-instruction start bytes, ROM)
| cmd | params | result |
|---|---|---|
| `coverage.get` | `arm` (activate), `ranges` (include per-bank ranges), `max_ranges` | `{armed, instructions, rom_bytes, banks, per_bank:[{bank,executed,pct,ranges?}]}` |
| `coverage.reset` | — | clears bits, keeps armed |

### Backtrace / call stack
| cmd | params | result |
|---|---|---|
| `backtrace` | — | `{call_depth, stack_sp, entries:[{addr,bank,sp,symbol?}]}` — most recent first; maintained by SameBoy's Core for calls/rets/interrupts |

### Snapshots / rewind
| cmd | params | result |
|---|---|---|
| `snapshot.save` | `name` (in-memory slot) xor `path` (file) | `{slot,size,name,pc,frame}` |
| `snapshot.load` | `name` xor `path` | restores full emulator state |
| `snapshot.list` | — | also lists memory captures |
| `snapshot.delete` | `name` | — |
| `rewind.set` | `seconds` (0 = off, max 300) | enables per-vblank state ring |
| `rewind.pop` | `frames` (default 1) | `{rewound}` steps back in time |

### Memory
| cmd | params | result |
|---|---|---|
| `mem.read` | `addr`,`len` (≤65536),`bank?` — or `region`,`offset`,`len` (raw hardware region) | `{addr,bank,len,data}` — CPU-view reads are side-effect-free |
| `mem.write` | `addr`,`data` (hex) — or `region`,`offset`,`data` (raw; ROM/bootrom rejected) | `{written}` |
| `mem.regions` | — | available regions + sizes + banks |
| `mem.capture` | `name` | snapshot of VRAM/WRAM/OAM/HRAM/cart-RAM |
| `mem.diff` | `a` (capture name), `b` (name or `"live"`), `max` | `{changes:[{region,off,addr,bank,a,b}],truncated}` |
| `mem.search` | `filter` (expression over `new`,`old`, e.g. `"new != old"`, `"new == 5 && new == old + 1"`), `wide` (16-bit LE), `max` | `{count, results:[{addr,bank,value}]}` — SameBoy cheat-search semantics: chained filters narrow progressively; first filter compares against zero baseline |
| `mem.search.reset` | — | reset search state |
| `rom.search` | `bytes` (hex needle, whitespace ignored), `mask` (optional same-length hex; 1-bits compared, 0-bits wildcard), `bank_lo`/`bank_hi` (inclusive file-bank filter), `max` (default 512, cap 8192) | `{count, results:[{off,bank,addr}]}` — literal search over the whole cart ROM in file order. `off` = file offset (hex), `addr` = banked map address (`bank==0: off; else 0x4000+(off%0x4000)`). Matches straddling a bank boundary are skipped (not contiguously addressable). `count` = total matches; `results` capped at `max`. |

Raw regions: `rom`, `vram`, `wram`, `cart_ram`, `oam`, `hram`, `bootrom`, `bgp`,
`obp`, `io`.

### Symbols
| cmd | params | result |
|---|---|---|
| `symbols.load` | `path` (rgbds/BGB `.sym`: `BB:AAAA name` lines) | — |
| `symbols.clear` | — | — |
| `symbols.resolve` | `q` (address → desc, or name → addr), `bank?`, `exact?`, `prefer_local?` | `{addr,desc}` or `{addr,bank}` |

### Sandbox (bare-CPU ASM validation)

For CI pipelines validating standalone SM83 assembly: runs code directly, no ROM file,
no boot ROM, post-boot machine state, deterministic.

| cmd | params | result |
|---|---|---|
| `cpu.init` | `model` (default `dmg`), `seed` (default 9001), `fill` (flat-ROM fill byte, default `"00"`), `vectors_ret` (default true: RST/int vectors 0x40-0x60 = `RET`), `lcd` (default true: LCDC=$91, BGP=$E4 post-boot) | `{sandbox:true,...}` |
| `cpu.load` | `addr`, `data` (hex bytes) | writes into the flat ROM (addr < $8000) or other memory |
| `cpu.reset` | `clear_rom` (default false) | zeroes RAM/VRAM/OAM/HRAM, restores post-boot regs, pc=0 |
| `cpu.exec` | `steps` (default 256), `max_cycles8` (default steps×64), `at` + `data` (inline load+set pc), any regs `pc,sp,af,bc,de,hl,a..l`, `ime`, `ie`, `bound` (`[lo,hi]`; default: auto-bounds the written region when `data` given; `[]` disables), `until_addr`, `detect_halt` (default true) | `{steps, cycles8, cycles_t, reason, pc, sp, halted, regs}` |

`reason`: `steps_completed`, `until_addr`, `left_bounds`, `cycle_limit`, `halted`, or a
breakpoint/watchpoint reason. `cycles8` = raw 8-MHz-domain cycles for this exec;
`cycles_t` = `cycles8/2` = T-cycles. `cpu.exec` always resumes from a runnable state
(clears HALT first).

All other commands (step, trace, breakpoints, mem.*, snapshots…) work in sandbox mode.

Example — validate a 16-bit add routine:
```json
{"id":1,"cmd":"cpu.init"}
{"id":2,"cmd":"cpu.exec","params":{"at":"$c000","data":"3a85","hl":"$d000","steps":2}}
```

### Input
| `input.set` | `keys`: array or "a,b" string | holds mask persistently |
| `input.press` | `keys`, `frames` (default 4), `release` (default true) | runs N frames with keys held |
| `input.tap` | `keys` (array of 1), `press_frames`=2, `release_frames`=8 | quick press-release |
| `input.script` | `steps` (array of `{keys, frames}`), `run` (default true) | frame-accurate input sequence; runs to completion |

Valid keys: `a b start select up down left right`.

### Video
| cmd | params | result |
|---|---|---|
| `screen.capture` | `path` (write file) or none → `png_b64`; `scale` 1-4 | 160×144 RGBA PNG (SGB border not included in v1) |
| `screen.palette` | — | DMG: bgp/obp0/obp1 mapped colors; CGB: 8+8 RGB palettes |
| `video.tiles` | `start`,`count` (≤96), `bank` | per-tile 64-char strings of color indices 0-3 |
| `video.tilemap` | `layer` (`bg`\|`win`) | 32×32 tile-index grid (+CGB attrs), map base, tile data mode, wx/wy |
| `video.sprites` | — | up to 40 OAM entries: x,y,tile, flips, palettes, priority |

## Determinism

`load_rom` with a fixed `seed` and fixed command stream gives bit-identical results
(verified by smoke tests). Caveats: RTC games read wall-clock time; `builtin` and real
boot ROMs leave different (but each deterministic) residual states.

## Errors

Common codes (in `error` string prefix): `no_rom` (command needs a loaded ROM),
`bad '<param>'`, `unknown_cmd`, `parse_error`.
