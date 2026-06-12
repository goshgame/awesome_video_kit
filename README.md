# awesome_video_kit

`awesome_video_kit` 是一个面向 iOS 和 Android 的 Flutter 视频处理插件。插件通过 Dart API 封装原生 `AwesomeVideoKitSDK` / JNI 能力，底层使用 FFmpeg、x264、mBedTLS 完成下载、转码、音频提取和媒体信息读取。

## 功能

- 下载 HLS/m3u8 网络视频并输出 MP4
- 本地视频转码为 MP4
- 下载或转码时添加图片水印
- 在视频头部或尾部拼接静态图片
- 将图片或视频与独立音频合成为 MP4
- 提取媒体文件首路音频
- 获取视频指定时间点的画面图片，支持单个或多个时间点
- 读取音频、视频、图片的媒体信息
- 提供下载、转码、音频提取进度流
- 支持取消下载、取消转码、取消音频提取
- 支持转码暂停和恢复

## 平台要求

| 平台 | 要求 |
| --- | --- |
| Flutter | `>=3.3.0` |
| Dart | `^3.8.1` |
| Android | `minSdk 21` |
| iOS | Podspec 声明 `iOS 12.0+` |

Android 需要在宿主 App 的 `AndroidManifest.xml` 中声明网络权限：

```xml
<uses-permission android:name="android.permission.INTERNET" />
```

如果传入的是 App 沙盒外部文件路径，请确保宿主 App 已自行处理文件选择、权限和可访问路径。插件不负责申请相册、文件管理或存储权限。

## 安装

在宿主 Flutter 项目的 `pubspec.yaml` 中添加本地依赖：

```yaml
dependencies:
  awesome_video_kit:
    path: ../awesome_video_kit
```

然后执行：

```bash
flutter pub get
```

iOS 项目首次接入或原生依赖更新后，建议重新安装 Pods：

```bash
cd ios
pod install
```

## 快速开始

```dart
import 'package:awesome_video_kit/awesome_video_kit.dart';
```

监听任务进度：

```dart
final downloadSub = AwesomeVideoKit.downloadProgressStream.listen((progress) {
  print('download: $progress%');
});

final transcodeSub = AwesomeVideoKit.transcodeProgressStream.listen((progress) {
  print('transcode: $progress%');
});

final extractSub = AwesomeVideoKit.extractAudioProgressStream.listen((progress) {
  print('extract audio: $progress%');
});
```

下载 m3u8 并输出 MP4：

```dart
final outputPath = await AwesomeVideoKit.downloadVideo(
  url: 'https://example.com/live/index.m3u8',
  overwrite: true,
);
```

下载时添加水印：

```dart
final outputPath = await AwesomeVideoKit.downloadVideo(
  url: 'https://example.com/live/index.m3u8',
  watermarkImagePath: '/path/to/watermark.png',
  watermarkPosition: VideoWatermarkPosition.bottomRight,
);
```

本地视频转码：

```dart
final outputPath = await AwesomeVideoKit.transcodeVideo(
  inputPath: '/path/to/input.mov',
  outputPath: '/path/to/output.mp4',
  options: const VideoTranscodeOptions(
    crf: 23,
    preset: 'fast',
    profile: 'high',
    level: '4.1',
    videoWidth: 720,
    faststart: true,
    frameRate: 30,
  ),
);
```

转码时添加水印：

```dart
final outputPath = await AwesomeVideoKit.transcodeVideo(
  inputPath: '/path/to/input.mp4',
  options: const VideoTranscodeOptions(
    watermarkImagePath: '/path/to/watermark.png',
    watermarkPosition: VideoWatermarkPosition.center,
  ),
);
```

拼接片头或片尾图片：

```dart
final outputPath = await AwesomeVideoKit.transcodeVideoWithConcatImage(
  inputPath: '/path/to/input.mp4',
  options: const VideoConcatImageOptions(
    concatImagePath: '/path/to/end.jpg',
    imageDuration: Duration(seconds: 3),
    concatPosition: VideoConcatImagePosition.tail,
  ),
);
```

图片或视频叠加独立音频：

```dart
final outputPath = await AwesomeVideoKit.transcodeMediaWithAudio(
  inputPath: '/path/to/cover.jpg',
  audioPath: '/path/to/audio.m4a',
  options: const MediaWithAudioOptions(
    audioBitrate: 128000,
    faststart: true,
    frameRate: 25,
  ),
);
```

提取音频：

```dart
final audioPath = await AwesomeVideoKit.extractAudio(
  inputPath: '/path/to/input.mp4',
  overwrite: true,
);
```

截取单个时间点的视频画面：

```dart
final imagePath = await AwesomeVideoKit.captureVideoFrame(
  inputPath: '/path/to/input.mp4',
  time: const Duration(seconds: 3, milliseconds: 500),
  outputPath: '/path/to/frame_3500ms.jpg',
  overwrite: true,
  outputWidth: 640,
  outputHeight: 360,
  fastMode: true,
);
```

一次截取多个时间点的视频画面：

```dart
final imagePaths = await AwesomeVideoKit.captureVideoFrames(
  inputPath: '/path/to/input.mp4',
  times: const [
    Duration(seconds: 1),
    Duration(seconds: 3),
    Duration(seconds: 5),
  ],
  outputDirectory: '/path/to/frames',
  overwrite: true,
  outputWidth: 640,
  outputHeight: 360,
  fastMode: true,
);
```

读取媒体信息：

```dart
final info = await AwesomeVideoKit.getMediaInfo(
  filePath: '/path/to/input.mp4',
);

if (info != null) {
  print('duration: ${info.duration}');
  print('video streams: ${info.videoStreamCount}');
  print('audio streams: ${info.audioStreamCount}');
  print('primary size: ${info.primaryVideoDimension?.width}x'
      '${info.primaryVideoDimension?.height}');
}
```

取消、暂停、恢复任务：

```dart
await AwesomeVideoKit.cancelDownload();
await AwesomeVideoKit.cancelExtractAudio();
await AwesomeVideoKit.cancelTranscode();

await AwesomeVideoKit.pauseTranscode();
await AwesomeVideoKit.resumeTranscode();
```

查询任务状态：

```dart
final downloading = await AwesomeVideoKit.isDownloading();
final transcoding = await AwesomeVideoKit.isTranscoding();
final extractingAudio = await AwesomeVideoKit.isExtractingAudio();
final paused = await AwesomeVideoKit.isTranscodePaused();
```

## API 说明

### 输出路径

多数方法都支持传入 `outputPath`：

- 传入 `outputPath`：使用指定路径输出。
- 不传 `outputPath`：由原生侧自动生成默认路径。
- `overwrite` 为 `true` 时，如果目标文件已存在会尝试覆盖。

`captureVideoFrame` / `captureVideoFrames` 当前通过 FFmpeg MJPEG 编码器输出 JPEG 图片：

- `inputPath` 支持本地视频路径，也支持 `http` / `https` 网络视频 URL。
- 单时间点可传 `outputPath`，不传时自动生成 `.jpg` 路径；如果传入目录，会在该目录下自动生成文件名。
- `outputWidth` / `outputHeight` 都大于 0 时会按指定分辨率缩放输出图片；只传宽或只传高时会按源视频比例自动补齐另一边；都不传或都传 `0` 时保持源视频帧尺寸。
- `fastMode` 为 `true` 时优先截帧速度，可能返回目标时间点附近的帧；默认 `false` 时优先时间点准确性。
- 多时间点可传 `outputDirectory`，每个时间点会生成一张 `.jpg` 图片，返回路径列表顺序与传入 `times` 顺序一致。

Android 当前默认输出目录位于 App cache 下的 `awesome_video_kit/` 子目录，例如：

```text
<app-cache-dir>/awesome_video_kit/
```

### 转码参数

`VideoTranscodeOptions` 常用字段：

| 字段 | 说明 |
| --- | --- |
| `crf` | 视频质量，通常值越小质量越高；`0` 表示使用默认值 |
| `preset` | x264 编码预设，例如 `veryfast`、`fast`、`medium` |
| `profile` | 编码 profile，例如 `baseline`、`main`、`high` |
| `level` | 编码 level，例如 `3.1`、`4.1` |
| `videoWidth` | 输出宽度，`0` 表示保持原宽度 |
| `videoHeight` | 输出高度，`0` 表示保持原高度 |
| `audioBitrate` | 输出音频码率，单位通常为 bps；`0` 表示默认值 |
| `faststart` | 是否启用 MP4 faststart |
| `frameRate` | 输出帧率，`0` 表示保持原帧率 |
| `watermarkImagePath` | 可选水印图片路径 |
| `watermarkPosition` | 水印位置 |

水印位置：

```dart
VideoWatermarkPosition.topLeft
VideoWatermarkPosition.topRight
VideoWatermarkPosition.bottomLeft
VideoWatermarkPosition.bottomRight
VideoWatermarkPosition.center
VideoWatermarkPosition.alternatingTopLeftBottomRight
```

### 媒体信息

`getMediaInfo` 支持音频、视频和图片文件。返回的 `MediaInfo` 包含：

- 文件类型
- 总时长
- 数据码率
- 视频流数量
- 音频流数量
- 源文件路径
- 视频流详情
- 音频流详情

视频流详情包含分辨率、帧率、旋转角度、编码类型、HDR 类型等字段。

## 示例工程

示例工程位于：

```text
awesome_video_kit/example
```

运行：

```bash
cd awesome_video_kit/example
flutter pub get
flutter run
```

## 原生产物

插件随包包含预编译原生产物：

- Android: `android/src/main/jniLibs/`
- iOS: `ios/Framework/AwesomeVideoKitSDK.xcframework`

更新底层 FFmpeg、x264、mBedTLS 或原生 SDK 后，需要重新生成这些产物并同步到 Flutter 插件目录。

依赖库构建脚本位于：

```text
native/DependencyLibraries/scripts
```

原生 SDK 构建入口：

```text
native/AndroidAwesomeVideoKit/build.sh
native/iOSAwesomeVideoKit/build.sh
```

Android 与 iOS 共用的 FFmpeg C/C++ 业务源码位于：

```text
native/shared/ffmpeg_tools
```

## 维护提示

新增或修改 Dart API 时，通常需要同步以下位置：

- `lib/awesome_video_kit.dart`
- `lib/awesome_video_kit_platform_interface.dart`
- `lib/awesome_video_kit_method_channel.dart`
- `android/src/main/kotlin/`
- `ios/Classes/`
- Android JNI / C++ 实现
- iOS Objective-C / C++ SDK 实现

枚举值是 Dart 与原生层之间的协议，不能随意调整已有数字值。

## 错误处理

原生方法返回空输出路径时，Dart 层会抛出 `PlatformException(code: 'NO_OUTPUT')`。实际媒体处理错误由平台侧返回，调用方应使用 `try/catch` 包裹耗时任务：

```dart
try {
  final output = await AwesomeVideoKit.transcodeVideo(
    inputPath: '/path/to/input.mp4',
  );
  print(output);
} catch (error) {
  print('task failed: $error');
}
```
