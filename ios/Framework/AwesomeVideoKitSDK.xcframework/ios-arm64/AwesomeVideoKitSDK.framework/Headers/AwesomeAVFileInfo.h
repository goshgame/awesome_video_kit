//
//  AwesomeAVFileInfo.h
//  AwesomeVideoKitSDK
//
//  Created by dev on 2026/3/23.
//

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

/// 媒体信息读取相关错误的 `NSError.domain`。
FOUNDATION_EXPORT NSErrorDomain const AwesomeAVFileInfoErrorDomain;

/// 媒体信息读取错误码。
typedef NS_ENUM(NSInteger, AwesomeAVFileInfoErrorCode) {
    /// 传入参数不合法，例如空路径。
    AwesomeAVFileInfoErrorInvalidArguments = 1,
    /// FFmpeg 读取文件信息失败。
    AwesomeAVFileInfoErrorLoadFailed = 2,
};

/// 资源文件类型。
typedef NS_ENUM(NSInteger, AwesomeAVFileType) {
    /// 未知类型或尚未成功加载。
    AwesomeAVFileTypeUnknown = 0,
    /// 纯音频文件。
    AwesomeAVFileTypeAudio = 1,
    /// 视频文件。
    AwesomeAVFileTypeVideo = 2,
    /// 静态图片文件。
    AwesomeAVFileTypeImage = 3,
};

/// 像素尺寸。
typedef struct AwesomeSize {
    int width;
    int height;
} AwesomeSize;

NS_INLINE AwesomeSize AwesomeSizeMake(int width, int height) {
    AwesomeSize size;
    size.width = width;
    size.height = height;
    return size;
}

/// 有理数结构，常用于表示帧率、像素宽高比等分数值。
typedef struct AwesomeRational {
    int num;
    int den;
} AwesomeRational;

NS_INLINE AwesomeRational AwesomeRationalMake(int num, int den) {
    AwesomeRational rational;
    rational.num = num;
    rational.den = den;
    return rational;
}

/// 视频旋转角度枚举。
typedef NS_ENUM(NSInteger, AwesomeVideoRotation) {
    /// 旋转角度不是标准的 0/90/180/270。
    AwesomeVideoRotationUnknown = -1,
    AwesomeVideoRotation0 = 0,
    AwesomeVideoRotation90 = 90,
    AwesomeVideoRotation180 = 180,
    AwesomeVideoRotation270 = 270,
};

/// 视频编码类型。
typedef NS_ENUM(NSInteger, AwesomeVideoCodecType) {
    AwesomeVideoCodecTypeUnknown = 0,
    AwesomeVideoCodecTypeH264,
    AwesomeVideoCodecTypeHEVC,
    AwesomeVideoCodecTypeMPEG4,
    AwesomeVideoCodecTypeMPEG2,
    AwesomeVideoCodecTypeH263,
    AwesomeVideoCodecTypeVP8,
    AwesomeVideoCodecTypeVP9,
    AwesomeVideoCodecTypeAV1,
    AwesomeVideoCodecTypeMJPEG,
    AwesomeVideoCodecTypePNG,
    AwesomeVideoCodecTypeProRes,
    AwesomeVideoCodecTypeDNxHD,
    AwesomeVideoCodecTypeVC1,
    AwesomeVideoCodecTypeWMV3,
    AwesomeVideoCodecTypeOther,
};

/// 视频传输特性（Transfer Characteristics）。
typedef NS_ENUM(NSInteger, AwesomeVideoColorTransfer) {
    AwesomeVideoColorTransferUnknown = 0,
    AwesomeVideoColorTransferBT709,
    AwesomeVideoColorTransferGamma22,
    AwesomeVideoColorTransferGamma28,
    AwesomeVideoColorTransferSMPTE170M,
    AwesomeVideoColorTransferSMPTE240M,
    AwesomeVideoColorTransferLinear,
    AwesomeVideoColorTransferLog,
    AwesomeVideoColorTransferLogSqrt,
    AwesomeVideoColorTransferIEC61966_2_4,
    AwesomeVideoColorTransferBT1361ECG,
    AwesomeVideoColorTransferSRGB,
    AwesomeVideoColorTransferBT2020_10,
    AwesomeVideoColorTransferBT2020_12,
    AwesomeVideoColorTransferPQ,
    AwesomeVideoColorTransferHLG,
    AwesomeVideoColorTransferOther,
};

/// 视频 HDR 类型。
typedef NS_ENUM(NSInteger, AwesomeVideoHDRType) {
    AwesomeVideoHDRTypeUnknown = 0,
    AwesomeVideoHDRTypeSDR,
    AwesomeVideoHDRTypeHDR10,
    AwesomeVideoHDRTypeHDR10Plus,
    AwesomeVideoHDRTypeHLG,
    AwesomeVideoHDRTypeDolbyVision,
};

/// 媒体资源信息读取器。
/// 使用 FFmpeg 探测本地或网络音频、视频、图片资源的基础信息以及流级别元数据。
@interface AwesomeAVFileInfo : NSObject

/// 创建实例并立即加载指定本地路径或网络 URL。
+ (nullable instancetype)infoWithFilePath:(NSString *)filePath error:(NSError * _Nullable * _Nullable)error;
/// 创建一个空实例，需后续手动调用 `loadFromFile:error:`。
- (instancetype)init NS_DESIGNATED_INITIALIZER;
/// 创建实例并立即加载指定本地路径或网络 URL。
- (nullable instancetype)initWithFilePath:(NSString *)filePath error:(NSError * _Nullable * _Nullable)error;
/// 加载或重新加载指定本地路径或网络 URL 的媒体信息。
- (BOOL)loadFromFile:(NSString *)filePath error:(NSError * _Nullable * _Nullable)error;

/// 当前文件类型。
@property (nonatomic, readonly) AwesomeAVFileType avFileType;
/// 文件总时长，单位为微秒；图片通常为 0。
@property (nonatomic, readonly) int64_t duration;
/// 码率，单位为 bit/s；未知时为 0。
@property (nonatomic, readonly) uint64_t dataRate;
/// 视频流数量。
@property (nonatomic, readonly) unsigned int videoStreamCount;
/// 音频流数量。
@property (nonatomic, readonly) unsigned int audioStreamCount;
/// 当前已加载文件的标准化路径。
@property (nonatomic, copy, readonly) NSString *sourcePath;

/// 视频流查询接口。`videoStreamIndex` 从 0 开始，越界时返回默认值。
/// 视频流时长，单位为微秒。
- (int64_t)getVideoStreamDuration:(unsigned int)videoStreamIndex;
/// 视频流分辨率，单位为像素。
- (AwesomeSize)getVideoStreamDimension:(unsigned int)videoStreamIndex;
/// 视频流像素宽高比，`num/den` 表示宽高比。
- (AwesomeRational)getVideoStreamPixelAspectRatio:(unsigned int)videoStreamIndex;
/// 视频流帧率，`num/den` 表示每秒帧数。
- (AwesomeRational)getVideoStreamFrameRate:(unsigned int)videoStreamIndex;
/// 视频流旋转枚举值。
- (AwesomeVideoRotation)getVideoStreamRotation:(unsigned int)videoStreamIndex;
/// 视频流旋转角度，单位为度。
- (int)getVideoStreamRotationDegrees:(unsigned int)videoStreamIndex;
/// 视频每个颜色分量的位深，例如 8/10/12。
- (unsigned int)getVideoStreamComponentBitCount:(unsigned int)videoStreamIndex;
/// 视频编码类型枚举值。
- (AwesomeVideoCodecType)getVideoStreamCodecType:(unsigned int)videoStreamIndex;
/// 视频编码名称，例如 `h264`、`hevc`。
- (NSString *)getVideoStreamCodecName:(unsigned int)videoStreamIndex;
/// 视频编码 profile，未知时为 -1。
- (int)getVideoCodecProfile:(unsigned int)videoStreamIndex;
/// 视频编码 level，未知时为 -1。
- (int)getVideoCodecLevel:(unsigned int)videoStreamIndex;
/// 视频传输特性。
- (AwesomeVideoColorTransfer)getVideoStreamColorTransfer:(unsigned int)videoStreamIndex;
/// 兼容旧拼写的别名，等价于 `getVideoStreamColorTransfer:`。
- (AwesomeVideoColorTransfer)getVideoStreamColorTranfer:(unsigned int)videoStreamIndex;
/// 视频 HDR 类型。
- (AwesomeVideoHDRType)getVideoStreamHDRType:(unsigned int)videoStreamIndex;

/// 音频流查询接口。`audioStreamIndex` 从 0 开始，越界时返回默认值。
/// 音频流时长，单位为微秒。
- (int64_t)getAudioStreamDuration:(unsigned int)audioStreamIndex;
/// 音频采样率，单位为 Hz。
- (unsigned int)getAudioStreamSampleRate:(unsigned int)audioStreamIndex;
/// 音频声道数。
- (unsigned int)getAudioStreamChannelCount:(unsigned int)audioStreamIndex;
/// 当前环境是否存在该音频编码的解码器支持。
- (BOOL)getAudioStreamCodecSupport:(unsigned int)audioStreamIndex;
/// 音频编码名称，例如 `aac`、`mp3`。
- (NSString *)getAudioStreamCodecName:(unsigned int)audioStreamIndex;

@end

NS_ASSUME_NONNULL_END
