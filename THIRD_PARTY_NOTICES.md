# Third-party notices

This project builds on the following third-party works. Their source code is fetched at build time
by `bootstrap.sh` (it is not stored in this repository) unless noted otherwise.

## Azahar

- Upstream: https://github.com/azahar-emu/azahar (revision `6b4e7f2e694514fdf2f72dc20368609b7cd4912e`)
- Copyright: Citra Emulator Project / Azahar Emulator Project
- License: GPL-2.0-or-later and GPL-3.0-or-later (per file)
- `patches/azahar.patch` is a modification of this work and is distributed under the same terms.

Azahar bundles further libraries as git submodules (Boost, fmt, SoundTouch, teakra, nihstro, zstd,
libyuv, FAAD2, xxHash, Crypto++, microtar, inih, lodepng, dds-ktx, ENet, libretro-common and others).
Each keeps its own license in its directory under `externals/` of the Azahar source tree.
Azahar also bundles an open-source replacement for the 3DS shared system font
(`externals/open_source_archives`), whose license is included there.

## LibreSSL

- Upstream: https://www.libressl.org/ (fetched as an Azahar submodule)
- License: OpenSSL / SSLeay / ISC licenses (see `COPYING` in the LibreSSL source)
- `patches/libressl.patch` is a modification of this work.

## robin-map

- Upstream: https://github.com/Tessil/robin-map
- Copyright (c) 2017 Thibaut Goetghebuer-Planchon
- License: MIT — [licenses/MIT-robin-map.txt](licenses/MIT-robin-map.txt)

## Emscripten

- Upstream: https://github.com/emscripten-core/emscripten (version 7.0.0)
- License: MIT / University of Illinois/NCSA. Parts of its runtime are linked into the build output.

## Fonts (glyph bitmaps in `patches/azahar.patch`)

`src/core/hle/service/apt/bcfnt/shared_font_patch.inc` (added by the patch) contains 4-bit glyph
bitmaps for 20 characters, rendered from the following fonts by the scripts in `tools/font/`.
The font files themselves are not included.

- **M PLUS 1p** — Copyright 2016 The M+ Project Authors.
  SIL Open Font License 1.1 — [licenses/OFL-MPLUS1p.txt](licenses/OFL-MPLUS1p.txt)
- **Noto Sans JP** (Noto Sans CJK) — Copyright 2014-2021 Adobe (http://www.adobe.com/), with
  Reserved Font Name 'Source'. SIL Open Font License 1.1 —
  [licenses/OFL-NotoSansJP.txt](licenses/OFL-NotoSansJP.txt)

## Homebrew used for testing

The development tooling can load the Homebrew Launcher (devkitPro `3ds-hbmenu`) and
Checkpoint for testing. Neither is included in this repository.

## Not included

No Nintendo ROMs, firmware, system archives, BIOS data or encryption keys are included or
downloaded by this project.
