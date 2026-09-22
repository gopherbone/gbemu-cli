# Vendored code: SameBoy

This repository vendors a subset of [SameBoy](https://sameboy.github.io/)
(<https://github.com/LIJI32/SameBoy>), an extremely accurate Game Boy/Game Boy
Color emulator, to build `gbemu` against its emulator core.

## Provenance

| | |
|---|---|
| Upstream project | SameBoy by Lior Halphon (LIJI32) |
| Vendored version | **1.0.3** (see `SameBoy/version.mk`) |
| Upstream source | <https://github.com/LIJI32/SameBoy> |
| License | Expat License — full text in [`SameBoy/LICENSE`](SameBoy/LICENSE) |

If you re-vendor a different version, update this file, `SameBoy/version.mk`
and the `VERSION` fallback in the root `Makefile`.

## What is kept

- `SameBoy/Core/` — the emulator core (the only part `gbemu` compiles against)
- `SameBoy/BootROMs/` — boot ROM assembler sources, used to rebuild
  `cli/bootroms/*.bin` (see `tools/fetch_bootroms.sh` and
  `cli/bootroms/README.md`)
- `SameBoy/Misc/`, upstream docs (`README.md`, `BESS.md`, `CHANGES.md`,
  `CONTRIBUTING.md`, `build-faq.md`), root `Makefile` (for the `bootroms`
  target) and `version.mk`

## What was removed, and why

Everything not needed by a headless build was deleted from the vendored tree:

`iOS/`, `Cocoa/`, `QuickLook/`, `AppleCommon/`, `JoyKit/`, `HexFiend/`,
`Windows/`, `SDL/`, `libretro/`, `Shaders/`, `FreeDesktop/`,
`XdgThumbnailer/`, `Tester/`, `sameboy.pc.in`, and build outputs
(`SameBoy/build/`).

Rationale:

- Dead weight for a headless CLI (`cli/src` only compiles against
  `SameBoy/Core/`).
- `SameBoy/LICENSE` places an **additional condition on the `iOS/`
  directory** (written permission from the author required for distribution
  through digital marketplaces). Dropping the directory keeps downstream
  redistribution of this project under the plain Expat terms only.
- `HexFiend/` is third-party code without a bundled license file in-tree.

The emulator core itself is **unmodified** upstream code. If you change
anything under `SameBoy/`, document the change here and note that
redistribution must still comply with `SameBoy/LICENSE`.
