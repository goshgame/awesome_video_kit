/// 视频水印位置。
enum VideoWatermarkPosition {
  topLeft(0),
  topRight(1),
  bottomLeft(2),
  bottomRight(3),
  center(4),
  alternatingTopLeftBottomRight(5);

  const VideoWatermarkPosition(this.value);

  final int value;
}

/// 静态图片拼接位置。
enum VideoConcatImagePosition {
  head(0),
  tail(1);

  const VideoConcatImagePosition(this.value);

  final int value;
}

/// 本地视频转 MP4 的转码参数。
class VideoTranscodeOptions {
  const VideoTranscodeOptions({
    this.crf = 0,
    this.preset,
    this.profile,
    this.level,
    this.videoWidth = 0,
    this.videoHeight = 0,
    this.audioBitrate = 0,
    this.faststart = true,
    this.frameRate = 0,
    this.watermarkImagePath,
    this.watermarkPosition = VideoWatermarkPosition.bottomRight,
  });

  /// 视频质量控制参数，通常值越小质量越高。
  final int crf;

  /// FFmpeg 编码预设，例如 `veryfast`、`medium`。
  final String? preset;

  /// H.264/H.265 等编码 profile。
  final String? profile;

  /// 编码 level。
  final String? level;

  /// 输出视频宽度，`0` 表示保持原始宽度。
  final int videoWidth;

  /// 输出视频高度，`0` 表示保持原始高度。
  final int videoHeight;

  /// 输出音频码率，单位通常为 bps，`0` 表示使用默认值。
  final int audioBitrate;

  /// 是否启用 `faststart`，便于 MP4 更快开始播放。
  final bool faststart;

  /// 输出帧率，`0` 表示保持原始帧率。
  final int frameRate;

  /// 可选图片水印路径。
  final String? watermarkImagePath;

  /// 水印显示位置。
  final VideoWatermarkPosition watermarkPosition;

  Map<String, dynamic> toMap() {
    String? normalized(String? value) {
      final trimmed = value?.trim();
      if (trimmed == null || trimmed.isEmpty) return null;
      return trimmed;
    }

    return <String, dynamic>{
      'crf': crf,
      'videoWidth': videoWidth,
      'videoHeight': videoHeight,
      'audioBitrate': audioBitrate,
      'faststart': faststart,
      'frameRate': frameRate,
      'watermarkPosition': watermarkPosition.value,
      if (normalized(preset) != null) 'preset': normalized(preset),
      if (normalized(profile) != null) 'profile': normalized(profile),
      if (normalized(level) != null) 'level': normalized(level),
      if (normalized(watermarkImagePath) != null)
        'watermarkImagePath': normalized(watermarkImagePath),
    };
  }
}

/// 本地视频转 MP4，并在头部或尾部拼接一张静态图片的参数。
///
/// 同时支持额外叠加图片水印。
class VideoConcatImageOptions {
  const VideoConcatImageOptions({
    required this.concatImagePath,
    required this.imageDuration,
    this.concatPosition = VideoConcatImagePosition.tail,
    this.watermarkImagePath,
    this.watermarkPosition = VideoWatermarkPosition.bottomRight,
  });

  /// 需要拼接的静态图片路径。
  final String concatImagePath;

  /// 静态图片在输出视频中展示的时长。
  final Duration imageDuration;

  /// 图片拼接到视频头部还是尾部。
  final VideoConcatImagePosition concatPosition;

  /// 可选图片水印路径；为空时表示不添加水印。
  final String? watermarkImagePath;

  /// 水印显示位置。
  final VideoWatermarkPosition watermarkPosition;

  Map<String, dynamic> toMap() {
    final normalizedPath = concatImagePath.trim();
    final normalizedWatermarkPath = watermarkImagePath?.trim();
    return <String, dynamic>{
      'concatImagePath': normalizedPath,
      'imageDurationMs': imageDuration.inMilliseconds,
      'concatPosition': concatPosition.value,
      'watermarkPosition': watermarkPosition.value,
      if (normalizedWatermarkPath != null && normalizedWatermarkPath.isNotEmpty)
        'watermarkImagePath': normalizedWatermarkPath,
    };
  }
}

/// 将图片或视频与独立音频合成为 MP4 的参数。
class MediaWithAudioOptions {
  const MediaWithAudioOptions({
    this.audioBitrate = 0,
    this.faststart = true,
    this.frameRate = 0,
  });

  /// 输出音频码率，单位通常为 bps，`0` 表示使用默认值。
  final int audioBitrate;

  /// 是否启用 `faststart`。
  final bool faststart;

  /// 输出帧率，`0` 表示使用默认值。
  final int frameRate;

  Map<String, dynamic> toMap() {
    return <String, dynamic>{
      'audioBitrate': audioBitrate,
      'faststart': faststart,
      'frameRate': frameRate,
    };
  }
}
