enum MediaFileType {
  unknown(0),
  audio(1),
  video(2),
  image(3);

  const MediaFileType(this.value);

  final int value;

  static MediaFileType fromValue(int value) {
    return MediaFileType.values.firstWhere(
      (item) => item.value == value,
      orElse: () => MediaFileType.unknown,
    );
  }
}

enum VideoRotation {
  unknown(-1),
  rotation0(0),
  rotation90(90),
  rotation180(180),
  rotation270(270);

  const VideoRotation(this.value);

  final int value;

  static VideoRotation fromValue(int value) {
    return VideoRotation.values.firstWhere(
      (item) => item.value == value,
      orElse: () => VideoRotation.unknown,
    );
  }
}

enum VideoCodecType {
  unknown(0),
  h264(1),
  hevc(2),
  mpeg4(3),
  mpeg2(4),
  h263(5),
  vp8(6),
  vp9(7),
  av1(8),
  mjpeg(9),
  png(10),
  proRes(11),
  dnxhd(12),
  vc1(13),
  wmv3(14),
  other(15);

  const VideoCodecType(this.value);

  final int value;

  static VideoCodecType fromValue(int value) {
    return VideoCodecType.values.firstWhere(
      (item) => item.value == value,
      orElse: () => VideoCodecType.unknown,
    );
  }
}

enum VideoColorTransfer {
  unknown(0),
  bt709(1),
  gamma22(2),
  gamma28(3),
  smpte170M(4),
  smpte240M(5),
  linear(6),
  log(7),
  logSqrt(8),
  iec61966_2_4(9),
  bt1361Ecg(10),
  sRgb(11),
  bt2020_10(12),
  bt2020_12(13),
  pq(14),
  hlg(15),
  other(16);

  const VideoColorTransfer(this.value);

  final int value;

  static VideoColorTransfer fromValue(int value) {
    return VideoColorTransfer.values.firstWhere(
      (item) => item.value == value,
      orElse: () => VideoColorTransfer.unknown,
    );
  }
}

enum VideoHdrType {
  unknown(0),
  sdr(1),
  hdr10(2),
  hdr10Plus(3),
  hlg(4),
  dolbyVision(5);

  const VideoHdrType(this.value);

  final int value;

  static VideoHdrType fromValue(int value) {
    return VideoHdrType.values.firstWhere(
      (item) => item.value == value,
      orElse: () => VideoHdrType.unknown,
    );
  }
}

class MediaSize {
  const MediaSize({required this.width, required this.height});

  final int width;
  final int height;

  factory MediaSize.fromMap(Map<dynamic, dynamic>? map) {
    return MediaSize(
      width: _intValue(map?['width']),
      height: _intValue(map?['height']),
    );
  }
}

class MediaRational {
  const MediaRational({required this.num, required this.den});

  final int num;
  final int den;

  factory MediaRational.fromMap(Map<dynamic, dynamic>? map) {
    return MediaRational(
      num: _intValue(map?['num']),
      den: _intValue(map?['den'], defaultValue: 1),
    );
  }
}

class VideoStreamInfo {
  const VideoStreamInfo({
    required this.duration,
    required this.dimension,
    required this.pixelAspectRatio,
    required this.frameRate,
    required this.rotation,
    required this.rotationDegrees,
    required this.componentBitCount,
    required this.codecType,
    required this.codecName,
    required this.codecProfile,
    required this.codecLevel,
    required this.colorTransfer,
    required this.hdrType,
  });

  final int duration;
  final MediaSize dimension;
  final MediaRational pixelAspectRatio;
  final MediaRational frameRate;
  final VideoRotation rotation;
  final int rotationDegrees;
  final int componentBitCount;
  final VideoCodecType codecType;
  final String codecName;
  final int codecProfile;
  final int codecLevel;
  final VideoColorTransfer colorTransfer;
  final VideoHdrType hdrType;

  factory VideoStreamInfo.fromMap(Map<dynamic, dynamic> map) {
    return VideoStreamInfo(
      duration: _intValue(map['duration']),
      dimension: MediaSize.fromMap(_mapValue(map['dimension'])),
      pixelAspectRatio: MediaRational.fromMap(
        _mapValue(map['pixelAspectRatio']),
      ),
      frameRate: MediaRational.fromMap(_mapValue(map['frameRate'])),
      rotation: VideoRotation.fromValue(
        _intValue(map['rotation'], defaultValue: -1),
      ),
      rotationDegrees: _intValue(map['rotationDegrees']),
      componentBitCount: _intValue(map['componentBitCount']),
      codecType: VideoCodecType.fromValue(_intValue(map['codecType'])),
      codecName: _stringValue(map['codecName']),
      codecProfile: _intValue(map['codecProfile'], defaultValue: -1),
      codecLevel: _intValue(map['codecLevel'], defaultValue: -1),
      colorTransfer: VideoColorTransfer.fromValue(
        _intValue(map['colorTransfer']),
      ),
      hdrType: VideoHdrType.fromValue(_intValue(map['hdrType'])),
    );
  }
}

class AudioStreamInfo {
  const AudioStreamInfo({
    required this.duration,
    required this.sampleRate,
    required this.channelCount,
    required this.codecSupported,
    required this.codecName,
  });

  final int duration;
  final int sampleRate;
  final int channelCount;
  final bool codecSupported;
  final String codecName;

  factory AudioStreamInfo.fromMap(Map<dynamic, dynamic> map) {
    return AudioStreamInfo(
      duration: _intValue(map['duration']),
      sampleRate: _intValue(map['sampleRate']),
      channelCount: _intValue(map['channelCount']),
      codecSupported: _boolValue(map['codecSupported']),
      codecName: _stringValue(map['codecName']),
    );
  }
}

class MediaInfo {
  const MediaInfo({
    required this.fileType,
    required this.duration,
    required this.dataRate,
    required this.videoStreamCount,
    required this.audioStreamCount,
    required this.sourcePath,
    required this.videoStreams,
    required this.audioStreams,
  });

  final MediaFileType fileType;
  final int duration;
  final int dataRate;
  final int videoStreamCount;
  final int audioStreamCount;
  final String sourcePath;
  final List<VideoStreamInfo> videoStreams;
  final List<AudioStreamInfo> audioStreams;

  MediaSize? get primaryVideoDimension =>
      videoStreams.isEmpty ? null : videoStreams.first.dimension;

  factory MediaInfo.fromMap(Map<dynamic, dynamic> map) {
    final videoStreams = _listValue(map['videoStreams']);
    final audioStreams = _listValue(map['audioStreams']);

    return MediaInfo(
      fileType: MediaFileType.fromValue(_intValue(map['fileType'])),
      duration: _intValue(map['duration']),
      dataRate: _intValue(map['dataRate']),
      videoStreamCount: _intValue(map['videoStreamCount']),
      audioStreamCount: _intValue(map['audioStreamCount']),
      sourcePath: _stringValue(map['sourcePath']),
      videoStreams: videoStreams
          .map((item) => VideoStreamInfo.fromMap(_mapValue(item)))
          .toList(growable: false),
      audioStreams: audioStreams
          .map((item) => AudioStreamInfo.fromMap(_mapValue(item)))
          .toList(growable: false),
    );
  }
}

int _intValue(dynamic value, {int defaultValue = 0}) {
  if (value is int) return value;
  if (value is num) return value.toInt();
  if (value is String) return int.tryParse(value) ?? defaultValue;
  return defaultValue;
}

bool _boolValue(dynamic value, {bool defaultValue = false}) {
  if (value is bool) return value;
  if (value is num) return value != 0;
  if (value is String) {
    final normalized = value.trim().toLowerCase();
    if (normalized == 'true' || normalized == '1') return true;
    if (normalized == 'false' || normalized == '0') return false;
  }
  return defaultValue;
}

String _stringValue(dynamic value, {String defaultValue = ''}) {
  if (value == null) return defaultValue;
  final normalized = value.toString().trim();
  return normalized.isEmpty ? defaultValue : normalized;
}

Map<dynamic, dynamic> _mapValue(dynamic value) {
  if (value is Map<dynamic, dynamic>) return value;
  return <dynamic, dynamic>{};
}

List<dynamic> _listValue(dynamic value) {
  if (value is List<dynamic>) return value;
  return const <dynamic>[];
}
