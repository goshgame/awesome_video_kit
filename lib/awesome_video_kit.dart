import 'awesome_video_kit_platform_interface.dart';
import 'src/media_info_models.dart';
import 'src/video_task_models.dart';

export 'src/media_info_models.dart';
export 'src/video_task_models.dart';

class AwesomeVideoKit {
  @Deprecated('Use static members on AwesomeVideoKit directly.')
  const AwesomeVideoKit();

  static AwesomeVideoKitPlatform get _platform =>
      AwesomeVideoKitPlatform.instance;

  /// 获取当前平台版本信息。
  ///
  /// Android 侧通常返回类似 `Android 14`，
  /// iOS 侧通常返回类似 `iOS 18.0`。
  static Future<String?> getPlatformVersion() {
    return _platform.getPlatformVersion();
  }

  /// 下载任务进度流。
  ///
  /// 进度范围为 `0~100`，表示当前下载并转 MP4 的整体百分比。
  static Stream<int> get downloadProgressStream {
    return _platform.downloadProgressStream;
  }

  /// 转码任务进度流。
  ///
  /// 进度范围为 `0~100`，表示当前转码任务的整体百分比。
  static Stream<int> get transcodeProgressStream {
    return _platform.transcodeProgressStream;
  }

  /// 音频提取任务进度流。
  ///
  /// 进度范围为 `0~100`，表示当前音频提取任务的整体百分比。
  static Stream<int> get extractAudioProgressStream {
    return _platform.extractAudioProgressStream;
  }

  /// 下载网络视频并输出为本地 MP4 文件。
  ///
  /// [url] 为输入地址，通常用于 HLS/m3u8 视频。
  /// [outputPath] 为空时由原生侧自动生成默认输出路径；
  /// Android 默认位于 App cache 下的 `awesome_video_kit/`。
  /// [overwrite] 为 `true` 时若目标文件已存在会尝试覆盖。
  /// [watermarkImagePath] 不为空时会叠加图片水印。
  /// [watermarkPosition] 指定水印位置。
  ///
  /// 返回值为生成后的本地 MP4 路径。
  static Future<String> downloadVideo({
    required String url,
    String? outputPath,
    bool overwrite = true,
    String? watermarkImagePath,
    VideoWatermarkPosition watermarkPosition =
        VideoWatermarkPosition.bottomRight,
  }) {
    return _platform.downloadVideo(
      url: url,
      outputPath: outputPath,
      overwrite: overwrite,
      watermarkImagePath: watermarkImagePath,
      watermarkPosition: watermarkPosition,
    );
  }

  /// 取消当前正在执行的下载任务。
  static Future<void> cancelDownload() {
    return _platform.cancelDownload();
  }

  /// 查询当前是否存在正在执行的下载任务。
  static Future<bool> isDownloading() {
    return _platform.isDownloading();
  }

  /// 从本地媒体文件中提取首路音频流。
  ///
  /// [inputPath] 为输入媒体文件路径。
  /// [outputPath] 为空时原生侧会根据源音频编码自动推导默认扩展名并生成路径；
  /// Android 默认位于 App cache 下的 `awesome_video_kit/`。
  /// [overwrite] 为 `true` 时若目标文件已存在会尝试覆盖。
  ///
  /// 返回值为提取后的音频文件路径。
  static Future<String> extractAudio({
    required String inputPath,
    String? outputPath,
    bool overwrite = true,
  }) {
    return _platform.extractAudio(
      inputPath: inputPath,
      outputPath: outputPath,
      overwrite: overwrite,
    );
  }

  /// 获取本地视频在指定时间点的画面图片。
  ///
  /// [time] 为目标时间点；[outputPath] 为空时由原生侧自动生成默认 JPEG
  /// 输出路径。[overwrite] 为 `true` 时若目标文件已存在会尝试覆盖。
  /// [outputWidth] 和 [outputHeight] 都大于 0 时指定输出图片分辨率；
  /// 只设置宽或高时会按源视频比例自动补齐另一边，宽高都为 0 时保持源视频帧尺寸。
  /// [fastMode] 为 `true` 时优先速度，可能返回目标时间点附近的帧。
  ///
  /// 返回值为生成后的图片路径。
  static Future<String> captureVideoFrame({
    required String inputPath,
    required Duration time,
    String? outputPath,
    bool overwrite = true,
    int outputWidth = 0,
    int outputHeight = 0,
    bool fastMode = false,
  }) {
    return _platform.captureVideoFrame(
      inputPath: inputPath,
      time: time,
      outputPath: outputPath,
      overwrite: overwrite,
      outputWidth: outputWidth,
      outputHeight: outputHeight,
      fastMode: fastMode,
    );
  }

  /// 同时获取本地视频多个时间点的画面图片。
  ///
  /// [times] 为目标时间点列表；[outputDirectory] 为空时由原生侧自动选择默认
  /// 输出目录。每个时间点会生成一张 JPEG 图片。
  /// [outputWidth] 和 [outputHeight] 都大于 0 时指定输出图片分辨率；
  /// 只设置宽或高时会按源视频比例自动补齐另一边，宽高都为 0 时保持源视频帧尺寸。
  /// [fastMode] 为 `true` 时优先速度，可能返回目标时间点附近的帧。
  ///
  /// 返回值为生成后的图片路径列表。
  static Future<List<String>> captureVideoFrames({
    required String inputPath,
    required List<Duration> times,
    String? outputDirectory,
    bool overwrite = true,
    int outputWidth = 0,
    int outputHeight = 0,
    bool fastMode = false,
  }) {
    return _platform.captureVideoFrames(
      inputPath: inputPath,
      times: times,
      outputDirectory: outputDirectory,
      overwrite: overwrite,
      outputWidth: outputWidth,
      outputHeight: outputHeight,
      fastMode: fastMode,
    );
  }

  /// 取消当前正在执行的音频提取任务。
  static Future<void> cancelExtractAudio() {
    return _platform.cancelExtractAudio();
  }

  /// 查询当前是否存在正在执行的音频提取任务。
  static Future<bool> isExtractingAudio() {
    return _platform.isExtractingAudio();
  }

  /// 将本地视频转码为 MP4。
  ///
  /// [inputPath] 为输入文件路径。
  /// [outputPath] 为空时原生侧会自动生成输出路径；
  /// Android 默认位于 App cache 下的 `awesome_video_kit/`。
  /// [overwrite] 为 `true` 时若目标文件已存在会尝试覆盖。
  /// [options] 用于指定编码参数、分辨率、帧率、水印等配置。
  ///
  /// 返回值为转码后的 MP4 文件路径。
  static Future<String> transcodeVideo({
    required String inputPath,
    String? outputPath,
    bool overwrite = true,
    VideoTranscodeOptions options = const VideoTranscodeOptions(),
  }) {
    return _platform.transcodeVideo(
      inputPath: inputPath,
      outputPath: outputPath,
      overwrite: overwrite,
      options: options,
    );
  }

  /// 取消当前正在执行的转码任务。
  static Future<void> cancelTranscode() {
    return _platform.cancelTranscode();
  }

  /// 查询当前是否存在正在执行的转码任务。
  static Future<bool> isTranscoding() {
    return _platform.isTranscoding();
  }

  /// 暂停当前正在执行的转码任务。
  static Future<void> pauseTranscode() {
    return _platform.pauseTranscode();
  }

  /// 恢复当前已暂停的转码任务。
  static Future<void> resumeTranscode() {
    return _platform.resumeTranscode();
  }

  /// 查询当前转码任务是否处于暂停状态。
  static Future<bool> isTranscodePaused() {
    return _platform.isTranscodePaused();
  }

  /// 将本地视频转码为 MP4，并在头部或尾部拼接一张静态图片。
  ///
  /// [options] 用于指定拼接图片路径、图片展示时长、拼接位置以及可选水印配置。
  ///
  /// 返回值为生成后的 MP4 文件路径。
  static Future<String> transcodeVideoWithConcatImage({
    required String inputPath,
    String? outputPath,
    bool overwrite = true,
    required VideoConcatImageOptions options,
  }) {
    return _platform.transcodeVideoWithConcatImage(
      inputPath: inputPath,
      outputPath: outputPath,
      overwrite: overwrite,
      options: options,
    );
  }

  /// 将本地图片或视频作为画面输入，并叠加独立音频文件输出为 MP4。
  ///
  /// [inputPath] 为画面输入路径，可为视频或静态图片。
  /// [audioPath] 为独立音频文件路径。
  /// [options] 用于指定音频码率、是否 faststart、帧率等配置。
  ///
  /// 返回值为生成后的 MP4 文件路径。
  static Future<String> transcodeMediaWithAudio({
    required String inputPath,
    required String audioPath,
    String? outputPath,
    bool overwrite = true,
    MediaWithAudioOptions options = const MediaWithAudioOptions(),
  }) {
    return _platform.transcodeMediaWithAudio(
      inputPath: inputPath,
      audioPath: audioPath,
      outputPath: outputPath,
      overwrite: overwrite,
      options: options,
    );
  }

  /// 读取本地或网络媒体资源信息。
  ///
  /// 支持音频、视频、图片资源，返回值中包含基础信息以及音视频流级别信息。
  static Future<MediaInfo?> getMediaInfo({required String filePath}) {
    try {
      return _platform.getMediaInfo(filePath: filePath);
    } catch (_) {
      return Future<MediaInfo?>.value(null);
    }
  }

  /// 获取当前下载任务的输出路径。
  ///
  /// 当没有活跃下载任务时返回 `null`。
  static Future<String?> getCurrentDownloadOutputPath() {
    return _platform.getCurrentDownloadOutputPath();
  }

  /// 获取当前转码任务的输出路径。
  ///
  /// 当没有活跃转码任务时返回 `null`。
  static Future<String?> getCurrentTranscodeOutputPath() {
    return _platform.getCurrentTranscodeOutputPath();
  }

  /// 获取当前音频提取任务的输出路径。
  ///
  /// 当没有活跃音频提取任务时返回 `null`。
  static Future<String?> getCurrentExtractAudioOutputPath() {
    return _platform.getCurrentExtractAudioOutputPath();
  }
}
