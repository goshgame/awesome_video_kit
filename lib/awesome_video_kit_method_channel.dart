import 'package:flutter/services.dart';
import 'dart:convert';

import 'awesome_video_kit_platform_interface.dart';
import 'src/media_info_models.dart';
import 'src/video_task_models.dart';

/// An implementation of [AwesomeVideoKitPlatform] that uses method channels.
class MethodChannelAwesomeVideoKit extends AwesomeVideoKitPlatform {
  /// The method channel used to interact with the native platform.
  final methodChannel = const MethodChannel('awesome_video_kit');
  final downloadEventChannel = const EventChannel(
    'awesome_video_kit/eventchannel',
  );
  final transcodeEventChannel = const EventChannel(
    'awesome_video_kit/transcode_eventchannel',
  );
  final extractAudioEventChannel = const EventChannel(
    'awesome_video_kit/extract_audio_eventchannel',
  );

  @override
  Future<String?> getPlatformVersion() async {
    final version = await methodChannel.invokeMethod<String>(
      'getPlatformVersion',
    );
    return version;
  }

  @override
  Future<String> downloadVideo({
    required String url,
    String? outputPath,
    bool overwrite = true,
    String? watermarkImagePath,
    VideoWatermarkPosition watermarkPosition =
        VideoWatermarkPosition.bottomRight,
  }) async {
    final path = await methodChannel.invokeMethod<String>('downloadVideo', {
      'url': url,
      if (outputPath != null) 'outputPath': outputPath,
      'overwrite': overwrite,
      if (watermarkImagePath != null && watermarkImagePath.trim().isNotEmpty)
        'watermarkImagePath': watermarkImagePath.trim(),
      'watermarkPosition': watermarkPosition.value,
    });
    if (path == null || path.isEmpty) {
      throw PlatformException(
        code: 'NO_OUTPUT',
        message: 'Native downloadVideo returned empty outputPath.',
      );
    }
    return path;
  }

  @override
  Stream<int> get downloadProgressStream {
    return _mapProgressStream(downloadEventChannel);
  }

  @override
  Stream<int> get transcodeProgressStream {
    return _mapProgressStream(transcodeEventChannel);
  }

  @override
  Stream<int> get extractAudioProgressStream {
    return _mapProgressStream(extractAudioEventChannel);
  }

  @override
  Future<void> cancelDownload() {
    return methodChannel.invokeMethod<void>('cancelDownload');
  }

  @override
  Future<bool> isDownloading() async {
    final value = await methodChannel.invokeMethod<dynamic>('isDownloading');
    if (value is bool) return value;
    if (value is num) return value != 0;
    return value?.toString().toLowerCase() == 'true';
  }

  @override
  Future<String> extractAudio({
    required String inputPath,
    String? outputPath,
    bool overwrite = true,
  }) async {
    final path = await methodChannel.invokeMethod<String>('extractAudio', {
      'inputPath': inputPath,
      if (outputPath != null) 'outputPath': outputPath,
      'overwrite': overwrite,
    });
    if (path == null || path.isEmpty) {
      throw PlatformException(
        code: 'NO_OUTPUT',
        message: 'Native extractAudio returned empty outputPath.',
      );
    }
    return path;
  }

  @override
  Future<String> captureVideoFrame({
    required String inputPath,
    required Duration time,
    String? outputPath,
    bool overwrite = true,
    int outputWidth = 0,
    int outputHeight = 0,
    bool fastMode = false,
  }) async {
    final path = await methodChannel.invokeMethod<String>('captureVideoFrame', {
      'inputPath': inputPath,
      'timeSeconds': time.inMicroseconds / Duration.microsecondsPerSecond,
      if (outputPath != null) 'outputPath': outputPath,
      'overwrite': overwrite,
      'outputWidth': outputWidth,
      'outputHeight': outputHeight,
      'fastMode': fastMode,
    });
    if (path == null || path.isEmpty) {
      throw PlatformException(
        code: 'NO_OUTPUT',
        message: 'Native captureVideoFrame returned empty outputPath.',
      );
    }
    return path;
  }

  @override
  Future<List<String>> captureVideoFrames({
    required String inputPath,
    required List<Duration> times,
    String? outputDirectory,
    bool overwrite = true,
    int outputWidth = 0,
    int outputHeight = 0,
    bool fastMode = false,
  }) async {
    final paths = await methodChannel.invokeMethod<List<dynamic>>(
      'captureVideoFrames',
      {
        'inputPath': inputPath,
        'timesSeconds': times
            .map((time) => time.inMicroseconds / Duration.microsecondsPerSecond)
            .toList(),
        if (outputDirectory != null) 'outputDirectory': outputDirectory,
        'overwrite': overwrite,
        'outputWidth': outputWidth,
        'outputHeight': outputHeight,
        'fastMode': fastMode,
      },
    );
    final outputPaths = _stringListPayload(paths);
    if (outputPaths.isEmpty) {
      throw PlatformException(
        code: 'NO_OUTPUT',
        message: 'Native captureVideoFrames returned empty outputPaths.',
      );
    }
    return outputPaths;
  }

  @override
  Future<void> cancelExtractAudio() {
    return methodChannel.invokeMethod<void>('cancelExtractAudio');
  }

  @override
  Future<bool> isExtractingAudio() async {
    final value = await methodChannel.invokeMethod<dynamic>(
      'isExtractingAudio',
    );
    if (value is bool) return value;
    if (value is num) return value != 0;
    return value?.toString().toLowerCase() == 'true';
  }

  @override
  Future<String> transcodeVideo({
    required String inputPath,
    String? outputPath,
    bool overwrite = true,
    VideoTranscodeOptions options = const VideoTranscodeOptions(),
  }) async {
    final path = await methodChannel.invokeMethod<String>('transcodeVideo', {
      'inputPath': inputPath,
      if (outputPath != null) 'outputPath': outputPath,
      'overwrite': overwrite,
      ...options.toMap(),
    });
    if (path == null || path.isEmpty) {
      throw PlatformException(
        code: 'NO_OUTPUT',
        message: 'Native transcodeVideo returned empty outputPath.',
      );
    }
    return path;
  }

  @override
  Future<void> cancelTranscode() {
    return methodChannel.invokeMethod<void>('cancelTranscode');
  }

  @override
  Future<bool> isTranscoding() async {
    final value = await methodChannel.invokeMethod<dynamic>('isTranscoding');
    if (value is bool) return value;
    if (value is num) return value != 0;
    return value?.toString().toLowerCase() == 'true';
  }

  @override
  Future<void> pauseTranscode() {
    return methodChannel.invokeMethod<void>('pauseTranscode');
  }

  @override
  Future<void> resumeTranscode() {
    return methodChannel.invokeMethod<void>('resumeTranscode');
  }

  @override
  Future<bool> isTranscodePaused() async {
    final value = await methodChannel.invokeMethod<dynamic>(
      'isTranscodePaused',
    );
    if (value is bool) return value;
    if (value is num) return value != 0;
    return value?.toString().toLowerCase() == 'true';
  }

  @override
  Future<String> transcodeVideoWithConcatImage({
    required String inputPath,
    String? outputPath,
    bool overwrite = true,
    required VideoConcatImageOptions options,
  }) async {
    final path = await methodChannel.invokeMethod<String>(
      'transcodeVideoWithConcatImage',
      <String, dynamic>{
        'inputPath': inputPath,
        if (outputPath != null) 'outputPath': outputPath,
        'overwrite': overwrite,
        ...options.toMap(),
      },
    );
    if (path == null || path.isEmpty) {
      throw PlatformException(
        code: 'NO_OUTPUT',
        message:
            'Native transcodeVideoWithConcatImage returned empty outputPath.',
      );
    }
    return path;
  }

  @override
  Future<String> transcodeMediaWithAudio({
    required String inputPath,
    required String audioPath,
    String? outputPath,
    bool overwrite = true,
    MediaWithAudioOptions options = const MediaWithAudioOptions(),
  }) async {
    final path = await methodChannel
        .invokeMethod<String>('transcodeMediaWithAudio', <String, dynamic>{
          'inputPath': inputPath,
          'audioPath': audioPath,
          if (outputPath != null) 'outputPath': outputPath,
          'overwrite': overwrite,
          ...options.toMap(),
        });
    if (path == null || path.isEmpty) {
      throw PlatformException(
        code: 'NO_OUTPUT',
        message: 'Native transcodeMediaWithAudio returned empty outputPath.',
      );
    }
    return path;
  }

  @override
  Future<MediaInfo> getMediaInfo({required String filePath}) async {
    final payload = await methodChannel.invokeMethod<dynamic>('getMediaInfo', {
      'filePath': filePath,
    });
    final map = _mapPayload(payload);
    return MediaInfo.fromMap(map);
  }

  @override
  Future<String?> getCurrentDownloadOutputPath() {
    return methodChannel.invokeMethod<String?>('getCurrentDownloadOutputPath');
  }

  @override
  Future<String?> getCurrentTranscodeOutputPath() {
    return methodChannel.invokeMethod<String?>('getCurrentTranscodeOutputPath');
  }

  @override
  Future<String?> getCurrentExtractAudioOutputPath() {
    return methodChannel.invokeMethod<String?>(
      'getCurrentExtractAudioOutputPath',
    );
  }

  Stream<int> _mapProgressStream(EventChannel channel) {
    return channel.receiveBroadcastStream().map((event) {
      if (event is int) return event;
      if (event is num) return event.toInt();
      return int.tryParse(event.toString()) ?? 0;
    });
  }

  Map<dynamic, dynamic> _mapPayload(dynamic payload) {
    if (payload is Map<dynamic, dynamic>) return payload;
    if (payload is String && payload.trim().isNotEmpty) {
      final decoded = jsonDecode(payload);
      if (decoded is Map<dynamic, dynamic>) return decoded;
    }
    throw PlatformException(
      code: 'INVALID_PAYLOAD',
      message: 'Native getMediaInfo returned an unsupported payload.',
    );
  }

  List<String> _stringListPayload(dynamic payload) {
    if (payload is List) {
      return payload
          .map((value) => value?.toString() ?? '')
          .where((value) => value.isNotEmpty)
          .toList();
    }
    return const <String>[];
  }
}
