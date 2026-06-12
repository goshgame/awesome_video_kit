//
//  AwesomeVideoFrameCaptureManager.h
//  AwesomeVideoKitSDK
//
//  Created by Codex on 2026/6/2.
//

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

FOUNDATION_EXPORT NSErrorDomain const AwesomeVideoFrameCaptureErrorDomain;

/// 视频帧截图错误码。
typedef NS_ENUM(NSInteger, AwesomeVideoFrameCaptureErrorCode) {
    /// 参数无效，例如路径为空、时间点为空、时间点为负数或输出尺寸为负数。
    AwesomeVideoFrameCaptureErrorInvalidArguments = 1,
    /// 当前已有截图任务在执行。同一时间只允许一个截图任务。
    AwesomeVideoFrameCaptureErrorBusy = 2,
    /// 输出文件已存在且 overwrite 为 NO。
    AwesomeVideoFrameCaptureErrorFileExists = 3,
    /// 创建输出目录失败。
    AwesomeVideoFrameCaptureErrorCreateDirectoryFailed = 4,
    /// FFmpeg 解码、缩放或 JPEG 编码失败。
    AwesomeVideoFrameCaptureErrorFFmpegFailed = 5,
    /// 当前任务已取消。
    AwesomeVideoFrameCaptureErrorCancelled = 6,
};

/// 单张截图完成回调。成功时 outputPath 为生成的 JPEG 路径，失败时 error 非空。
typedef void (^AwesomeVideoFrameCaptureCompletionBlock)(NSString *_Nullable outputPath, NSError *_Nullable error);

/// 多张截图完成回调。成功时 outputPaths 按传入时间点顺序返回，失败时 error 非空。
typedef void (^AwesomeVideoFramesCaptureCompletionBlock)(NSArray<NSString *> *_Nullable outputPaths, NSError *_Nullable error);

@interface AwesomeVideoFrameCaptureManager : NSObject

/// 全局单例。
+ (instancetype)sharedInstance;

/// 是否正在执行视频截图任务。
@property (atomic, readonly) BOOL isCapturingVideoFrames;

/// 当前任务的第一个输出路径；没有任务时为 nil。
@property (atomic, readonly, nullable) NSString *currentOutputPath;

/// 截取单个时间点的原始分辨率 JPEG。
///
/// 这是便利方法，等价于 outputWidth=0、outputHeight=0、fastMode=NO。
/// @param inputPath 输入视频本地路径或 HTTP/HTTPS URL。
/// @param timeSeconds 截图时间点，单位秒，必须大于等于 0。
/// @param outputPath 输出 JPEG 路径；传 nil 时自动生成到临时目录。
/// @param overwrite 输出文件存在时是否覆盖。
/// @param completion 主线程回调。
- (void)captureVideoFrameAtPath:(NSString *)inputPath
                    timeSeconds:(NSTimeInterval)timeSeconds
                     outputPath:(nullable NSString *)outputPath
                      overwrite:(BOOL)overwrite
                     completion:(nullable AwesomeVideoFrameCaptureCompletionBlock)completion;

/// 截取单个时间点的 JPEG，可指定输出分辨率。
///
/// 这是便利方法，等价于 fastMode=NO。
/// @param outputWidth 输出宽度；传 0 且 outputHeight > 0 时按视频比例自动计算。
/// @param outputHeight 输出高度；传 0 且 outputWidth > 0 时按视频比例自动计算。宽高都为 0 时使用视频原始尺寸。
- (void)captureVideoFrameAtPath:(NSString *)inputPath
                    timeSeconds:(NSTimeInterval)timeSeconds
                     outputPath:(nullable NSString *)outputPath
                      overwrite:(BOOL)overwrite
                    outputWidth:(NSInteger)outputWidth
                   outputHeight:(NSInteger)outputHeight
                     completion:(nullable AwesomeVideoFrameCaptureCompletionBlock)completion;

/// 截取单个时间点的 JPEG，支持指定分辨率和快速模式。
///
/// 完整单图接口。outputWidth/outputHeight 只传一个时按视频比例补齐，宽高都为 0 时使用原始尺寸；fastMode 为 YES 时优先速度，
/// 可能返回目标时间点附近的帧，fastMode 为 NO 时更偏向时间点准确性。
- (void)captureVideoFrameAtPath:(NSString *)inputPath
                    timeSeconds:(NSTimeInterval)timeSeconds
                     outputPath:(nullable NSString *)outputPath
                      overwrite:(BOOL)overwrite
                    outputWidth:(NSInteger)outputWidth
                   outputHeight:(NSInteger)outputHeight
                       fastMode:(BOOL)fastMode
                     completion:(nullable AwesomeVideoFrameCaptureCompletionBlock)completion;

/// 截取多个时间点的原始分辨率 JPEG。
///
/// 这是便利方法，等价于 outputWidth=0、outputHeight=0、fastMode=NO。
/// @param inputPath 输入视频本地路径或 HTTP/HTTPS URL。
/// @param timesSeconds 截图时间点数组，单位秒，元素必须大于等于 0。
/// @param outputDirectory 输出目录；传 nil 时自动生成到临时目录。
/// @param overwrite 输出文件存在时是否覆盖。
/// @param completion 主线程回调。
- (void)captureVideoFramesAtPath:(NSString *)inputPath
                     timeSeconds:(NSArray<NSNumber *> *)timesSeconds
                 outputDirectory:(nullable NSString *)outputDirectory
                       overwrite:(BOOL)overwrite
                      completion:(nullable AwesomeVideoFramesCaptureCompletionBlock)completion;

/// 截取多个时间点的 JPEG，可指定输出分辨率。
///
/// 这是便利方法，等价于 fastMode=NO。
/// @param outputWidth 输出宽度；传 0 且 outputHeight > 0 时按视频比例自动计算。
/// @param outputHeight 输出高度；传 0 且 outputWidth > 0 时按视频比例自动计算。宽高都为 0 时使用视频原始尺寸。
- (void)captureVideoFramesAtPath:(NSString *)inputPath
                     timeSeconds:(NSArray<NSNumber *> *)timesSeconds
                 outputDirectory:(nullable NSString *)outputDirectory
                       overwrite:(BOOL)overwrite
                     outputWidth:(NSInteger)outputWidth
                    outputHeight:(NSInteger)outputHeight
                      completion:(nullable AwesomeVideoFramesCaptureCompletionBlock)completion;

/// 截取多个时间点的 JPEG，支持指定分辨率和快速模式。
///
/// 完整多图接口。outputWidth/outputHeight 只传一个时按视频比例补齐，宽高都为 0 时使用原始尺寸；fastMode 为 YES 时优先速度，
/// 可能返回目标时间点附近的帧，fastMode 为 NO 时更偏向时间点准确性。
- (void)captureVideoFramesAtPath:(NSString *)inputPath
                     timeSeconds:(NSArray<NSNumber *> *)timesSeconds
                 outputDirectory:(nullable NSString *)outputDirectory
                       overwrite:(BOOL)overwrite
                     outputWidth:(NSInteger)outputWidth
                    outputHeight:(NSInteger)outputHeight
                        fastMode:(BOOL)fastMode
                      completion:(nullable AwesomeVideoFramesCaptureCompletionBlock)completion;

/// 取消当前截图任务。取消后 completion 会返回 AwesomeVideoFrameCaptureErrorCancelled。
- (void)cancelCurrentTask;

@end

NS_ASSUME_NONNULL_END
