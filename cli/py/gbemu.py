#!/usr/bin/env python3
"""gbemu.py — Python client for the gbemu JSON-lines Game Boy analysis server.

Stdlib only. Usage:

    from gbemu import GBEmu

    with GBEmu() as g:
        g.load_rom("game.gb", model="cgb", boot="cgb_boot.bin", seed=42)
        g.run_frames(120)
        g.break_add(addr="$0150")
        r = g.run_until(addr="$c100")   # or expr="..."
        img = g.screenshot_png()        # PNG bytes
        mem = g.mem_read(0xC000, 32)    # bytes
"""
import base64
import json
import os
import subprocess
import threading


class GBEmuError(Exception):
    pass


class GBEmu:
    def __init__(self, binary=None, bootrom_dir=None, timeout=120.0):
        if binary is None:
            binary = os.environ.get("GBEMU_BIN", self._default_binary())
        self.binary = binary
        args = [binary]
        if bootrom_dir or os.environ.get("GBEMU_BOOTROM_DIR"):
            args += ["--bootrom-dir", bootrom_dir or os.environ["GBEMU_BOOTROM_DIR"]]
        self.proc = subprocess.Popen(
            args, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL, text=True, bufsize=1,
        )
        self.timeout = timeout
        self._id = 0
        self._closed = False

    @staticmethod
    def _default_binary():
        here = os.path.dirname(os.path.abspath(__file__))
        return os.path.join(here, "..", "..", "build", "gbemu")

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def close(self):
        if self._closed:
            return
        self._closed = True
        try:
            self.proc.stdin.close()
            self.proc.wait(timeout=5)
        except Exception:
            self.proc.kill()

    def cmd(self, _cmd_name, timeout=None, **params):
        """Send a command; returns the result object or raises GBEmuError."""
        if self._closed or self.proc.poll() is not None:
            raise GBEmuError("gbemu process is not running")
        self._id += 1
        req = {"id": self._id, "cmd": _cmd_name, "params": params}
        line = json.dumps(req) + "\n"
        self.proc.stdin.write(line)
        self.proc.stdin.flush()
        deadline_holder = {"deadline": timeout or self.timeout, "resp": None, "err": None}

        def _read():
            try:
                deadline_holder["resp"] = self.proc.stdout.readline()
            except Exception as e:  # noqa: BLE001
                deadline_holder["err"] = e

        t = threading.Thread(target=_read, daemon=True)
        t.start()
        t.join(deadline_holder["deadline"])
        if t.is_alive():
            self.proc.kill()
            raise GBEmuError(f"timeout waiting for response to {_cmd_name}")
        resp_line = deadline_holder["resp"]
        if deadline_holder["err"]:
            raise GBEmuError(f"read error: {deadline_holder['err']}")
        if not resp_line:
            raise GBEmuError("gbemu exited")
        resp = json.loads(resp_line)
        if resp.get("id") != self._id:
            raise GBEmuError(f"protocol error: expected id {self._id}, got {resp.get('id')}")
        if not resp.get("ok"):
            raise GBEmuError(resp.get("error", "unknown error"))
        return resp["result"]

    # ---------------- convenience wrappers ----------------

    def ping(self):
        return self.cmd("ping")

    def info(self):
        return self.cmd("info")

    def load_rom(self, path, model="dmg", boot=None, symbols=None, sav=None, seed=None):
        p = {"path": path, "model": model}
        if boot is not None:
            p["boot"] = boot
        if symbols:
            p["symbols"] = symbols
        if sav:
            p["sav"] = sav
        if seed is not None:
            p["seed"] = seed
        return self.cmd("load_rom", **p)

    def regs(self):
        return self.cmd("regs.get")

    def set_regs(self, **regs):
        return self.cmd("regs.set", **regs)

    def eval(self, expr):
        return self.cmd("eval", expr=expr)

    def disasm(self, addr=None, n=16):
        p = {"n": n}
        if addr is not None:
            p["addr"] = addr
        return self.cmd("disasm", **p)["listing"]

    def mem_read(self, addr, length, bank=None, region=None, offset=None):
        p = {}
        if region:
            p["region"] = region
            p["offset"] = offset or 0
        else:
            p["addr"] = f"${addr:x}" if isinstance(addr, int) else addr
            if bank is not None:
                p["bank"] = bank
        p["len"] = length
        return bytes.fromhex(self.cmd("mem.read", **p)["data"])

    def mem_write(self, addr, data, region=None, offset=None):
        p = {"data": bytes(data).hex()}
        if region:
            p["region"] = region
            p["offset"] = offset or 0
        else:
            p["addr"] = f"${addr:x}" if isinstance(addr, int) else addr
        return self.cmd("mem.write", **p)

    def mem_capture(self, name):
        return self.cmd("mem.capture", name=name)

    def mem_diff(self, a, b="live", max_changes=1024):
        return self.cmd("mem.diff", a=a, b=b, max=max_changes)

    def mem_search(self, filter_expr, wide=False, max_results=512):
        return self.cmd("mem.search", filter=filter_expr, wide=wide, max=max_results)

    def mem_search_reset(self):
        return self.cmd("mem.search.reset")

    def rom_search(self, bytes_hex, mask=None, bank_lo=None, bank_hi=None, max=512):
        """Literal byte search over the whole cart ROM (file order).

        bytes_hex: hex string of the needle (whitespace ignored), e.g.
        "f28c8d" or "f2 8c 8d fd". mask: optional same-length hex string,
        1-bits compared, 0-bits wildcard (e.g. bytes="deadbeef",
        mask="ff00ffff" matches de??beef). Returns {"count": N,
        "results": [{"off": file offset hex, "bank": int, "addr": "$xxxx"}]}.
        Bank-straddling matches are skipped (not addressable contiguously).
        """
        p = {"bytes": bytes_hex, "max": max}
        if mask:
            p["mask"] = mask
        if bank_lo is not None:
            p["bank_lo"] = bank_lo
        if bank_hi is not None:
            p["bank_hi"] = bank_hi
        return self.cmd("rom.search", **p)

    def step(self, n=1):
        return self.cmd("step", n=n)

    def run_frames(self, n=1):
        return self.cmd("run.frames", n=n)

    def run_until(self, addr=None, expr=None, max_instructions=10_000_000, timeout_ms=120_000):
        p = {"max_instructions": max_instructions, "timeout_ms": timeout_ms}
        if addr is not None:
            p["addr"] = f"${addr:x}" if isinstance(addr, int) else addr
        if expr is not None:
            p["expr"] = expr
        return self.cmd("run.until", **p)

    def break_add(self, addr, bank=None, cond=None, type=None, end=None,
                  access=None, value=None, mask=None, stop=None,
                  pc=None, pc_not=None, pc_lo=None, pc_hi=None):
        """Add a breakpoint.

        Legacy PC form: break_add(addr[, bank][, cond]).
        Mem-predicate form (AND-composed): type="mem" + access ("r"/"w"/"rw"),
        optional end (inclusive range), value+mask (emit when (data & mask) ==
        value), bank (effective bank of the access), pc/pc_not/pc_lo+pc_hi
        (id/ exclusion / window on the accessing pc), stop=False to count
        without halting.
        """
        p = {"addr": f"${addr:x}" if isinstance(addr, int) else addr}
        if bank is not None:
            p["bank"] = bank
        if cond:
            p["cond"] = cond
        hx = lambda v: f"${v:x}" if isinstance(v, int) else v  # noqa: E731
        if type is not None: p["type"] = type
        if end is not None: p["end"] = hx(end)
        if access is not None: p["access"] = access
        if value is not None: p["value"] = value
        if mask is not None: p["mask"] = mask
        if stop is not None: p["stop"] = stop
        if pc is not None: p["pc"] = hx(pc)
        if pc_not is not None: p["pc_not"] = hx(pc_not)
        if pc_lo is not None: p["pc_lo"] = hx(pc_lo)
        if pc_hi is not None: p["pc_hi"] = hx(pc_hi)
        return self.cmd("break.add", **p)["id"]

    def break_list(self):
        return self.cmd("break.list")["breakpoints"]

    def break_del(self, bid):
        return self.cmd("break.del", id=bid)

    def break_clear(self):
        return self.cmd("break.clear")

    def watch_add(self, addr, end=None, access="w", value=None, bank=None, stop=True, cond=None):
        p = {"addr": f"${addr:x}" if isinstance(addr, int) else addr, "access": access, "stop": stop}
        if end is not None:
            p["end"] = f"${end:x}" if isinstance(end, int) else end
        if value is not None:
            p["value"] = value
        if bank is not None:
            p["bank"] = bank
        if cond:
            p["cond"] = cond
        return self.cmd("watch.add", **p)["id"]

    def watch_list(self):
        return self.cmd("watch.list")["watchpoints"]

    def watch_del(self, wid):
        return self.cmd("watch.del", id=wid)

    def watch_clear(self):
        return self.cmd("watch.clear")

    def trace_start(self, max_entries=65536, with_regs=False, with_mem=False,
                    rom_only=False, pc_lo=None, pc_hi=None):
        p = {"max": max_entries, "with_regs": with_regs, "with_mem": with_mem,
             "rom_only": rom_only}
        if pc_lo is not None and pc_hi is not None:
            p["pc_lo"] = pc_lo
            p["pc_hi"] = pc_hi
        return self.cmd("trace.start", **p)

    def trace_stop(self):
        return self.cmd("trace.stop")

    def trace_dump(self, limit=1000):
        return self.cmd("trace.dump", limit=limit)

    def coverage(self, arm=False, ranges=False, max_ranges=4096):
        return self.cmd("coverage.get", arm=arm, ranges=ranges, max_ranges=max_ranges)

    def coverage_reset(self):
        return self.cmd("coverage.reset")

    def backtrace(self):
        return self.cmd("backtrace")

    def snapshot_save(self, name=None, path=None):
        p = {}
        if name:
            p["name"] = name
        if path:
            p["path"] = path
        return self.cmd("snapshot.save", **p)

    def snapshot_load(self, name=None, path=None):
        p = {}
        if name:
            p["name"] = name
        if path:
            p["path"] = path
        return self.cmd("snapshot.load", **p)

    def snapshot_list(self):
        return self.cmd("snapshot.list")

    def snapshot_delete(self, name):
        return self.cmd("snapshot.delete", name=name)

    def rewind_set(self, seconds):
        return self.cmd("rewind.set", seconds=seconds)

    def rewind_pop(self, frames=1):
        return self.cmd("rewind.pop", frames=frames)

    def input_set(self, keys):
        return self.cmd("input.set", keys=list(keys))

    def input_press(self, keys, frames=4, release=True):
        return self.cmd("input.press", keys=list(keys), frames=frames, release=release)

    def input_tap(self, key, press_frames=2, release_frames=8):
        return self.cmd("input.tap", keys=[key], press_frames=press_frames,
                        release_frames=release_frames)

    def input_script(self, steps, run=True):
        """steps: [([keys], frames), ...]"""
        return self.cmd("input.script",
                        steps=[{"keys": list(k), "frames": f} for k, f in steps], run=run)

    def screenshot(self, scale=1):
        return base64.b64decode(self.cmd("screen.capture", scale=scale)["png_b64"])

    def screenshot_png(self, path=None, scale=1):
        p = {"scale": scale}
        if path:
            p["path"] = path
            return self.cmd("screen.capture", **p)
        return base64.b64decode(self.cmd("screen.capture", **p)["png_b64"])

    def palette(self):
        return self.cmd("screen.palette")

    def tilemap(self, layer="bg"):
        return self.cmd("video.tilemap", layer=layer)

    def tiles(self, start=0, count=32, bank=0):
        return self.cmd("video.tiles", start=start, count=count, bank=bank)

    def sprites(self):
        return self.cmd("video.sprites")

    def symbols_load(self, path):
        return self.cmd("symbols.load", path=path)

    def symbols_resolve(self, q, bank=None, exact=False):
        p = {"q": q, "exact": exact}
        if bank is not None:
            p["bank"] = bank
        return self.cmd("symbols.resolve", **p)

    # ---------------- sandbox (bare-CPU validation) ----------------

    def cpu_init(self, model="dmg", seed=9001, fill="00", vectors_ret=True, lcd=True):
        """Create a boot-ROM-less machine on a flat 32KB ROM buffer."""
        return self.cmd("cpu.init", model=model, seed=seed, fill=fill,
                        vectors_ret=vectors_ret, lcd=lcd)

    def cpu_load(self, addr, data):
        a = f"${addr:x}" if isinstance(addr, int) else addr
        return self.cmd("cpu.load", addr=a, data=bytes(data).hex())

    def cpu_reset(self, clear_rom=False):
        return self.cmd("cpu.reset", clear_rom=clear_rom)

    def cpu_exec(self, steps=256, at=None, data=None, bound="auto", until_addr=None,
                 detect_halt=True, max_cycles8=None, ime=None, ie=None, **regs):
        """Run code. If data given, writes it at `at` and sets pc=at.
        bound: [lo, hi] list, False/None to disable, or "auto" (default: bound to
        the written region when data given). regs: pc/sp/af/bc/de/hl/a..l values.
        Returns {steps, cycles8, cycles_t, reason, pc, regs:{...}}."""
        p = {"steps": steps, "detect_halt": detect_halt}
        if at is not None:
            p["at"] = f"${at:x}" if isinstance(at, int) else at
        if data is not None:
            p["data"] = bytes(data).hex()
        if max_cycles8 is not None:
            p["max_cycles8"] = max_cycles8
        if until_addr is not None:
            p["until_addr"] = f"${until_addr:x}" if isinstance(until_addr, int) else until_addr
        if bound != "auto":
            if bound:
                lo, hi = bound
                p["bound"] = [f"${lo:x}" if isinstance(lo, int) else lo,
                              f"${hi:x}" if isinstance(hi, int) else hi]
            else:
                p["bound"] = []
        if ime is not None:
            p["ime"] = ime
        if ie is not None:
            p["ie"] = ie
        p.update(regs)
        return self.cmd("cpu.exec", **p)

    def quit(self):
        try:
            return self.cmd("quit")
        finally:
            self.close()
