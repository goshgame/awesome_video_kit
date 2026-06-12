# 依赖库构建脚本说明

本目录提供一套分层构建脚本，用于拉取和编译 FFmpeg、x264、mBedTLS。旧脚本 `build_ffmpeg_mbedtls_ios.sh` 和 `build_ffmpeg_mbedtls_android.sh` 保留不动；新脚本按“拉源码、单库构建、总入口构建”的方式组织。

## 脚本结构

| 脚本 | 说明 |
| --- | --- |
| `fetch_dependency_sources.sh` | 拉取 FFmpeg、x264、mBedTLS 源码 |
| `build_ios_x264.sh` | 单独编译 iOS x264 |
| `build_ios_mbedtls.sh` | 单独编译 iOS mBedTLS |
| `build_ios_ffmpeg.sh` | 单独编译 iOS FFmpeg，并链接 x264 / mBedTLS |
| `build_ios_all.sh` | iOS 总入口，按 x264 -> mBedTLS -> FFmpeg 顺序构建 |
| `build_android_x264.sh` | 单独编译 Android x264 |
| `build_android_mbedtls.sh` | 单独编译 Android mBedTLS |
| `build_android_ffmpeg.sh` | 单独编译 Android FFmpeg，并链接 x264 / mBedTLS |
| `build_android_all.sh` | Android 总入口，按 x264 -> mBedTLS -> FFmpeg 顺序构建 |
| `ios_build_common.sh` | iOS 公共配置，不建议直接执行 |
| `android_build_common.sh` | Android 公共配置，不建议直接执行 |

## 拉取源码

默认源码目录：

- FFmpeg: `native/DependencyLibraries/FFmpeg-n6.1.1`
- x264: `native/DependencyLibraries/x264`
- mBedTLS: `native/DependencyLibraries/mbedtls/mbedtls`

执行：

```bash
native/DependencyLibraries/scripts/fetch_dependency_sources.sh
```

默认版本：

- FFmpeg: `n6.1.1`
- x264: `master`
- mBedTLS: `v3.6.5`

可以通过环境变量覆盖：

```bash
FFMPEG_REF=n6.1.1 \
X264_REF=master \
MBEDTLS_REF=v3.6.5 \
native/DependencyLibraries/scripts/fetch_dependency_sources.sh
```

## iOS 构建

构建全部 iOS 依赖：

```bash
native/DependencyLibraries/scripts/build_ios_all.sh all
```

只构建真机：

```bash
native/DependencyLibraries/scripts/build_ios_all.sh device
```

只构建模拟器：

```bash
native/DependencyLibraries/scripts/build_ios_all.sh simulator
```

单独构建某个库：

```bash
native/DependencyLibraries/scripts/build_ios_x264.sh all
native/DependencyLibraries/scripts/build_ios_mbedtls.sh all
native/DependencyLibraries/scripts/build_ios_ffmpeg.sh all
```

iOS 默认输出：

- x264: `native/DependencyLibraries/build/x264-ios`
- mBedTLS: `native/DependencyLibraries/mbedtls/mbedtls/out`
- FFmpeg: `native/iOSAwesomeVideoKit/FFmpeg`

## Android 构建

构建全部 Android 依赖：

```bash
native/DependencyLibraries/scripts/build_android_all.sh all
```

只构建 arm64-v8a：

```bash
native/DependencyLibraries/scripts/build_android_all.sh arm64-v8a
```

只构建 armeabi-v7a：

```bash
native/DependencyLibraries/scripts/build_android_all.sh armeabi-v7a
```

单独构建某个库：

```bash
native/DependencyLibraries/scripts/build_android_x264.sh all
native/DependencyLibraries/scripts/build_android_mbedtls.sh all
native/DependencyLibraries/scripts/build_android_ffmpeg.sh all
```

Android 默认输出：

- x264: `native/DependencyLibraries/build/x264-android`
- mBedTLS: `native/DependencyLibraries/build/mbedtls-android-install`
- FFmpeg: `native/AndroidAwesomeVideoKit/jniLibs`

## 拉源码并构建

iOS：

```bash
FETCH_SOURCES=1 native/DependencyLibraries/scripts/build_ios_all.sh all
```

Android：

```bash
FETCH_SOURCES=1 native/DependencyLibraries/scripts/build_android_all.sh all
```

## 常用环境变量

| 变量 | 默认值 / 说明 |
| --- | --- |
| `MIN_IOS_VERSION` | iOS 最低版本，默认 `13.0` |
| `ANDROID_API` | Android API，默认 `21` |
| `JOBS` | 并行编译数，默认自动读取 CPU 数 |
| `SIZE_OPTFLAGS` | 编译优化参数，默认 `-Oz` |
| `ENABLE_LTO` | 是否启用 LTO，默认 `1` |
| `DISABLE_LOGGING` | 是否关闭 FFmpeg 日志，默认 `1` |
| `AGGRESSIVE_SIZE` | 更激进的体积优化，默认 `0` |
| `X264_SOURCE_DIR` | x264 源码目录 |
| `X264_ROOT` | x264 安装输出目录 |
| `MBEDTLS_SOURCE_DIR` | mBedTLS 源码目录 |
| `MBEDTLS_INSTALL_ROOT` | mBedTLS 安装输出目录 |
| `FFMPEG_SOURCE_DIR` | FFmpeg 源码目录 |
| `FFMPEG_INSTALL_ROOT` | FFmpeg 安装输出目录 |

示例：

```bash
ANDROID_API=23 JOBS=8 native/DependencyLibraries/scripts/build_android_all.sh arm64-v8a
MIN_IOS_VERSION=14.0 native/DependencyLibraries/scripts/build_ios_all.sh device
```

## 注意事项

1. 新脚本不会自动删除或修改旧脚本。
2. `build_ios_ffmpeg.sh` 和 `build_android_ffmpeg.sh` 都要求 x264、mBedTLS 已经先编译完成。
3. Android 新脚本把 x264 和 mBedTLS 放在 `native/DependencyLibraries/build` 下。后续如果使用 `native/AndroidAwesomeVideoKit/build.sh` 重新打 `libAwesomeVideoKit.so`，需要传入新的依赖路径，例如：

```bash
X264_PREFIX="$PWD/native/DependencyLibraries/build/x264-android/android-arm64-v8a" \
MBEDTLS_PREFIX="$PWD/native/DependencyLibraries/build/mbedtls-android-install/android-arm64" \
ANDROID_ABI=arm64-v8a \
native/AndroidAwesomeVideoKit/build.sh
```

4. `fetch_dependency_sources.sh` 需要网络访问。已有源码目录如果是 git 仓库，会执行 fetch 和 checkout；如果目录存在但不是 git 仓库，脚本会停止，避免覆盖本地文件。
