# ChromaH264

The software H.264 decoder framework used by **Chroma**, published to satisfy the LGPL v2.1
obligations of the FFmpeg libraries it links.

This repository contains everything needed to rebuild `ChromaH264.framework` from scratch,
with a modified FFmpeg if you wish, and to relink Chroma against your build.

## What's in the framework

- **FFmpeg n7.1** (commit `b08d796`), **unmodified**, built from the upstream release.
- Only `libavcodec` and `libavutil`, configured with `--disable-everything` plus
  `--enable-decoder=h264` and `--enable-parser=h264`. Nothing else is compiled in.
- **No `--enable-gpl`, no `--enable-nonfree`** — so no GPL-only or non-free components are
  present. This is stock LGPL FFmpeg.
- A thin C wrapper (`shim/chroma_h264.c`) exposing seven functions.

## Licensing

FFmpeg here is licensed under the **GNU Lesser General Public License, version 2.1 or later**
(see `COPYING.LGPLv2.1`). Chroma links the resulting framework **dynamically**, so it can be
replaced with your own build — build it, drop it into `Chroma.app/Frameworks/`, re-sign, and
the app will use yours.

The shim (`shim/`) and the build script are © Drew Baumann and are offered under the same
terms, so that the combined work remains replaceable.

**Note on patents.** The LGPL covers the *software*. H.264/AVC is separately encumbered by
patents administered by [Via LA](https://www.via-la.com/licensing-2/avc-h-264/) (formerly
MPEG-LA), and that is independent of which implementation you use. If you ship an H.264
decoder in a product, that obligation is yours to address.

## Build notes

FFmpeg is compiled `-fvisibility=hidden` and linked into a **dynamic** library whose export
list contains only the seven `ch264_*` symbols (`shim/exports.txt`). Every FFmpeg symbol
resolves inside the dylib and is invisible outside it. Keep that shape if you modify the
build — it is also exactly what the LGPL asks for: dynamically linked and relinkable.

Two things that will bite you if you change the build:

- **`-force_load` is required.** Nothing references `ff_h264_decoder` by name, so the linker
  will happily dead-strip the decoder registration table and hand you a framework that links,
  loads, and returns `NULL` from `avcodec_find_decoder`.
- **The public shim functions need explicit `__attribute__((visibility("default")))`.**
  `-fvisibility=hidden` hides *everything*, including them, and an export list cannot
  re-export a symbol the compiler already marked `private_extern`.

The build script self-checks both conditions and prints the result.

## Building

Requires Xcode and a checkout of the FFmpeg source at the release named above.

```sh
export FFMPEG_SRC=/path/to/ffmpeg-n7.1
SLICES="xros-device xros-sim" ./build-h264-decoder.sh
```

Produces `ChromaH264.xcframework`. The final lines verify the export isolation:

```
xros-device  exports: ch264_=7  av*=0 (must be 0)   contains ff_h264_decoder: 1 (must be >0)
```

Seven shim symbols exported, zero FFmpeg symbols exported, and the decoder actually present.
If those numbers differ, the framework will fail at runtime rather than at build time.

## API

Seven functions — see `shim/chroma_h264.h`. Create a decoder from the stream's `avcC` atom,
feed it AVCC (length-prefixed) samples, and receive NV12 `CVPixelBuffer`s from an internal
pool, with presentation timestamps carried through so B-frame reordering stays correct.
