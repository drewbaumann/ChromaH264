#!/bin/bash
# Build ChromaH264.xcframework — a self-contained software H.264 decoder framework.
#
# SHAPE OF THE BUILD (keep this if you modify it)
# -----------------------------------------------
# FFmpeg is compiled -fvisibility=hidden and linked into a DYNAMIC library whose
# -exported_symbols_list contains only _ch264_*. Every FFmpeg symbol (including
# avcodec_find_decoder) resolves INSIDE the dylib and is not exported, so the framework
# cannot collide with any other FFmpeg build an app may link. Callers only ever see the
# C shim.
#
# That is also the shape LGPL wants (this is stock LGPL FFmpeg — no --enable-gpl, no
# --enable-nonfree): dynamically linked and relinkable, with this repo as the
# corresponding source.
set -euo pipefail

SRC="${FFMPEG_SRC:-$HOME/ffmpeg-n7.1}"
OUT="$(cd "$(dirname "$0")" && pwd)"
BUILD="$OUT/.h264-build"
SHIM="$OUT/shim"
SLICES="${SLICES:-xros-device xros-sim ios-device ios-sim}"
IOS_MIN="17.0"
VISIONOS_MIN="2.0"

[ -d "$SRC" ] || { echo "FFmpeg source not found at $SRC (set FFMPEG_SRC)"; exit 1; }

DECODER_FLAGS=(
  --disable-everything
  --disable-programs --disable-doc --disable-debug
  --disable-avdevice --disable-avfilter
  --disable-avformat            # AVAssetReader demuxes; we only decode
  --disable-swscale --disable-swresample --disable-postproc
  --disable-network --disable-bzlib --disable-lzma --disable-iconv --disable-zlib

  # The whole point.
  --enable-decoder=h264
  --enable-parser=h264

  # Threading: FFmpeg's h264 decoder frame-threads across cores — most of a software
  # decoder's throughput comes from this.
  --enable-pthreads

  # Keep the assembly: NEON is where H.264's per-macroblock work actually happens, so
  # --disable-asm would be fatal for a decoder. (Also NOT --enable-small: that trades
  # decode speed for binary size, the wrong side of the trade here.)

  # Required: libavcodec/videotoolbox.c references kCVPixelBufferOpenGLESCompatibilityKey,
  # which visionOS's SDK lacks. FFmpeg n7.1 predates visionOS and mis-branches it as
  # iOS/tvOS, so the file fails to compile for xros.
  --disable-videotoolbox --disable-audiotoolbox

  --enable-static --disable-shared
  --enable-pic
)

archs_for_slice() {
  case "$1" in
    ios-sim) echo "arm64 x86_64" ;;
    *) echo "arm64" ;;
  esac
}

sdk_for_slice() {
  case "$1" in
    ios-device)  echo "iphoneos" ;;
    ios-sim)     echo "iphonesimulator" ;;
    xros-device) echo "xros" ;;
    xros-sim)    echo "xrsimulator" ;;
  esac
}

triple_for() {
  local slice="$1" arch="$2"
  case "$slice" in
    ios-device)  echo "${arch}-apple-ios${IOS_MIN}" ;;
    ios-sim)     echo "${arch}-apple-ios${IOS_MIN}-simulator" ;;
    xros-device) echo "${arch}-apple-xros${VISIONOS_MIN}" ;;
    xros-sim)    echo "${arch}-apple-xros${VISIONOS_MIN}-simulator" ;;
  esac
}

platform_for_slice() {
  case "$1" in
    ios-device)  echo "iPhoneOS" ;;
    ios-sim)     echo "iPhoneSimulator" ;;
    xros-device) echo "XROS" ;;
    xros-sim)    echo "XRSimulator" ;;
  esac
}

# Build FFmpeg (static, hidden) for one slice+arch.
build_ffmpeg() {
  local slice="$1" arch="$2" prefix="$3"
  local sdk sysroot cc cflags host_sysroot triple
  sdk="$(sdk_for_slice "$slice")"
  triple="$(triple_for "$slice" "$arch")"
  sysroot="$(xcrun --sdk "$sdk" --show-sdk-path)"
  cc="$(xcrun --sdk "$sdk" --find clang)"
  # -fvisibility=hidden: belt to the export-list's braces. Every FFmpeg symbol becomes
  # private to the dylib, so even a mistake in the export list cannot leak avcodec_*.
  cflags="-arch $arch -target $triple -isysroot $sysroot -O3 -fvisibility=hidden"
  host_sysroot="$(xcrun --sdk macosx --show-sdk-path)"

  echo "==> ffmpeg $slice/$arch ($triple)"
  ( cd "$SRC" && make distclean >/dev/null 2>&1 || true )
  ( cd "$SRC" && ./configure \
      "${DECODER_FLAGS[@]}" \
      --enable-cross-compile --target-os=darwin --arch="$arch" \
      --prefix="$prefix" \
      --cc="$cc" \
      --extra-cflags="$cflags" \
      --extra-ldflags="$cflags" \
      --host-cflags="-isysroot $host_sysroot" \
      --host-ldflags="-isysroot $host_sysroot" \
      >/dev/null )
  ( cd "$SRC" && make -j"$(sysctl -n hw.ncpu)" >/dev/null && make install >/dev/null )
}

# Compile the shim and link the DYLIB for one slice+arch. This is where isolation happens.
link_dylib() {
  local slice="$1" arch="$2" prefix="$3" out_dylib="$4"
  local sdk sysroot cc triple
  sdk="$(sdk_for_slice "$slice")"
  triple="$(triple_for "$slice" "$arch")"
  sysroot="$(xcrun --sdk "$sdk" --show-sdk-path)"
  cc="$(xcrun --sdk "$sdk" --find clang)"

  "$cc" -c "$SHIM/chroma_h264.c" -o "$prefix/chroma_h264.o" \
    -arch "$arch" -target "$triple" -isysroot "$sysroot" \
    -O3 -fvisibility=hidden -fPIC \
    -I"$prefix/include" -I"$SHIM"

  # -force_load: FFmpeg registers its decoder through a static table the linker would
  # otherwise dead-strip, since nothing in the shim references ff_h264_decoder by name.
  # Dropping it gives you a dylib that links, loads, and returns NULL from
  # avcodec_find_decoder — the exact failure this rebuild exists to fix, wearing a new hat.
  "$cc" -dynamiclib -o "$out_dylib" \
    "$prefix/chroma_h264.o" \
    -Wl,-force_load,"$prefix/lib/libavcodec.a" \
    -Wl,-force_load,"$prefix/lib/libavutil.a" \
    -Wl,-exported_symbols_list,"$SHIM/exports.txt" \
    -install_name "@rpath/ChromaH264.framework/ChromaH264" \
    -arch "$arch" -target "$triple" -isysroot "$sysroot" \
    -framework CoreVideo -framework CoreFoundation -lpthread
}

# Assemble a .framework bundle (flat layout — iOS/visionOS, not macOS).
make_framework() {
  local slice="$1" binary="$2" fw="$3"
  rm -rf "$fw"
  mkdir -p "$fw/Headers" "$fw/Modules"
  cp "$binary" "$fw/ChromaH264"
  cp "$SHIM/chroma_h264.h" "$fw/Headers/"

  cat > "$fw/Modules/module.modulemap" <<'MMAP'
framework module ChromaH264 {
    umbrella header "chroma_h264.h"
    export *
}
MMAP

  local min platform
  platform="$(platform_for_slice "$slice")"
  case "$slice" in
    ios-*) min="$IOS_MIN" ;;
    *)     min="$VISIONOS_MIN" ;;
  esac

  cat > "$fw/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleDevelopmentRegion</key><string>en</string>
  <key>CFBundleExecutable</key><string>ChromaH264</string>
  <key>CFBundleIdentifier</key><string>drewbaumann.ChromaH264</string>
  <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
  <key>CFBundleName</key><string>ChromaH264</string>
  <key>CFBundlePackageType</key><string>FMWK</string>
  <key>CFBundleShortVersionString</key><string>1.0</string>
  <key>CFBundleVersion</key><string>1</string>
  <key>MinimumOSVersion</key><string>$min</string>
  <key>CFBundleSupportedPlatforms</key><array><string>$platform</string></array>
</dict>
</plist>
PLIST
}

rm -rf "$BUILD"; mkdir -p "$BUILD"
FRAMEWORK_ARGS=()

for slice in $SLICES; do
  slice_root="$BUILD/$slice"; mkdir -p "$slice_root"
  dylibs=()
  for arch in $(archs_for_slice "$slice"); do
    prefix="$slice_root/$arch"
    build_ffmpeg "$slice" "$arch" "$prefix"
    link_dylib "$slice" "$arch" "$prefix" "$slice_root/ChromaH264-$arch.dylib"
    dylibs+=("$slice_root/ChromaH264-$arch.dylib")
  done
  lipo -create "${dylibs[@]}" -output "$slice_root/ChromaH264"
  make_framework "$slice" "$slice_root/ChromaH264" "$slice_root/ChromaH264.framework"
  FRAMEWORK_ARGS+=(-framework "$slice_root/ChromaH264.framework")
done

rm -rf "$OUT/ChromaH264.xcframework"
xcodebuild -create-xcframework "${FRAMEWORK_ARGS[@]}" -output "$OUT/ChromaH264.xcframework" >/dev/null

echo "=== built ==="
du -sh "$OUT/ChromaH264.xcframework" | sed 's/^/  /'
echo "  slices: $SLICES"
echo
echo "=== isolation check (the whole point) ==="
for slice in $SLICES; do
  bin="$BUILD/$slice/ChromaH264"
  [ -f "$bin" ] || continue
  exported_av=$(nm -gU "$bin" 2>/dev/null | grep -c "avcodec_\|avutil_\|av_" || true)
  exported_ch=$(nm -gU "$bin" 2>/dev/null | grep -c "_ch264_" || true)
  has_decoder=$(nm "$bin" 2>/dev/null | grep -c "ff_h264_decoder" || true)
  printf "  %-12s exports: ch264_=%s  av*=%s (must be 0)   contains ff_h264_decoder: %s (must be >0)\n" \
    "$slice" "$exported_ch" "$exported_av" "$has_decoder"
done
