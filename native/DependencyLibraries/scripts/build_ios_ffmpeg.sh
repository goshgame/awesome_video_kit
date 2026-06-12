#!/bin/bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=ios_build_common.sh
source "$SCRIPT_DIR/ios_build_common.sh"

TARGET="${1:-${IOS_TARGET:-all}}"

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

rewrite_pkgconfig_prefixes() {
  local install_root="$1"
  local pkgconfig_dir="$install_root/lib/pkgconfig"
  local pc_file
  local pc_list

  [[ -d "$pkgconfig_dir" ]] || return 0

  pc_list="$(mktemp "${TMPDIR:-/tmp}/ffmpeg-ios-pkgconfig.XXXXXX")"
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

  rm -rf "$dst_root"
  mkdir -p "$dst_root"

  for component in include lib share; do
    if [[ -d "$src_root/$component" ]]; then
      mkdir -p "$dst_root/$component"
      rsync -a --delete "$src_root/$component"/ "$dst_root/$component"/
    fi
  done
}

write_x264_pkg_config_wrapper() {
  local wrapper_path="$1"

  cat >"$wrapper_path" <<'EOF'
#!/bin/sh

if [ "$1" = "--version" ] || [ "$1" = "--modversion" ]; then
  echo "0.165.x"
  exit 0
fi

if [ "$1" = "--exists" ]; then
  exit 0
fi

if [ "$1" = "--cflags" ]; then
  printf '%s\n' "${X264_PKG_CFLAGS:-}"
  exit 0
fi

if [ "$1" = "--libs" ] || [ "${2:-}" = "--libs" ]; then
  printf '%s\n' "${X264_PKG_LIBS:-}"
  exit 0
fi

case "$*" in
  *--cflags*)
    printf '%s\n' "${X264_PKG_CFLAGS:-}"
    ;;
  *--libs*)
    printf '%s\n' "${X264_PKG_LIBS:-}"
    ;;
  *)
    exit 0
    ;;
esac
EOF

  chmod +x "$wrapper_path"
}

copy_x264_artifacts() {
  local x264_prefix="$1"
  local install_prefix="$2"

  mkdir -p "$install_prefix/include/libx264" "$install_prefix/lib/pkgconfig"

  cp "$x264_prefix/lib/libx264.a" "$install_prefix/lib/libx264.a"
  cp "$x264_prefix/include/x264.h" "$install_prefix/include/libx264/x264.h"

  if [[ -f "$x264_prefix/include/x264_config.h" ]]; then
    cp "$x264_prefix/include/x264_config.h" "$install_prefix/include/libx264/x264_config.h"
  fi

  if [[ -d "$x264_prefix/lib/pkgconfig" ]]; then
    rsync -a "$x264_prefix/lib/pkgconfig"/ "$install_prefix/lib/pkgconfig"/
  fi
}

copy_mbedtls_artifacts() {
  local mbedtls_prefix="$1"
  local install_prefix="$2"
  local library

  mkdir -p "$install_prefix/include" "$install_prefix/lib/pkgconfig"
  rsync -a "$mbedtls_prefix/include"/ "$install_prefix/include"/

  for library in libmbedtls.a libmbedx509.a libmbedcrypto.a; do
    cp "$mbedtls_prefix/lib/$library" "$install_prefix/lib/$library"
  done

  if [[ -d "$mbedtls_prefix/lib/pkgconfig" ]]; then
    rsync -a "$mbedtls_prefix/lib/pkgconfig"/ "$install_prefix/lib/pkgconfig"/
  fi
}

build_ffmpeg_for_sdk() {
  local sdk_name="$1"
  local arch
  local sdk_path
  local cc
  local x264_prefix
  local mbedtls_prefix
  local stage_prefix
  local install_prefix
  local min_flag
  local extra_cflags
  local extra_ldflags
  local configure_args

  arch="$(ios_arch_for_sdk "$sdk_name")"
  sdk_path="$(xcrun --sdk "$sdk_name" --show-sdk-path)"
  cc="$(xcrun --sdk "$sdk_name" -f clang)"
  x264_prefix="$(x264_prefix_for_sdk "$sdk_name")"
  mbedtls_prefix="$(mbedtls_prefix_for_sdk "$sdk_name")"
  stage_prefix="$(ffmpeg_stage_prefix_for_sdk "$sdk_name")"
  install_prefix="$(ffmpeg_install_prefix_for_sdk "$sdk_name")"

  require_file "$x264_prefix/include/x264.h"
  require_file "$x264_prefix/lib/libx264.a"
  require_file "$mbedtls_prefix/include/mbedtls/ssl.h"
  require_file "$mbedtls_prefix/lib/libmbedtls.a"
  require_file "$mbedtls_prefix/lib/libmbedx509.a"
  require_file "$mbedtls_prefix/lib/libmbedcrypto.a"

  sync_ffmpeg_source_copy

  log_step "Build FFmpeg for $sdk_name ($arch)"
  rm -rf "$stage_prefix"
  mkdir -p "$stage_prefix"

  case "$sdk_name" in
    iphoneos)
      min_flag="-mios-version-min=$MIN_IOS_VERSION"
      ;;
    iphonesimulator)
      min_flag="-mios-simulator-version-min=$MIN_IOS_VERSION"
      ;;
    *)
      fail "Unsupported SDK: $sdk_name"
      ;;
  esac

  extra_cflags="-arch $arch -isysroot $sdk_path $min_flag $SIZE_OPTFLAGS -I$x264_prefix/include -I$mbedtls_prefix/include"
  extra_ldflags="-arch $arch -isysroot $sdk_path $min_flag $SIZE_OPTFLAGS -L$x264_prefix/lib -L$mbedtls_prefix/lib"

  if [[ "$AGGRESSIVE_SIZE" == "1" ]]; then
    extra_cflags="$extra_cflags -fno-unwind-tables -fno-asynchronous-unwind-tables"
  fi

  configure_args=(
    --prefix="$stage_prefix"
    --cc="$cc"
    --sysroot="$sdk_path"
    --pkg-config="$FFMPEG_SOURCE_COPY/pkg-config-x264.sh"
    --extra-cflags="$extra_cflags"
    --extra-ldflags="$extra_ldflags"
    --extra-libs="-lmbedtls -lmbedx509 -lmbedcrypto"
    --target-os=darwin
    --arch="$arch"
    --enable-cross-compile
    --enable-pic
    --enable-small
    --enable-static
    --disable-shared
    --disable-autodetect
    --disable-runtime-cpudetect
    --disable-bzlib
    --disable-iconv
    --disable-securetransport
    --enable-videotoolbox
    --enable-mbedtls
    --enable-version3
    --enable-zlib
    --disable-everything
    --enable-avformat
    --enable-avcodec
    --enable-avutil
    --enable-avfilter
    --enable-swresample
    --enable-swscale
    --enable-network
    --enable-filter=overlay,color,buffersrc,buffersink,format,scale,fps,trim,setpts,asetpts,anullsrc,atrim,concat
    --enable-protocol=http,https,tcp,tls,file
    --enable-muxer=mov,mp4,fragmented_mp4,m4v
    --enable-demuxer=hls,mpegts,mov
    --enable-decoder=h264,hevc,aac,aac_latm,png,mjpeg
    --enable-hwaccel=h264_videotoolbox,hevc_videotoolbox
    --enable-libx264
    --enable-encoder=libx264,aac,mjpeg,h264_videotoolbox,hevc_videotoolbox
    --enable-parser=h264,hevc,aac
    --enable-bsf=aac_adtstoasc,h264_mp4toannexb,hevc_mp4toannexb
    --enable-pthreads
    --enable-gpl
    --pkg-config-flags=--static
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
    )
  fi

  pushd "$FFMPEG_SOURCE_COPY" >/dev/null
  make distclean >/dev/null 2>&1 || true
  write_x264_pkg_config_wrapper "$FFMPEG_SOURCE_COPY/pkg-config-x264.sh"

  export X264_PKG_PREFIX="$x264_prefix"
  export X264_PKG_CFLAGS="-I$x264_prefix/include"
  export X264_PKG_LIBS="-L$x264_prefix/lib -lx264 -lm"
  export PKG_CONFIG_PATH="$mbedtls_prefix/lib/pkgconfig"
  export PKG_CONFIG_LIBDIR="$mbedtls_prefix/lib/pkgconfig"

  ./configure "${configure_args[@]}"
  make -j"$JOBS"
  make install
  popd >/dev/null

  sync_install_tree "$stage_prefix" "$install_prefix"
  copy_x264_artifacts "$x264_prefix" "$install_prefix"
  copy_mbedtls_artifacts "$mbedtls_prefix" "$install_prefix"
  rewrite_pkgconfig_prefixes "$install_prefix"
}

main() {
  require_command find
  require_command make
  require_command rsync
  require_command sed
  require_command xcrun
  require_dir "$IOS_PROJECT_DIR"
  require_dir "$FFMPEG_SOURCE_DIR"
  validate_ios_arches
  print_ios_environment
  run_for_ios_target "$TARGET" build_ffmpeg_for_sdk
}

main "$@"
