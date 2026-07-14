# ChromaH264

The software H.264 decoder used by **Chroma** for Apple Vision Pro, published to satisfy the
LGPL v2.1 obligations of the FFmpeg libraries it links.

This repository contains everything needed to rebuild `ChromaH264.framework` from scratch,
with a modified FFmpeg if you wish, and to relink Chroma against your build.

## Why it exists

Apple silicon will not hardware-decode H.264 wider than **4096 pixels**. VR180 masters cross
that line routinely, because they carry both eyes side by side (4320×2160 is typical). Past
the line, AVFoundation silently falls back to its own software decoder, which manages roughly
15 frames per second of a 60fps source — a slideshow.

FFmpeg's H.264 decoder handles the same file at well over 200fps, because it frame-threads
across cores and uses NEON assembly. That difference is the entire reason this framework
exists. (Measured, on the actual hardware: 56fps for Apple's software decoder against a 60fps
requirement; the FFmpeg build here sustains a full 60fps with headroom to spare.)

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

## Why a dynamic framework with hidden symbols

Chroma links **two** copies of libavcodec: a demux-only build inside its video engine (which
deliberately registers no decoders), and this decode-only build. Both static archives export
`avcodec_find_decoder`. A static linker resolves that symbol once, globally — and it bound
every call to the demux-only copy, which has no H.264 decoder and returns `NULL`. The decoder
was present in the binary the whole time and nothing could reach it.

Link *order* cannot fix this and must not be attempted: two FFmpeg builds with different
configure flags have different struct layouts, so cross-bound calls are ABI roulette.

So the fix is isolation, not ordering. FFmpeg is compiled `-fvisibility=hidden` and linked
into a **dynamic** library whose export list contains only the seven `ch264_*` symbols
(`shim/exports.txt`). Every FFmpeg symbol resolves inside the dylib and is invisible outside
it, so there is nothing left to collide with.

That shape is also exactly what the LGPL asks for — dynamically linked and relinkable — which
is why it is the right answer rather than a workaround.

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

Produces `ChromaH264.xcframework`. The final lines verify the isolation that makes it work:

```
xros-device  exports: ch264_=7  av*=0 (must be 0)   contains ff_h264_decoder: 1 (must be >0)
```

Seven shim symbols exported, zero FFmpeg symbols exported, and the decoder actually present.
If those numbers differ, the framework will fail at runtime rather than at build time.

## API

Seven functions — see `shim/chroma_h264.h`. Create a decoder from the stream's `avcC` atom,
feed it AVCC (length-prefixed) samples, and receive NV12 `CVPixelBuffer`s from an internal
pool, with presentation timestamps carried through so B-frame reordering stays correct.
