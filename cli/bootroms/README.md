# Boot ROMs

The `*_boot.bin` files in this directory are the boot ROM binaries produced
from the assembler sources in `SameBoy/BootROMs/` of the vendored
[SameBoy](https://github.com/LIJI32/SameBoy) distribution (version 1.0.3).

These are SameBoy's own clean-room reimplementations of the Game Boy boot
ROMs — they are **not** Nintendo's copyrighted ROMs. They carry the same
Expat License as the rest of SameBoy: copyright (c) 2015-2026 Lior Halphon,
full text in [`../../SameBoy/LICENSE`](../../SameBoy/LICENSE). The
`SameBoyLogo.png` artwork embedded in some boot ROMs is likewise part of the
SameBoy repository and covered by that license.

`SameBoy/BootROMs/hardware.inc` originates from
<https://github.com/gbdev/hardware.inc> and is released under CC0
(<https://creativecommons.org/publicdomain/zero/1.0/>).

## Regenerating

```sh
make bootroms
```

This prefers building from source with RGBDS (`make -C SameBoy bootroms`) and
falls back to extracting the binaries from the official SameBoy release
assets for the vendored version (`tools/fetch_bootroms.sh`). Alternatively,
ROMs can always be run without boot ROM files using `boot:"builtin"`, which
uses the minimal boot ROM embedded in `gbemu` (suitable for tests/CI, but not
cycle-accurate).
