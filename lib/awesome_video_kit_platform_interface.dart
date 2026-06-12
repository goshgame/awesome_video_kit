import 'package:plugin_platform_interface/plugin_platform_interface.dart';

import 'awesome_video_kit_method_channel.dart';
import 'src/media_info_models.dart';
import 'src/video_task_models.dart';

abstract class AwesomeVideoKitPlatform extends PlatformInterface {
  /// Constructs a AwesomeVideoKitPlatform.
  AwesomeVideoKitPlatform() : super(token: _token);

  static final Object _token = Object();

  static AwesomeVideoKitPlatform _instance = MethodChannelAwesomeVideoKit();

  /// The default instance of [AwesomeVideoKitPlatform] to use.
  ///
  /// Defaults to [MethodChannelAwesomeVideoKit].
  static AwesomeVideoKitPlatform get instance => _instance;

  /// Platform-specific implementations should set this with their own
  /// platform-specific class that extends [AwesomeVideoKitPlatform] when
  /// they register themselves.
  static set instance(AwesomeVideoKitPlatform instance) {
    PlatformInterface.verifyToken(instance, _token);
    _instance = instance;
  }

  Future<String?> getPlatformVersion() {
    throw UnimplementedError('platformVersion() has not been implemented.');
  }

  Stream<int> get downloadProgressStream {
    throw UnimplementedError(
      'downloadProgressStream has not been implemented.',
    );
  }

  Stream<int> get transcodeProgressStream {
    throw UnimplementedError(
      'transcodeProgressStream has not been implemented.',
    );
  }

  Stream<int> get extractAudioProgressStream {
    throw UnimplementedError(
      'extractAudioProgressStream has not been implemented.',
    );
  }

  Future<String> downloadVideo({
    required String url,
    String? outputPath,
    bool overwrite = true,
    String? watermarkImagePath,
    VideoWatermarkPosition watermarkPosition =
        VideoWatermarkPosition.bottomRight,
  }) {
    throw UnimplementedError('downloadVideo() has not been implemented.');
  }

  Future<void> cancelDownload() {
    throw UnimplementedError('cancelDownload() has not been implemented.');
  }

  @Deprecated('Use cancelDownload() instead.')
  Future<void> canceDownload() {
    return cancelDownload();
  }

  Future<bool> isDownloading() {
    throw UnimplementedError('isVideoDownloading() has not been implemented.');
  }

  Future<String> extractAudio({
    required String inputPath,
    String? outputPath,
    bool overwrite = true,
  }) {
    throw UnimplementedError('extractAudio() has not been implemented.');
  }

  Future<String> captureVideoFrame({
    required String inputPath,
    required Duration time,
    String? outputPath,
    bool overwrite = true,
    int outputWidth = 0,
    int outputHeight = 0,
    bool fastMode = false,
  }) {
    throw UnimplementedError('captureVideoFrame() has not been implemented.');
  }

  Future<List<String>> captureVideoFrames({
    required String inputPath,
    required List<Duration> times,
    String? outputDirectory,
    bool overwrite = true,
    int outputWidth = 0,
    int outputHeight = 0,
    bool fastMode = false,
  }) {
    throw UnimplementedError('captureVideoFrames() has not been implemented.');
  }

  Future<void> cancelExtractAudio() {
    throw UnimplementedError('cancelExtractAudio() has not been implemented.');
  }

  Future<bool> isExtractingAudio() {
    throw UnimplementedError('isExtractingAudio() has not been implemented.');
  }

  Future<String> transcodeVideo({
    required String inputPath,
    String? outputPath,
    bool overwrite = true,
    VideoTranscodeOptions options = const VideoTranscodeOptions(),
  }) {
    throw UnimplementedError('transcodeVideo() has not been implemented.');
  }

  Future<void> cancelTranscode() {
    throw UnimplementedError('cancelTranscode() has not been implemented.');
  }

  Future<bool> isTranscoding() {
    throw UnimplementedError('isTranscoding() has not been implemented.');
  }

  Future<void> pauseTranscode() {
    throw UnimplementedError('pauseTranscode() has not been implemented.');
  }

  Future<void> resumeTranscode() {
    throw UnimplementedError('resumeTranscode() has not been implemented.');
  }

  Future<bool> isTranscodePaused() {
    throw UnimplementedError('isTranscodePaused() has not been implemented.');
  }

  Future<String> transcodeVideoWithConcatImage({
    required String inputPath,
    String? outputPath,
    bool overwrite = true,
    required VideoConcatImageOptions options,
  }) {
    throw UnimplementedError(
      'transcodeVideoWithConcatImage() has not been implemented.',
    );
  }

  Future<String> transcodeMediaWithAudio({
    required String inputPath,
    required String audioPath,
    String? outputPath,
    bool overwrite = true,
    MediaWithAudioOptions options = const MediaWithAudioOptions(),
  }) {
    throw UnimplementedError(
      'transcodeMediaWithAudio() has not been implemented.',
    );
  }

  Future<MediaInfo> getMediaInfo({required String filePath}) {
    throw UnimplementedError('getMediaInfo() has not been implemented.');
  }

  Future<String?> getCurrentDownloadOutputPath() {
    throw UnimplementedError(
      'getCurrentDownloadOutputPath() has not been implemented.',
    );
  }

  Future<String?> getCurrentTranscodeOutputPath() {
    throw UnimplementedError(
      'getCurrentTranscodeOutputPath() has not been implemented.',
    );
  }

  Future<String?> getCurrentExtractAudioOutputPath() {
    throw UnimplementedError(
      'getCurrentExtractAudioOutputPath() has not been implemented.',
    );
  }
}
