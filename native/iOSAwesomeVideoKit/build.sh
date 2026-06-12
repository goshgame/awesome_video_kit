#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

PROJECT="iOSAwesomeVideoKit.xcodeproj"
SCHEME="AwesomeVideoKitSDK"
CONFIGURATION="${CONFIGURATION:-Release}"

DEFAULT_EXPORT_DIR="$SCRIPT_DIR/ExportSDK"
DEFAULT_BUILD_DIR="$SCRIPT_DIR/build"
EXPORT_DIR="${EXPORT_DIR:-$DEFAULT_EXPORT_DIR}"
BUILD_DIR="${BUILD_DIR:-$DEFAULT_BUILD_DIR}"
DERIVED_DATA_PATH="$BUILD_DIR/DerivedData"
XCFRAMEWORK_OUTPUT="$EXPORT_DIR/${SCHEME}.xcframework"
DEBUG_INFORMATION_FORMAT="${DEBUG_INFORMATION_FORMAT:-auto}"
INCLUDE_DSYMS="${INCLUDE_DSYMS:-auto}"
STRIP_FRAMEWORK="${STRIP_FRAMEWORK:-auto}"

IOS_ARCHIVE_PATH="$BUILD_DIR/${SCHEME}-iphoneos.xcarchive"
SIM_ARCHIVE_PATH="$BUILD_DIR/${SCHEME}-iphonesimulator.xcarchive"

IOS_EXPORT_DIR="$EXPORT_DIR/ios-arm64"
SIM_EXPORT_DIR="$EXPORT_DIR/ios-arm64-simulator"

is_debug_configuration() {
  case "$CONFIGURATION" in
    Debug|debug)
      return 0
      ;;
    *)
      return 1
      ;;
  esac
}

resolve_debug_information_format() {
  case "$DEBUG_INFORMATION_FORMAT" in
    auto)
      if is_debug_configuration; then
        printf 'dwarf-with-dsym\n'
      else
        printf 'dwarf\n'
      fi
      ;;
    *)
      printf '%s\n' "$DEBUG_INFORMATION_FORMAT"
      ;;
  esac
}

should_include_dsyms() {
  case "$INCLUDE_DSYMS" in
    1|true|TRUE|yes|YES)
      return 0
      ;;
    0|false|FALSE|no|NO)
      return 1
      ;;
    auto)
      is_debug_configuration
      return
      ;;
    *)
      printf 'Unsupported INCLUDE_DSYMS: %s (expected: auto|0|1)\n' "$INCLUDE_DSYMS" >&2
      exit 1
      ;;
  esac
}

should_strip_framework() {
  case "$STRIP_FRAMEWORK" in
    1|true|TRUE|yes|YES)
      return 0
      ;;
    0|false|FALSE|no|NO)
      return 1
      ;;
    auto)
      if is_debug_configuration; then
        return 1
      fi
      return 0
      ;;
    *)
      printf 'Unsupported STRIP_FRAMEWORK: %s (expected: auto|0|1)\n' "$STRIP_FRAMEWORK" >&2
      exit 1
      ;;
  esac
}

RESOLVED_DEBUG_INFORMATION_FORMAT="$(resolve_debug_information_format)"
if should_include_dsyms; then
  RESOLVED_INCLUDE_DSYMS="1"
else
  RESOLVED_INCLUDE_DSYMS="0"
fi

if should_strip_framework; then
  RESOLVED_STRIP_FRAMEWORK="1"
else
  RESOLVED_STRIP_FRAMEWORK="0"
fi

COMMON_FLAGS=(
  -project "$PROJECT"
  -scheme "$SCHEME"
  -configuration "$CONFIGURATION"
  -derivedDataPath "$DERIVED_DATA_PATH"
  BUILD_LIBRARY_FOR_DISTRIBUTION=YES
  SKIP_INSTALL=NO
  ONLY_ACTIVE_ARCH=NO
  CODE_SIGNING_ALLOWED=NO
  CODE_SIGNING_REQUIRED=NO
  CODE_SIGN_IDENTITY=
  DEBUG_INFORMATION_FORMAT="$RESOLVED_DEBUG_INFORMATION_FORMAT"
)

log_step() {
  printf '\n==> %s\n' "$1"
}

require_path() {
  local path="$1"
  if [[ ! -e "$path" ]]; then
    printf 'Missing required path: %s\n' "$path" >&2
    exit 1
  fi
}

archive_target() {
  local sdk="$1"
  local destination="$2"
  local archive_path="$3"

  log_step "Archive ${sdk}"
  xcodebuild archive \
    "${COMMON_FLAGS[@]}" \
    -sdk "$sdk" \
    -destination "$destination" \
    -archivePath "$archive_path" \
    ARCHS=arm64
}

append_debug_symbols_if_exists() {
  local debug_symbols_path="$1"
  if should_include_dsyms && [[ -d "$debug_symbols_path" ]]; then
    XCFRAMEWORK_FLAGS+=(-debug-symbols "$debug_symbols_path")
  fi
}

copy_if_exists() {
  local source_path="$1"
  local destination_dir="$2"
  if [[ -e "$source_path" ]]; then
    cp -R "$source_path" "$destination_dir/"
  fi
}

strip_framework_binary_if_requested() {
  local framework_path="$1"
  local binary_path="$framework_path/$SCHEME"

  if ! should_strip_framework; then
    return 0
  fi

  require_path "$binary_path"
  xcrun strip -S -x "$binary_path"
}

require_path "$PROJECT"
require_path "$SCRIPT_DIR/FFmpeg/ios-arm64/include"
require_path "$SCRIPT_DIR/FFmpeg/ios-arm64/lib/libavcodec.a"
require_path "$SCRIPT_DIR/FFmpeg/ios-arm64/lib/libavformat.a"
require_path "$SCRIPT_DIR/FFmpeg/ios-arm64/lib/libavutil.a"
require_path "$SCRIPT_DIR/FFmpeg/ios-arm64/lib/libavfilter.a"
require_path "$SCRIPT_DIR/FFmpeg/ios-arm64/lib/libswscale.a"
require_path "$SCRIPT_DIR/FFmpeg/ios-arm64/lib/libswresample.a"
require_path "$SCRIPT_DIR/FFmpeg/ios-arm64/lib/libx264.a"
require_path "$SCRIPT_DIR/FFmpeg/ios-arm64-simulator/include"
require_path "$SCRIPT_DIR/FFmpeg/ios-arm64-simulator/lib/libavcodec.a"
require_path "$SCRIPT_DIR/FFmpeg/ios-arm64-simulator/lib/libavformat.a"
require_path "$SCRIPT_DIR/FFmpeg/ios-arm64-simulator/lib/libavutil.a"
require_path "$SCRIPT_DIR/FFmpeg/ios-arm64-simulator/lib/libavfilter.a"
require_path "$SCRIPT_DIR/FFmpeg/ios-arm64-simulator/lib/libswscale.a"
require_path "$SCRIPT_DIR/FFmpeg/ios-arm64-simulator/lib/libswresample.a"
require_path "$SCRIPT_DIR/FFmpeg/ios-arm64-simulator/lib/libx264.a"

printf 'CONFIGURATION=%s\n' "$CONFIGURATION"
printf 'BUILD_DIR=%s\n' "$BUILD_DIR"
printf 'EXPORT_DIR=%s\n' "$EXPORT_DIR"
printf 'DEBUG_INFORMATION_FORMAT=%s\n' "$RESOLVED_DEBUG_INFORMATION_FORMAT"
printf 'INCLUDE_DSYMS=%s (raw=%s)\n' "$RESOLVED_INCLUDE_DSYMS" "$INCLUDE_DSYMS"
printf 'STRIP_FRAMEWORK=%s (raw=%s)\n' "$RESOLVED_STRIP_FRAMEWORK" "$STRIP_FRAMEWORK"

rm -rf "$EXPORT_DIR" "$BUILD_DIR"
mkdir -p "$EXPORT_DIR" "$IOS_EXPORT_DIR" "$SIM_EXPORT_DIR"

archive_target "iphoneos" "generic/platform=iOS" "$IOS_ARCHIVE_PATH"
archive_target "iphonesimulator" "generic/platform=iOS Simulator" "$SIM_ARCHIVE_PATH"

IOS_FRAMEWORK_PATH="$IOS_ARCHIVE_PATH/Products/Library/Frameworks/${SCHEME}.framework"
SIM_FRAMEWORK_PATH="$SIM_ARCHIVE_PATH/Products/Library/Frameworks/${SCHEME}.framework"
IOS_DSYM_PATH="$IOS_ARCHIVE_PATH/dSYMs/${SCHEME}.framework.dSYM"
SIM_DSYM_PATH="$SIM_ARCHIVE_PATH/dSYMs/${SCHEME}.framework.dSYM"

require_path "$IOS_FRAMEWORK_PATH"
require_path "$SIM_FRAMEWORK_PATH"

strip_framework_binary_if_requested "$IOS_FRAMEWORK_PATH"
strip_framework_binary_if_requested "$SIM_FRAMEWORK_PATH"

log_step "Create XCFramework"
XCFRAMEWORK_FLAGS=(
  -create-xcframework
  -framework "$IOS_FRAMEWORK_PATH"
)
append_debug_symbols_if_exists "$IOS_DSYM_PATH"
XCFRAMEWORK_FLAGS+=(-framework "$SIM_FRAMEWORK_PATH")
append_debug_symbols_if_exists "$SIM_DSYM_PATH"
XCFRAMEWORK_FLAGS+=(-output "$XCFRAMEWORK_OUTPUT")
xcodebuild "${XCFRAMEWORK_FLAGS[@]}"

log_step "Export build artifacts"
copy_if_exists "$IOS_FRAMEWORK_PATH" "$IOS_EXPORT_DIR"
copy_if_exists "$SIM_FRAMEWORK_PATH" "$SIM_EXPORT_DIR"
if should_include_dsyms; then
  copy_if_exists "$IOS_DSYM_PATH" "$IOS_EXPORT_DIR"
  copy_if_exists "$SIM_DSYM_PATH" "$SIM_EXPORT_DIR"
fi

printf '\nDone: %s\n' "$XCFRAMEWORK_OUTPUT"
