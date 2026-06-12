//
//  AwesomeVideoTranscoderManager.m
//  AwesomeVideoKitSDK
//
//  Created by 崔志伟 on 2026/3/20.
//

#import "AwesomeVideoTranscoderManager.h"
#import "AwesomeVideoFileManager.h"
#import "ffmpeg_mp4.h"
#import "ffmpeg_mp4_concat_image.h"
#import "ffmpeg_mp4_watermark.h"
#if __has_include(<UIKit/UIKit.h>)
#import <UIKit/UIKit.h>
#endif

NSErrorDomain const VideoTranscoderErrorDomain = @"video_transcoder";

typedef NS_OPTIONS(int, VideoTranscoderPauseReason) {
    VideoTranscoderPauseReasonManual = 1 << 0,
    VideoTranscoderPauseReasonLifecycle = 1 << 1,
};

@interface _VideoTranscoderProgressContext : NSObject
@property (nonatomic, copy, nullable) VideoTranscoderProgressBlock progress;
@property (nonatomic, assign) volatile int *cancelFlag;
@end

@implementation _VideoTranscoderProgressContext
@end

@implementation AwesomeVideoTranscodeOptions
@end

@implementation AwesomeVideoConcatOptions
@end

@implementation AwesomeVideoSeparateAudioOptions
@end

static const NSInteger kVideoTranscoderDefaultCRF = 23;
static const NSInteger kVideoTranscoderDefaultVideoWidth = 720;
static const NSInteger kVideoTranscoderDefaultVideoHeight = 0;
static const NSInteger kVideoTranscoderDefaultAudioBitrate = 128000;
static const NSInteger kVideoTranscoderDefaultFrameRate = 0;
static const BOOL kVideoTranscoderDefaultOverwrite = NO;
static const BOOL kVideoTranscoderDefaultFaststart = YES;
static const NSTimeInterval kVideoTranscoderDefaultConcatImageDuration = 5.0;

static NSInteger video_transcoder_integer_or_default(NSNumber *_Nullable value, NSInteger defaultValue) {
    return value ? value.integerValue : defaultValue;
}

static BOOL video_transcoder_bool_or_default(NSNumber *_Nullable value, BOOL defaultValue) {
    return value ? value.boolValue : defaultValue;
}

static NSTimeInterval video_transcoder_time_interval_or_default(NSNumber *_Nullable value, NSTimeInterval defaultValue) {
    return value ? value.doubleValue : defaultValue;
}

static VideoWatermarkPosition video_transcoder_watermark_position_or_default(NSNumber *_Nullable value) {
    switch (value.integerValue) {
        case VideoWatermarkPositionTopLeft:
            return VideoWatermarkPositionTopLeft;
        case VideoWatermarkPositionTopRight:
            return VideoWatermarkPositionTopRight;
        case VideoWatermarkPositionBottomLeft:
            return VideoWatermarkPositionBottomLeft;
        case VideoWatermarkPositionCenter:
            return VideoWatermarkPositionCenter;
        case VideoWatermarkPositionAlternatingTopLeftBottomRight:
            return VideoWatermarkPositionAlternatingTopLeftBottomRight;
        case VideoWatermarkPositionBottomRight:
        default:
            return VideoWatermarkPositionBottomRight;
    }
}

static VideoConcatImagePosition video_transcoder_concat_position_or_default(NSNumber *_Nullable value) {
    switch (value.integerValue) {
        case VideoConcatImagePositionHead:
            return VideoConcatImagePositionHead;
        case VideoConcatImagePositionTail:
        default:
            return VideoConcatImagePositionTail;
    }
}

static void video_transcoder_progress_cb(int percentage, void *user_data) {
    _VideoTranscoderProgressContext *ctx = (__bridge _VideoTranscoderProgressContext *)user_data;
    if (!ctx.progress) return;
    if (ctx.cancelFlag && *(ctx.cancelFlag)) return;

    NSInteger clamped = (NSInteger)percentage;
    if (clamped < 0) clamped = 0;
    if (clamped > 100) clamped = 100;

    dispatch_async(dispatch_get_main_queue(), ^{
        ctx.progress(clamped);
    });
}

static FFmpegWatermarkPosition video_transcoder_to_c_watermark_position(VideoWatermarkPosition position) {
    switch (position) {
        case VideoWatermarkPositionTopLeft:
            return FFmpegWatermarkPositionTopLeft;
        case VideoWatermarkPositionTopRight:
            return FFmpegWatermarkPositionTopRight;
        case VideoWatermarkPositionBottomLeft:
            return FFmpegWatermarkPositionBottomLeft;
        case VideoWatermarkPositionCenter:
            return FFmpegWatermarkPositionCenter;
        case VideoWatermarkPositionAlternatingTopLeftBottomRight:
            return FFmpegWatermarkPositionAlternatingTopLeftBottomRight;
        case VideoWatermarkPositionBottomRight:
        default:
            return FFmpegWatermarkPositionBottomRight;
    }
}

static FFmpegConcatImagePosition video_transcoder_to_c_concat_position(VideoConcatImagePosition position) {
    switch (position) {
        case VideoConcatImagePositionHead:
            return FFmpegConcatImagePositionHead;
        case VideoConcatImagePositionTail:
        default:
            return FFmpegConcatImagePositionTail;
    }
}

static NSError *video_transcoder_error(VideoTranscoderErrorCode code, NSString *message) {
    return [NSError errorWithDomain:VideoTranscoderErrorDomain
                               code:code
                           userInfo:@{NSLocalizedDescriptionKey : message ?: @"Unknown error"}];
}

static NSString *video_transcoder_random_file_stem(void) {
    return [NSString stringWithFormat:@"video_%@", [NSUUID UUID].UUIDString];
}

static NSError *video_transcoder_wrap_managed_video_error(NSError *_Nullable error) {
    NSMutableDictionary *userInfo = [@{
        NSLocalizedDescriptionKey : @"Failed to create output directory.",
    } mutableCopy];
    if (error) userInfo[NSUnderlyingErrorKey] = error;
    return [NSError errorWithDomain:VideoTranscoderErrorDomain
                               code:VideoTranscoderErrorCreateDirectoryFailed
                           userInfo:userInfo];
}

static NSString *video_transcoder_ensure_mp4_extension(NSString *path) {
    if (path.length == 0) return path;
    if ([[path.pathExtension lowercaseString] isEqualToString:@"mp4"]) return path;
    return [path stringByAppendingPathExtension:@"mp4"];
}

static NSString *video_transcoder_resolve_input_path(NSString *inputPath, NSError **errorOut) {
    NSString *path = [[[inputPath ?: @"" stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet]
        stringByExpandingTildeInPath] copy];
    if (path.length == 0) {
        if (errorOut) *errorOut = video_transcoder_error(VideoTranscoderErrorInvalidArguments, @"inputPath is required.");
        return @"";
    }

    NSFileManager *fm = NSFileManager.defaultManager;
    BOOL isDirectory = NO;
    if (![fm fileExistsAtPath:path isDirectory:&isDirectory] || isDirectory) {
        if (errorOut) *errorOut = video_transcoder_error(VideoTranscoderErrorInvalidArguments, @"inputPath must be an existing file.");
        return @"";
    }

    return path;
}

static NSString *video_transcoder_resolve_required_file_path(
    NSString *_Nullable rawPath,
    NSString *argumentName,
    NSError **errorOut
) {
    NSString *path = [[[rawPath ?: @"" stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet]
        stringByExpandingTildeInPath] copy];
    if (path.length == 0) {
        if (errorOut) {
            NSString *message = [NSString stringWithFormat:@"%@ is required.", argumentName ?: @"path"];
            *errorOut = video_transcoder_error(VideoTranscoderErrorInvalidArguments, message);
        }
        return @"";
    }

    NSFileManager *fm = NSFileManager.defaultManager;
    BOOL isDirectory = NO;
    if (![fm fileExistsAtPath:path isDirectory:&isDirectory] || isDirectory) {
        if (errorOut) {
            NSString *message = [NSString stringWithFormat:@"%@ must be an existing file.", argumentName ?: @"path"];
            *errorOut = video_transcoder_error(VideoTranscoderErrorInvalidArguments, message);
        }
        return @"";
    }

    return path;
}

static NSString *video_transcoder_resolve_output_path(NSString *_Nullable outputPath, NSError **errorOut) {
    NSFileManager *fm = NSFileManager.defaultManager;

    NSString *path = outputPath;
    if (path) {
        path = [[path stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet]
            stringByExpandingTildeInPath];
    }

    if (!path || path.length == 0) {
        NSError *managedPathError = nil;
        path = [[AwesomeVideoFileManager sharedInstance] createManagedVideoOutputPathWithPrefix:@"video_transcode"
                                                                                       error:&managedPathError];
        if (path.length == 0) {
            if (errorOut) *errorOut = video_transcoder_wrap_managed_video_error(managedPathError);
            return @"";
        }
    }

    BOOL isDirectory = NO;
    if ([fm fileExistsAtPath:path isDirectory:&isDirectory] && isDirectory) {
        path = [path stringByAppendingPathComponent:video_transcoder_random_file_stem()];
    } else if ([path hasSuffix:@"/"]) {
        NSString *trimmed = [path stringByTrimmingCharactersInSet:[NSCharacterSet characterSetWithCharactersInString:@"/"]];
        path = [trimmed stringByAppendingPathComponent:video_transcoder_random_file_stem()];
    }

    path = video_transcoder_ensure_mp4_extension(path);

    NSString *dir = [path stringByDeletingLastPathComponent];
    if (dir.length == 0) {
        if (errorOut) *errorOut = video_transcoder_error(VideoTranscoderErrorInvalidArguments, @"Invalid outputPath.");
        return @"";
    }

    NSError *mkdirError = nil;
    if (![fm createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:&mkdirError]) {
        if (errorOut) {
            NSMutableDictionary *info = [@{NSLocalizedDescriptionKey : @"Failed to create output directory."} mutableCopy];
            if (mkdirError) info[NSUnderlyingErrorKey] = mkdirError;
            *errorOut = [NSError errorWithDomain:VideoTranscoderErrorDomain
                                            code:VideoTranscoderErrorCreateDirectoryFailed
                                        userInfo:info];
        }
        return @"";
    }

    return path;
}

@interface AwesomeVideoTranscoderManager ()
@property (atomic, readwrite) BOOL isTranscoding;
@property (atomic, readwrite) BOOL isPaused;
@property (atomic, readwrite, nullable) NSString *currentOutputPath;

@end

@implementation AwesomeVideoTranscoderManager {
    dispatch_queue_t _workQueue;
    volatile int _cancelFlag;
    volatile int _pauseFlags;
}

static AwesomeVideoTranscoderManager *_instance = nil;

// 返回单例转码器，用来串行化所有 FFmpeg 任务。
+ (instancetype)sharedInstance {
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        _instance = [[AwesomeVideoTranscoderManager alloc] init];
    });
    return _instance;
}

// 初始化任务状态，并注册应用生命周期监听以支持暂停和恢复。
- (instancetype)init {
    self = [super init];
    if (self) {
        _workQueue = dispatch_queue_create("video_transcoder.work", DISPATCH_QUEUE_SERIAL);
        _cancelFlag = 0;
        _pauseFlags = 0;
        [self registerLifecycleObservers];
    }
    return self;
}

// 在管理器释放前移除生命周期监听。
- (void)dealloc {
    [self unregisterLifecycleObservers];
}

// 监听应用前后台切换，并同步更新暂停标记。
- (void)registerLifecycleObservers {
#if __has_include(<UIKit/UIKit.h>)
    NSNotificationCenter *notificationCenter = NSNotificationCenter.defaultCenter;
    [notificationCenter addObserver:self
                           selector:@selector(handleApplicationDidEnterBackground:)
                               name:UIApplicationDidEnterBackgroundNotification
                             object:nil];
    [notificationCenter addObserver:self
                           selector:@selector(handleApplicationWillEnterForeground:)
                               name:UIApplicationWillEnterForegroundNotification
                             object:nil];
#endif
}

// 移除当前管理器注册的全部生命周期通知。
- (void)unregisterLifecycleObservers {
#if __has_include(<UIKit/UIKit.h>)
    [NSNotificationCenter.defaultCenter removeObserver:self
                                                  name:UIApplicationDidEnterBackgroundNotification
                                                object:nil];
    [NSNotificationCenter.defaultCenter removeObserver:self
                                                  name:UIApplicationWillEnterForegroundNotification
                                                object:nil];
#endif
}

// 在新任务开始时继承当前的生命周期暂停状态。
- (int)initialPauseFlagsForNewTask {
    int pauseFlags = 0;
#if __has_include(<UIKit/UIKit.h>)
    if (UIApplication.sharedApplication.applicationState == UIApplicationStateBackground) {
        pauseFlags |= VideoTranscoderPauseReasonLifecycle;
    }
#endif
    return pauseFlags;
}

// 占用唯一活动任务槽位，并重置共享取消状态。
- (BOOL)beginTaskIfPossibleWithCompletion:(nullable VideoTranscoderCompletionBlock)completion {
    NSError *busyError = nil;
    @synchronized (self) {
        if (self.isTranscoding) {
            busyError = video_transcoder_error(VideoTranscoderErrorBusy, @"A transcode task is already running.");
        } else {
            self.isTranscoding = YES;
            self.isPaused = NO;
            self.currentOutputPath = nil;
            _cancelFlag = 0;
            _pauseFlags = [self initialPauseFlagsForNewTask];
            self.isPaused = (_pauseFlags != 0);
        }
    }

    if (busyError) {
        if (completion) completion(nil, busyError);
        return NO;
    }
    return YES;
}

// 在调用方已持有同步锁的前提下清理任务状态。
- (void)resetTaskStateLocked {
    self.isTranscoding = NO;
    self.isPaused = NO;
    self.currentOutputPath = nil;
    _cancelFlag = 0;
    _pauseFlags = 0;
}

// 线程安全地清理任务状态。
- (void)resetTaskState {
    @synchronized (self) {
        [self resetTaskStateLocked];
    }
}

// 增减某个暂停原因，同时不影响其他已生效的暂停来源。
- (void)updatePauseReason:(VideoTranscoderPauseReason)reason enabled:(BOOL)enabled {
    @synchronized (self) {
        if (!self.isTranscoding) return;
        if (enabled) {
            _pauseFlags |= reason;
        } else {
            _pauseFlags &= ~reason;
        }
        self.isPaused = (_pauseFlags != 0);
    }
}

// 当应用进入后台时，自动暂停当前任务。
- (void)handleApplicationDidEnterBackground:(NSNotification *)notification {
    (void)notification;
    [self updatePauseReason:VideoTranscoderPauseReasonLifecycle enabled:YES];
}

// 当应用回到前台时，移除生命周期带来的暂停标记。
- (void)handleApplicationWillEnterForeground:(NSNotification *)notification {
    (void)notification;
    [self updatePauseReason:VideoTranscoderPauseReasonLifecycle enabled:NO];
}

// 将 options 对象展开为基础参数，并转发到带水印能力的转码入口。
- (void)transcodeVideoAtPath:(NSString *)inputPath
                  outputPath:(nullable NSString *)outputPath
                     options:(nullable AwesomeVideoTranscodeOptions *)options
                    progress:(nullable VideoTranscoderProgressBlock)progress
                  completion:(nullable VideoTranscoderCompletionBlock)completion {
    [self transcodeVideoAtPath:inputPath
                    outputPath:outputPath
                     overwrite:video_transcoder_bool_or_default(options.overwrite, kVideoTranscoderDefaultOverwrite)
                           crf:video_transcoder_integer_or_default(options.crf, kVideoTranscoderDefaultCRF)
                        preset:options.preset
                       profile:options.profile
                         level:options.level
                    videoWidth:video_transcoder_integer_or_default(options.videoWidth, kVideoTranscoderDefaultVideoWidth)
                   videoHeight:video_transcoder_integer_or_default(options.videoHeight, kVideoTranscoderDefaultVideoHeight)
                  audioBitrate:video_transcoder_integer_or_default(options.audioBitrate, kVideoTranscoderDefaultAudioBitrate)
                     faststart:video_transcoder_bool_or_default(options.faststart, kVideoTranscoderDefaultFaststart)
                     frameRate:video_transcoder_integer_or_default(options.frameRate, kVideoTranscoderDefaultFrameRate)
            watermarkImagePath:options.watermarkImagePath
             watermarkPosition:video_transcoder_watermark_position_or_default(options.watermarkPosition)
                      progress:progress
                    completion:completion];
}

// 根据参数决定走纯拼接尾图流程，还是水印加尾图的组合流程。
- (void)transcodeVideoAtPath:(NSString *)inputPath
                  outputPath:(nullable NSString *)outputPath
             concatImagePath:(NSString *)concatImagePath
                     options:(nullable AwesomeVideoConcatOptions *)options
                    progress:(nullable VideoTranscoderProgressBlock)progress
                  completion:(nullable VideoTranscoderCompletionBlock)completion {
    const BOOL overwrite = video_transcoder_bool_or_default(options.overwrite, kVideoTranscoderDefaultOverwrite);
    const NSTimeInterval imageDuration = video_transcoder_time_interval_or_default(
        options.imageDuration,
        kVideoTranscoderDefaultConcatImageDuration
    );
    const VideoConcatImagePosition concatPosition = video_transcoder_concat_position_or_default(options.concatPosition);
    NSString *watermarkImagePath = options.watermarkImagePath;

    if (watermarkImagePath.length > 0) {
        [self transcodeVideoAtPath:inputPath
                        outputPath:outputPath
                         overwrite:overwrite
                watermarkImagePath:watermarkImagePath
                 watermarkPosition:video_transcoder_watermark_position_or_default(options.watermarkPosition)
                   concatImagePath:concatImagePath
                     imageDuration:imageDuration
                    concatPosition:concatPosition
                          progress:progress
                        completion:completion];
        return;
    }

    [self transcodeVideoAtPath:inputPath
                    outputPath:outputPath
                     overwrite:overwrite
               concatImagePath:concatImagePath
                 imageDuration:imageDuration
                concatPosition:concatPosition
                      progress:progress
                    completion:completion];
}

// 将分离音频模式的 options 展开后，转发到基础媒体转码入口。
- (void)transcodeMediaAtPath:(NSString *)inputPath
                   audioPath:(NSString *)audioPath
                  outputPath:(nullable NSString *)outputPath
                     options:(nullable AwesomeVideoSeparateAudioOptions *)options
                    progress:(nullable VideoTranscoderProgressBlock)progress
                  completion:(nullable VideoTranscoderCompletionBlock)completion {
    [self transcodeMediaAtPath:inputPath
                     audioPath:audioPath
                    outputPath:outputPath
                     overwrite:video_transcoder_bool_or_default(options.overwrite, kVideoTranscoderDefaultOverwrite)
                  audioBitrate:video_transcoder_integer_or_default(options.audioBitrate, kVideoTranscoderDefaultAudioBitrate)
                     faststart:video_transcoder_bool_or_default(options.faststart, kVideoTranscoderDefaultFaststart)
                     frameRate:video_transcoder_integer_or_default(options.frameRate, kVideoTranscoderDefaultFrameRate)
                      progress:progress
                    completion:completion];
}


// 校验尾图参数、准备输出路径，并在工作队列中执行尾图拼接转码。
- (void)transcodeVideoAtPath:(NSString *)inputPath
                  outputPath:(nullable NSString *)outputPath
                   overwrite:(BOOL)overwrite
             concatImagePath:(NSString *)concatImagePath
               imageDuration:(NSTimeInterval)imageDuration
              concatPosition:(VideoConcatImagePosition)concatPosition
                    progress:(nullable VideoTranscoderProgressBlock)progress
                  completion:(nullable VideoTranscoderCompletionBlock)completion {
    NSError *inputError = nil;
    NSString *resolvedInputPath = video_transcoder_resolve_input_path(inputPath, &inputError);
    if (inputError) {
        if (completion) completion(nil, inputError);
        return;
    }

    NSError *concatImageError = nil;
    NSString *resolvedConcatImagePath = video_transcoder_resolve_required_file_path(
        concatImagePath,
        @"concatImagePath",
        &concatImageError
    );
    if (concatImageError) {
        if (completion) completion(nil, concatImageError);
        return;
    }

    if (imageDuration <= 0) {
        if (completion) {
            completion(nil, video_transcoder_error(VideoTranscoderErrorInvalidArguments, @"imageDuration must be greater than 0."));
        }
        return;
    }

    if (![self beginTaskIfPossibleWithCompletion:completion]) return;

    NSError *outputError = nil;
    NSString *resolvedOutputPath = video_transcoder_resolve_output_path(outputPath, &outputError);
    if (outputError) {
        [self resetTaskState];
        if (completion) completion(nil, outputError);
        return;
    }

    if ([[resolvedInputPath stringByStandardizingPath] isEqualToString:[resolvedOutputPath stringByStandardizingPath]]) {
        [self resetTaskState];
        if (completion) completion(nil, video_transcoder_error(VideoTranscoderErrorInvalidArguments, @"inputPath and outputPath must be different."));
        return;
    }

    NSFileManager *fm = NSFileManager.defaultManager;
    BOOL outputExists = [fm fileExistsAtPath:resolvedOutputPath];
    if (outputExists && !overwrite) {
        [self resetTaskState];
        if (completion) completion(nil, video_transcoder_error(VideoTranscoderErrorFileExists, @"outputPath already exists."));
        return;
    }
    if (outputExists && overwrite) {
        (void)[fm removeItemAtPath:resolvedOutputPath error:nil];
    }

    @synchronized (self) {
        self.currentOutputPath = resolvedOutputPath;
    }

    __weak typeof(self) weakSelf = self;
    dispatch_async(_workQueue, ^{
        __strong typeof(self) strongSelf = weakSelf;
        if (!strongSelf) return;

        @autoreleasepool {
            const char *inputPathC = resolvedInputPath.fileSystemRepresentation;
            const char *outputPathC = resolvedOutputPath.fileSystemRepresentation;
            const char *concatImagePathC = resolvedConcatImagePath.fileSystemRepresentation;
            const int64_t imageDurationUs = (int64_t)(imageDuration * 1000000.0);

            if (!inputPathC || !outputPathC || !concatImagePathC || imageDurationUs <= 0) {
                dispatch_async(dispatch_get_main_queue(), ^{
                    @synchronized (strongSelf) {
                        [strongSelf resetTaskStateLocked];
                    }
                    if (completion) {
                        completion(nil, video_transcoder_error(VideoTranscoderErrorInvalidArguments, @"Failed to convert concat image arguments."));
                    }
                });
                return;
            }

            _VideoTranscoderProgressContext *ctx = [_VideoTranscoderProgressContext new];
            ctx.progress = progress;
            ctx.cancelFlag = &strongSelf->_cancelFlag;

            FFmpegMp4ConcatImageConfig concatImage = {0};
            concatImage.image_path = concatImagePathC;
            concatImage.image_duration_us = imageDurationUs;
            concatImage.position = video_transcoder_to_c_concat_position(concatPosition);

            void *ctxRef = (__bridge_retained void *)ctx;
            const int ret = transcode_file_to_mp4_with_concat_image(
                inputPathC,
                outputPathC,
                &concatImage,
                progress ? video_transcoder_progress_cb : NULL,
                ctxRef,
                &strongSelf->_cancelFlag,
                &strongSelf->_pauseFlags
            );
            (void)CFBridgingRelease(ctxRef);

            const BOOL cancelled = (strongSelf->_cancelFlag != 0);
            if (cancelled) {
                (void)[fm removeItemAtPath:resolvedOutputPath error:nil];
            }

            NSError *error = nil;
            if (cancelled) {
                error = video_transcoder_error(VideoTranscoderErrorCancelled, @"Cancelled.");
            } else if (ret != 0) {
                NSString *message = [NSString stringWithFormat:@"FFmpeg failed (code=%d).", ret];
                message = [message stringByAppendingString:@" Concat image mode requires image decoding support, H.264 video encoding, and AAC audio encoding when the source video contains audio."];
                error = video_transcoder_error(VideoTranscoderErrorFFmpegFailed, message);
            }

            dispatch_async(dispatch_get_main_queue(), ^{
                [strongSelf resetTaskState];
                if (completion) completion(error ? nil : resolvedOutputPath, error);
            });
        }
    });
}

// 在校验水印图和尾图资源后，执行水印加尾图的组合转码流程。
- (void)transcodeVideoAtPath:(NSString *)inputPath
                  outputPath:(nullable NSString *)outputPath
                   overwrite:(BOOL)overwrite
          watermarkImagePath:(NSString *)watermarkImagePath
           watermarkPosition:(VideoWatermarkPosition)watermarkPosition
             concatImagePath:(NSString *)concatImagePath
               imageDuration:(NSTimeInterval)imageDuration
              concatPosition:(VideoConcatImagePosition)concatPosition
                    progress:(nullable VideoTranscoderProgressBlock)progress
                  completion:(nullable VideoTranscoderCompletionBlock)completion {
    NSError *inputError = nil;
    NSString *resolvedInputPath = video_transcoder_resolve_input_path(inputPath, &inputError);
    if (inputError) {
        if (completion) completion(nil, inputError);
        return;
    }

    NSError *watermarkError = nil;
    NSString *resolvedWatermarkPath = video_transcoder_resolve_required_file_path(
        watermarkImagePath,
        @"watermarkImagePath",
        &watermarkError
    );
    if (watermarkError) {
        if (completion) completion(nil, watermarkError);
        return;
    }

    NSError *concatImageError = nil;
    NSString *resolvedConcatImagePath = video_transcoder_resolve_required_file_path(
        concatImagePath,
        @"concatImagePath",
        &concatImageError
    );
    if (concatImageError) {
        if (completion) completion(nil, concatImageError);
        return;
    }

    if (imageDuration <= 0) {
        if (completion) {
            completion(nil, video_transcoder_error(VideoTranscoderErrorInvalidArguments, @"imageDuration must be greater than 0."));
        }
        return;
    }

    if (![self beginTaskIfPossibleWithCompletion:completion]) return;

    NSError *outputError = nil;
    NSString *resolvedOutputPath = video_transcoder_resolve_output_path(outputPath, &outputError);
    if (outputError) {
        [self resetTaskState];
        if (completion) completion(nil, outputError);
        return;
    }

    if ([[resolvedInputPath stringByStandardizingPath] isEqualToString:[resolvedOutputPath stringByStandardizingPath]]) {
        [self resetTaskState];
        if (completion) completion(nil, video_transcoder_error(VideoTranscoderErrorInvalidArguments, @"inputPath and outputPath must be different."));
        return;
    }

    NSFileManager *fm = NSFileManager.defaultManager;
    BOOL outputExists = [fm fileExistsAtPath:resolvedOutputPath];
    if (outputExists && !overwrite) {
        [self resetTaskState];
        if (completion) completion(nil, video_transcoder_error(VideoTranscoderErrorFileExists, @"outputPath already exists."));
        return;
    }
    if (outputExists && overwrite) {
        (void)[fm removeItemAtPath:resolvedOutputPath error:nil];
    }

    @synchronized (self) {
        self.currentOutputPath = resolvedOutputPath;
    }

    __weak typeof(self) weakSelf = self;
    dispatch_async(_workQueue, ^{
        __strong typeof(self) strongSelf = weakSelf;
        if (!strongSelf) return;

        @autoreleasepool {
            const char *inputPathC = resolvedInputPath.fileSystemRepresentation;
            const char *outputPathC = resolvedOutputPath.fileSystemRepresentation;
            const char *watermarkPathC = resolvedWatermarkPath.fileSystemRepresentation;
            const char *concatImagePathC = resolvedConcatImagePath.fileSystemRepresentation;
            const int64_t imageDurationUs = (int64_t)(imageDuration * 1000000.0);

            if (!inputPathC || !outputPathC || !watermarkPathC || !concatImagePathC || imageDurationUs <= 0) {
                dispatch_async(dispatch_get_main_queue(), ^{
                    @synchronized (strongSelf) {
                        [strongSelf resetTaskStateLocked];
                    }
                    if (completion) {
                        completion(nil, video_transcoder_error(VideoTranscoderErrorInvalidArguments, @"Failed to convert watermark concat arguments."));
                    }
                });
                return;
            }

            _VideoTranscoderProgressContext *ctx = [_VideoTranscoderProgressContext new];
            ctx.progress = progress;
            ctx.cancelFlag = &strongSelf->_cancelFlag;

            FFmpegWatermarkConfig watermark = {0};
            watermark.image_path = watermarkPathC;
            watermark.position = video_transcoder_to_c_watermark_position(watermarkPosition);
            watermark.margin_x = 20;
            watermark.margin_y = 20;
            watermark.width = 0;
            watermark.height = 0;

            FFmpegMp4ConcatImageConfig concatImage = {0};
            concatImage.image_path = concatImagePathC;
            concatImage.image_duration_us = imageDurationUs;
            concatImage.position = video_transcoder_to_c_concat_position(concatPosition);

            void *ctxRef = (__bridge_retained void *)ctx;
            const int ret = transcode_file_to_mp4_with_watermark_and_concat_image(
                inputPathC,
                outputPathC,
                NULL,
                &watermark,
                &concatImage,
                progress ? video_transcoder_progress_cb : NULL,
                ctxRef,
                &strongSelf->_cancelFlag,
                &strongSelf->_pauseFlags
            );
            (void)CFBridgingRelease(ctxRef);

            const BOOL cancelled = (strongSelf->_cancelFlag != 0);
            if (cancelled) {
                (void)[fm removeItemAtPath:resolvedOutputPath error:nil];
            }

            NSError *error = nil;
            if (cancelled) {
                error = video_transcoder_error(VideoTranscoderErrorCancelled, @"Cancelled.");
            } else if (ret != 0) {
                NSString *message = [NSString stringWithFormat:@"FFmpeg failed (code=%d).", ret];
                message = [message stringByAppendingString:@" Watermark+concat image mode requires FFmpeg built with libavfilter/libswscale, image decoding support, H.264 video encoding, and AAC audio encoding when the source video contains audio."];
                error = video_transcoder_error(VideoTranscoderErrorFFmpegFailed, message);
            }

            dispatch_async(dispatch_get_main_queue(), ^{
                [strongSelf resetTaskState];
                if (completion) completion(error ? nil : resolvedOutputPath, error);
            });
        }
    });
}


// 校验视频源和音频源后，异步执行分离音频的合成或转码流程。
- (void)transcodeMediaAtPath:(NSString *)inputPath
                   audioPath:(NSString *)audioPath
                  outputPath:(nullable NSString *)outputPath
                   overwrite:(BOOL)overwrite
                audioBitrate:(NSInteger)audioBitrate
                   faststart:(BOOL)faststart
                   frameRate:(NSInteger)frameRate
                    progress:(nullable VideoTranscoderProgressBlock)progress
                  completion:(nullable VideoTranscoderCompletionBlock)completion {
    NSError *inputError = nil;
    NSString *resolvedInputPath = video_transcoder_resolve_input_path(inputPath, &inputError);
    if (inputError) {
        if (completion) completion(nil, inputError);
        return;
    }

    NSError *audioError = nil;
    NSString *resolvedAudioPath = video_transcoder_resolve_required_file_path(
        audioPath,
        @"audioPath",
        &audioError
    );
    if (audioError) {
        if (completion) completion(nil, audioError);
        return;
    }

    if (![self beginTaskIfPossibleWithCompletion:completion]) return;

    NSError *outputError = nil;
    NSString *resolvedOutputPath = video_transcoder_resolve_output_path(outputPath, &outputError);
    if (outputError) {
        [self resetTaskState];
        if (completion) completion(nil, outputError);
        return;
    }

    NSString *standardizedOutputPath = [resolvedOutputPath stringByStandardizingPath];
    if ([[resolvedInputPath stringByStandardizingPath] isEqualToString:standardizedOutputPath] ||
        [[resolvedAudioPath stringByStandardizingPath] isEqualToString:standardizedOutputPath]) {
        [self resetTaskState];
        if (completion) completion(nil, video_transcoder_error(VideoTranscoderErrorInvalidArguments, @"inputPath/audioPath and outputPath must be different."));
        return;
    }

    NSFileManager *fm = NSFileManager.defaultManager;
    BOOL outputExists = [fm fileExistsAtPath:resolvedOutputPath];
    if (outputExists && !overwrite) {
        [self resetTaskState];
        if (completion) completion(nil, video_transcoder_error(VideoTranscoderErrorFileExists, @"outputPath already exists."));
        return;
    }
    if (outputExists && overwrite) {
        (void)[fm removeItemAtPath:resolvedOutputPath error:nil];
    }

    @synchronized (self) {
        self.currentOutputPath = resolvedOutputPath;
    }

    __weak typeof(self) weakSelf = self;
    dispatch_async(_workQueue, ^{
        __strong typeof(self) strongSelf = weakSelf;
        if (!strongSelf) return;

        @autoreleasepool {
            const char *inputPathC = resolvedInputPath.fileSystemRepresentation;
            const char *audioPathC = resolvedAudioPath.fileSystemRepresentation;
            const char *outputPathC = resolvedOutputPath.fileSystemRepresentation;

            if (!inputPathC || !audioPathC || !outputPathC) {
                dispatch_async(dispatch_get_main_queue(), ^{
                    @synchronized (strongSelf) {
                        [strongSelf resetTaskStateLocked];
                    }
                    if (completion) {
                        completion(nil, video_transcoder_error(VideoTranscoderErrorInvalidArguments, @"Failed to convert media/audio arguments to C strings."));
                    }
                });
                return;
            }

            _VideoTranscoderProgressContext *ctx = [_VideoTranscoderProgressContext new];
            ctx.progress = progress;
            ctx.cancelFlag = &strongSelf->_cancelFlag;

            FFmpegMp4TranscodeConfig config = {0};
            config.audio_bitrate = (int)audioBitrate;
            config.faststart = faststart ? 1 : -1;
            config.frame_rate = (int)frameRate;

            void *ctxRef = (__bridge_retained void *)ctx;
            const int ret = transcode_file_with_separate_audio_to_mp4(
                inputPathC,
                audioPathC,
                outputPathC,
                &config,
                progress ? video_transcoder_progress_cb : NULL,
                ctxRef,
                &strongSelf->_cancelFlag,
                &strongSelf->_pauseFlags
            );
            (void)CFBridgingRelease(ctxRef);

            const BOOL cancelled = (strongSelf->_cancelFlag != 0);
            if (cancelled) {
                (void)[fm removeItemAtPath:resolvedOutputPath error:nil];
            }

            NSError *error = nil;
            if (cancelled) {
                error = video_transcoder_error(VideoTranscoderErrorCancelled, @"Cancelled.");
            } else if (ret != 0) {
                NSString *message = [NSString stringWithFormat:@"FFmpeg failed (code=%d).", ret];
                message = [message stringByAppendingString:@" Separate-audio mode requires a valid visual input (video or still image), a valid audio input, AAC encoding support when the audio cannot be copied, and H.264 encoding support when the visual input is a still image."];
                error = video_transcoder_error(VideoTranscoderErrorFFmpegFailed, message);
            }

            dispatch_async(dispatch_get_main_queue(), ^{
                [strongSelf resetTaskState];
                if (completion) completion(error ? nil : resolvedOutputPath, error);
            });
        }
    });
}


// 执行标准 MP4 转码；如果传入水印图，则切换到带水印的转码流程。
- (void)transcodeVideoAtPath:(NSString *)inputPath
                  outputPath:(nullable NSString *)outputPath
                   overwrite:(BOOL)overwrite
                         crf:(NSInteger)crf
                      preset:(nullable NSString *)preset
                     profile:(nullable NSString *)profile
                       level:(nullable NSString *)level
                  videoWidth:(NSInteger)videoWidth
                 videoHeight:(NSInteger)videoHeight
                audioBitrate:(NSInteger)audioBitrate
                   faststart:(BOOL)faststart
                   frameRate:(NSInteger)frameRate
          watermarkImagePath:(nullable NSString *)watermarkImagePath
           watermarkPosition:(VideoWatermarkPosition)watermarkPosition
                    progress:(nullable VideoTranscoderProgressBlock)progress
                  completion:(nullable VideoTranscoderCompletionBlock)completion {
    NSError *inputError = nil;
    NSString *resolvedInputPath = video_transcoder_resolve_input_path(inputPath, &inputError);
    if (inputError) {
        if (completion) completion(nil, inputError);
        return;
    }

    NSString *trimmedWatermarkPath = nil;
    if (watermarkImagePath.length > 0) {
        trimmedWatermarkPath = [[[watermarkImagePath stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet]
            stringByExpandingTildeInPath] copy];
        if (trimmedWatermarkPath.length == 0) {
            trimmedWatermarkPath = nil;
        }
    }

    if (![self beginTaskIfPossibleWithCompletion:completion]) return;

    NSError *outputError = nil;
    NSString *resolvedOutputPath = video_transcoder_resolve_output_path(outputPath, &outputError);
    if (outputError) {
        [self resetTaskState];
        if (completion) completion(nil, outputError);
        return;
    }

    if ([[resolvedInputPath stringByStandardizingPath] isEqualToString:[resolvedOutputPath stringByStandardizingPath]]) {
        [self resetTaskState];
        if (completion) completion(nil, video_transcoder_error(VideoTranscoderErrorInvalidArguments, @"inputPath and outputPath must be different."));
        return;
    }

    NSFileManager *fm = NSFileManager.defaultManager;
    BOOL outputExists = [fm fileExistsAtPath:resolvedOutputPath];
    if (outputExists && !overwrite) {
        [self resetTaskState];
        if (completion) completion(nil, video_transcoder_error(VideoTranscoderErrorFileExists, @"outputPath already exists."));
        return;
    }
    if (outputExists && overwrite) {
        (void)[fm removeItemAtPath:resolvedOutputPath error:nil];
    }

    NSString *presetCopy = [preset copy];
    NSString *profileCopy = [profile copy];
    NSString *levelCopy = [level copy];
    @synchronized (self) {
        self.currentOutputPath = resolvedOutputPath;
    }

    __weak typeof(self) weakSelf = self;
    dispatch_async(_workQueue, ^{
        __strong typeof(self) strongSelf = weakSelf;
        if (!strongSelf) return;

        @autoreleasepool {
            const char *inputPathC = resolvedInputPath.fileSystemRepresentation;
            const char *outputPathC = resolvedOutputPath.fileSystemRepresentation;
            const char *presetC = presetCopy.length > 0 ? presetCopy.UTF8String : NULL;
            const char *profileC = profileCopy.length > 0 ? profileCopy.UTF8String : NULL;
            const char *levelC = levelCopy.length > 0 ? levelCopy.UTF8String : NULL;
            const char *watermarkPathC = trimmedWatermarkPath.fileSystemRepresentation;
            const BOOL needsWatermark = (trimmedWatermarkPath.length > 0 && watermarkPathC && *watermarkPathC);

            if (!inputPathC || !outputPathC) {
                dispatch_async(dispatch_get_main_queue(), ^{
                    @synchronized (strongSelf) {
                        [strongSelf resetTaskStateLocked];
                    }
                    if (completion) {
                        completion(nil, video_transcoder_error(VideoTranscoderErrorInvalidArguments, @"Failed to convert arguments to C strings."));
                    }
                });
                return;
            }

            _VideoTranscoderProgressContext *ctx = [_VideoTranscoderProgressContext new];
            ctx.progress = progress;
            ctx.cancelFlag = &strongSelf->_cancelFlag;

            FFmpegMp4TranscodeConfig config = {0};
            config.crf = (int)crf;
            config.preset = presetC;
            config.profile = profileC;
            config.level = levelC;
            config.audio_bitrate = (int)audioBitrate;
            config.faststart = faststart ? 1 : -1;
            config.scale_width = (int)videoWidth;
            config.scale_height = (int)videoHeight;
            config.frame_rate = (int)frameRate;

            void *ctxRef = (__bridge_retained void *)ctx;
            int ret = 0;
            if (needsWatermark) {
                FFmpegWatermarkConfig watermark = {0};
                watermark.image_path = watermarkPathC;
                watermark.position = video_transcoder_to_c_watermark_position(watermarkPosition);
                watermark.margin_x = 20;
                watermark.margin_y = 20;
                watermark.width = 0;
                watermark.height = 0;
                ret = transcode_file_to_mp4_with_watermark(
                    inputPathC,
                    outputPathC,
                    &config,
                    &watermark,
                    progress ? video_transcoder_progress_cb : NULL,
                    ctxRef,
                    &strongSelf->_cancelFlag,
                    &strongSelf->_pauseFlags
                );
            } else {
                ret = transcode_file_to_mp4(
                    inputPathC,
                    outputPathC,
                    &config,
                    progress ? video_transcoder_progress_cb : NULL,
                    ctxRef,
                    &strongSelf->_cancelFlag,
                    &strongSelf->_pauseFlags
                );
            }
            (void)CFBridgingRelease(ctxRef);

            const BOOL cancelled = (strongSelf->_cancelFlag != 0);
            if (cancelled) {
                (void)[fm removeItemAtPath:resolvedOutputPath error:nil];
            }

            NSError *error = nil;
            if (cancelled) {
                error = video_transcoder_error(VideoTranscoderErrorCancelled, @"Cancelled.");
            } else if (ret != 0) {
                NSString *message = [NSString stringWithFormat:@"FFmpeg failed (code=%d).", ret];
                if (needsWatermark) {
                    message = [message stringByAppendingString:@" Watermark mode requires FFmpeg built with libavfilter/libswscale and image decoding support."];
                }
                error = video_transcoder_error(
                    VideoTranscoderErrorFFmpegFailed,
                    message
                );
            }

            dispatch_async(dispatch_get_main_queue(), ^{
                [strongSelf resetTaskState];
                if (completion) completion(error ? nil : resolvedOutputPath, error);
            });
        }
    });
}

// 通知当前 FFmpeg 任务取消，并清除暂停状态以便尽快退出。
- (void)cancelCurrentTask {
    @synchronized (self) {
        if (!self.isTranscoding) return;
        _cancelFlag = 1;
        _pauseFlags = 0;
        self.isPaused = NO;
    }
}

// 增加手动暂停标记，供 FFmpeg 循环在长任务中轮询。
- (void)pauseCurrentTask {
    [self updatePauseReason:VideoTranscoderPauseReasonManual enabled:YES];
}

// 移除手动暂停标记，让 FFmpeg 循环继续执行。
- (void)resumeCurrentTask {
    [self updatePauseReason:VideoTranscoderPauseReasonManual enabled:NO];
}

@end
