//
//  AwesomeVideoTranscoderManager.h
//  AwesomeVideoKitSDK
//
//  Created by 崔志伟 on 2026/3/20.
//

#import <Foundation/Foundation.h>
#import <AwesomeVideoKitSDK/AwesomeVideoDownloadManager.h>

NS_ASSUME_NONNULL_BEGIN

FOUNDATION_EXPORT NSErrorDomain const VideoTranscoderErrorDomain;

// 转码对外错误码定义。
typedef NS_ENUM(NSInteger, VideoTranscoderErrorCode) {
    VideoTranscoderErrorInvalidArguments = 1,
    VideoTranscoderErrorBusy = 2,
    VideoTranscoderErrorFileExists = 3,
    VideoTranscoderErrorCreateDirectoryFailed = 4,
    VideoTranscoderErrorFFmpegFailed = 5,
    VideoTranscoderErrorCancelled = 6,
};

// 图片拼接位置。
typedef NS_ENUM(NSInteger, VideoConcatImagePosition) {
    VideoConcatImagePositionHead = 0,
    VideoConcatImagePositionTail = 1,
};

// progress 取值范围为 0~100。
typedef void (^VideoTranscoderProgressBlock)(NSInteger progress);
typedef void (^VideoTranscoderCompletionBlock)(NSString *_Nullable outputPath, NSError *_Nullable error);

// 本地视频转码配置；options 或单个字段传 nil 时会使用默认值。
@interface AwesomeVideoTranscodeOptions : NSObject

// nil 时默认 NO。
@property (nonatomic, strong, nullable) NSNumber *overwrite;
// nil 时默认 23。
@property (nonatomic, strong, nullable) NSNumber *crf;
// nil 时使用 FFmpeg 默认 preset。
@property (nonatomic, copy, nullable) NSString *preset;
// nil 时使用 FFmpeg 默认 profile。
@property (nonatomic, copy, nullable) NSString *profile;
// nil 时使用 FFmpeg 默认 level。
@property (nonatomic, copy, nullable) NSString *level;
// nil 时默认 720；传 0 表示不指定宽度。
@property (nonatomic, strong, nullable) NSNumber *videoWidth;
// nil 时默认 0，表示按比例自动推导。
@property (nonatomic, strong, nullable) NSNumber *videoHeight;
// nil 时默认 128000。
@property (nonatomic, strong, nullable) NSNumber *audioBitrate;
// nil 时默认 YES。
@property (nonatomic, strong, nullable) NSNumber *faststart;
// nil 时默认 0，表示保持源视频帧率。
@property (nonatomic, strong, nullable) NSNumber *frameRate;
// nil 或空字符串时不启用水印。
@property (nonatomic, copy, nullable) NSString *watermarkImagePath;
// 使用 VideoWatermarkPosition 的 rawValue；nil 时默认 BottomRight。
@property (nonatomic, strong, nullable) NSNumber *watermarkPosition;

@end

// 图片拼接转码配置；options 或单个字段传 nil 时会使用默认值。
@interface AwesomeVideoConcatOptions : NSObject

// nil 时默认 NO。
@property (nonatomic, strong, nullable) NSNumber *overwrite;
// nil 时默认 5 秒。
@property (nonatomic, strong, nullable) NSNumber *imageDuration;
// 使用 VideoConcatImagePosition 的 rawValue；nil 时默认 Tail。
@property (nonatomic, strong, nullable) NSNumber *concatPosition;
// nil 或空字符串时不启用水印。
@property (nonatomic, copy, nullable) NSString *watermarkImagePath;
// 使用 VideoWatermarkPosition 的 rawValue；nil 时默认 BottomRight。
@property (nonatomic, strong, nullable) NSNumber *watermarkPosition;

@end

// 外部音频合成配置；options 或单个字段传 nil 时会使用默认值。
@interface AwesomeVideoSeparateAudioOptions : NSObject

// nil 时默认 NO。
@property (nonatomic, strong, nullable) NSNumber *overwrite;
// nil 时默认 128000。
@property (nonatomic, strong, nullable) NSNumber *audioBitrate;
// nil 时默认 YES。
@property (nonatomic, strong, nullable) NSNumber *faststart;
// nil 时默认 0，表示保持源视频帧率。
@property (nonatomic, strong, nullable) NSNumber *frameRate;

@end

@interface AwesomeVideoTranscoderManager : NSObject
// 单例入口，内部串行执行转码任务。
+ (instancetype)sharedInstance;

// outputPath 为空时，生成视频会落到 AwesomeVideoFileManager 管理目录。
@property (atomic, readonly) BOOL isTranscoding;
// 当前任务是否处于暂停状态。
@property (atomic, readonly) BOOL isPaused;
// 当前任务输出路径，空表示暂无活跃任务。
@property (atomic, readonly, nullable) NSString *currentOutputPath;

// 本地视频转 MP4，可选水印和编码参数。
- (void)transcodeVideoAtPath:(NSString *)inputPath
                  outputPath:(nullable NSString *)outputPath
                     options:(nullable AwesomeVideoTranscodeOptions *)options
                    progress:(nullable VideoTranscoderProgressBlock)progress
                  completion:(nullable VideoTranscoderCompletionBlock)completion;

// 本地视频转 MP4，并在头部或尾部拼接一张静态图片；可选额外配置水印。
- (void)transcodeVideoAtPath:(NSString *)inputPath
                  outputPath:(nullable NSString *)outputPath
             concatImagePath:(NSString *)concatImagePath
                     options:(nullable AwesomeVideoConcatOptions *)options
                    progress:(nullable VideoTranscoderProgressBlock)progress
                  completion:(nullable VideoTranscoderCompletionBlock)completion;

// 将本地图片或本地视频作为画面输入，并叠加独立音频文件输出为 MP4。
- (void)transcodeMediaAtPath:(NSString *)inputPath
                   audioPath:(NSString *)audioPath
                  outputPath:(nullable NSString *)outputPath
                     options:(nullable AwesomeVideoSeparateAudioOptions *)options
                    progress:(nullable VideoTranscoderProgressBlock)progress
                  completion:(nullable VideoTranscoderCompletionBlock)completion;

// 取消当前正在执行的转码任务。
- (void)cancelCurrentTask;
// 暂停当前正在执行的转码任务。
- (void)pauseCurrentTask;
// 恢复当前已暂停的转码任务。
- (void)resumeCurrentTask;

@end

NS_ASSUME_NONNULL_END
