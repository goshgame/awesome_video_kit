#!/bin/bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=android_build_common.sh
source "$SCRIPT_DIR/android_build_common.sh"

TARGET="${1:-${ANDROID_TARGET:-all}}"
if [[ "$TARGET" != "all" ]]; then
  TARGET="$(canonicalize_android_abi "$TARGET")"
fi

sync_ffmpeg_source_copy() {
  require_dir "$FFMPEG_SOURCE_DIR"
  mkdir -p "$FFMPEG_SOURCE_COPY"
  rsync -a --delete \
    --exclude '/.git' \
    --exclude '/build' \
    --exclude '/android-*' \
    --exclude '/install' \
    "$FFMPEG_SOURCE_DIR"/ "$FFMPEG_SOURCE_COPY"/
}

generate_x264_pkg_config() {
  local x264_prefix="$1"
  local pkg_config_work_dir="$2"

  mkdir -p "$pkg_config_work_dir"
  cat >"$pkg_config_work_dir/x264.pc" <<EOF
prefix=$x264_prefix
exec_prefix=\${prefix}
libdir=\${exec_prefix}/lib
includedir=\${prefix}/include

Name: x264
Description: H.264 (MPEG4 AVC) encoder library
Version: 0.165.x
Libs: -L\${exec_prefix}/lib -lx264 -lm
Libs.private:
Cflags: -I\${prefix}/include
EOF
}

rewrite_pkgconfig_prefixes() {
  local install_root="$1"
  local pkgconfig_dir="$install_root/lib/pkgconfig"
  local pc_file
  local pc_list

  [[ -d "$pkgconfig_dir" ]] || return 0

  pc_list="$(mktemp "${TMPDIR:-/tmp}/ffmpeg-android-pkgconfig.XXXXXX")"
  find "$pkgconfig_dir" -type f -name '*.pc' -print0 >"$pc_list"

  while IFS= read -r -d '' pc_file; do
    sed -i.bak \
      -e "s|^prefix=.*$|prefix=$install_root|" \
      -e "s|^libdir=.*$|libdir=$install_root/lib|" \
      -e "s|^includedir=.*$|includedir=$install_root/include|" \
      "$pc_file"
    rm -f "$pc_file.bak"
  done <"$pc_list"

  rm -f "$pc_list"
}

sync_install_tree() {
  local src_root="$1"
  local dst_root="$2"
  local component

  mkdir -p "$dst_root"
  for component in include lib share; do
    if [[ -d "$src_root/$component" ]]; then
      mkdir -p "$dst_root/$component"
      rsync -a --delete "$src_root/$component"/ "$dst_root/$component"/
    else
      rm -rf "$dst_root/$component"
    fi
  done
}

build_ffmpeg_for_abi() {
  local abi="$1"
  local cc
  local cxx
  local ar
  local nm
  local ranlib
  local strip
  local pkg_config_path
  local configure_args
  local clang_triple
  local ffmpeg_arch
  local ffmpeg_cpu
  local ffmpeg_base_cflags
  local mbedtls_prefix
  local x264_prefix
  local ffmpeg_stage_prefix
  local ffmpeg_install_prefix
  local pkg_config_work_dir

  mbedtls_prefix="$(mbedtls_prefix_for_abi "$abi")"
  x264_prefix="$(x264_prefix_for_abi "$abi")"
  ffmpeg_stage_prefix="$(ffmpeg_stage_prefix_for_abi "$abi")"
  ffmpeg_install_prefix="$(ffmpeg_install_prefix_for_abi "$abi")"
  pkg_config_work_dir="$(pkg_config_work_dir_for_abi "$abi")"

  require_file "$x264_prefix/include/x264.h"
  require_file "$x264_prefix/lib/libx264.a"
  require_file "$mbedtls_prefix/include/mbedtls/ssl.h"
  require_file "$mbedtls_prefix/lib/libmbedtls.a"
  require_file "$mbedtls_prefix/lib/libmbedx509.a"
  require_file "$mbedtls_prefix/lib/libmbedcrypto.a"

  sync_ffmpeg_source_copy
  generate_x264_pkg_config "$x264_prefix" "$pkg_config_work_dir"

  log_step "Build FFmpeg for Android ($abi)"
  rm -rf "$ffmpeg_stage_prefix"
  mkdir -p "$ffmpeg_stage_prefix"

  clang_triple="$(toolchain_triple_for_abi "$abi")"
  ffmpeg_arch="$(ffmpeg_arch_for_abi "$abi")"
  ffmpeg_cpu="$(ffmpeg_cpu_for_abi "$abi")"
  ffmpeg_base_cflags="$(ffmpeg_base_cflags_for_abi "$abi")"
  cc="$TOOLCHAIN_DIR/bin/${clang_triple}${ANDROID_API}-clang"
  cxx="$TOOLCHAIN_DIR/bin/${clang_triple}${ANDROID_API}-clang++"
  ar="$TOOLCHAIN_DIR/bin/llvm-ar"
  nm="$TOOLCHAIN_DIR/bin/llvm-nm"
  ranlib="$TOOLCHAIN_DIR/bin/llvm-ranlib"
  strip="$TOOLCHAIN_DIR/bin/llvm-strip"
  pkg_config_path="$pkg_config_work_dir:$mbedtls_prefix/lib/pkgconfig"

  require_file "$cc"
  require_file "$cxx"
  require_file "$ar"
  require_file "$nm"
  require_file "$ranlib"
  require_file "$strip"

  configure_args=(
    --prefix="$ffmpeg_stage_prefix"
    --target-os=android
    --arch="$ffmpeg_arch"
    --cpu="$ffmpeg_cpu"
    --enable-cross-compile
    --sysroot="$SYSROOT"
    --cc="$cc"
    --cxx="$cxx"
    --ar="$ar"
    --nm="$nm"
    --ranlib="$ranlib"
    --strip="$strip"
    --pkg-config="$PKG_CONFIG_BIN"
    --pkg-config-flags=--static
    --optflags="$SIZE_OPTFLAGS"
    --extra-cflags="$ffmpeg_base_cflags -I$mbedtls_prefix/include -I$x264_prefix/include"
    --extra-ldflags="-Wl,--gc-sections -L$mbedtls_prefix/lib -L$x264_prefix/lib"
    --extra-libs="-lmbedtls -lmbedx509 -lmbedcrypto"
    --enable-static
    --disable-shared
    --enable-pic
    --enable-small
    --disable-autodetect
    --disable-symver
    --disable-runtime-cpudetect
    --disable-bzlib
    --disable-iconv
    --disable-vulkan
    --disable-everything
    --enable-pthreads
    --enable-gpl
    --enable-version3
    --enable-zlib
    --enable-jni
    --enable-mediacodec
    --enable-mbedtls
    --enable-avformat
    --enable-avcodec
    --enable-avutil
    --enable-avfilter
    --enable-swresample
    --enable-swscale
    --enable-network
    --enable-libx264
    --enable-protocol=file,http,https,tcp,tls
    --enable-demuxer=hls,mpegts,mov,image2,image2pipe
    --enable-muxer=mov,mp4,fragmented_mp4,m4v
    --enable-bsf=aac_adtstoasc,h264_mp4toannexb,hevc_mp4toannexb,h264_metadata,hevc_metadata
    --enable-decoder=h264,hevc,aac,aac_latm,png,mjpeg,h264_mediacodec,hevc_mediacodec
    --enable-encoder=aac,libx264,mjpeg,h264_mediacodec,hevc_mediacodec
    --enable-parser=h264,hevc,aac
    --enable-filter=overlay,color,buffersrc,buffersink,format,scale,fps,trim,setpts,asetpts,anullsrc,atrim,concat
    --disable-programs
    --disable-doc
    --disable-debug
    --disable-avdevice
    --disable-postproc
  )

  if [[ "$DISABLE_LOGGING" == "1" ]]; then
    configure_args+=(--disable-logging)
  fi

  if [[ "$ENABLE_LTO" == "1" ]]; then
    configure_args+=(--enable-lto)
  fi

  if [[ "$AGGRESSIVE_SIZE" == "1" ]]; then
    configure_args+=(
      --disable-asm
      --disable-inline-asm
      --disable-neon
      --disable-dotprod
      --disable-i8mm
    )
  fi

  pushd "$FFMPEG_SOURCE_COPY" >/dev/null
  make distclean >/dev/null 2>&1 || true

  export PKG_CONFIG_PATH="$pkg_config_path"
  export PKG_CONFIG_LIBDIR="$pkg_config_path"

  ./configure "${configure_args[@]}"
  make -j"$JOBS"
  make install
  popd >/dev/null

  sync_install_tree "$ffmpeg_stage_prefix" "$ffmpeg_install_prefix"
  rewrite_pkgconfig_prefixes "$ffmpeg_install_prefix"
}

main() {
  require_command find
  require_command make
  require_command rsync
  require_command sed
  require_command "$PKG_CONFIG_BIN"
  require_dir "$ANDROID_PROJECT_DIR"
  require_dir "$FFMPEG_SOURCE_DIR"
  configure_android_toolchain
  validate_android_api_for_target "$TARGET"
  print_android_environment
  run_for_android_target "$TARGET" build_ffmpeg_for_abi
}

main "$@"
