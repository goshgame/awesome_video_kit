//
//  AwesomeVideoFrameCaptureManager.m
//  AwesomeVideoKitSDK
//
//  Created by Codex on 2026/6/2.
//

#import "AwesomeVideoFrameCaptureManager.h"

#import "ffmpeg_video_frame_extractor.h"

#import <cmath>
#import <vector>

NSErrorDomain const AwesomeVideoFrameCaptureErrorDomain = @"awesome_video_frame_capture";

static NSError *awesome_video_frame_capture_error(
    AwesomeVideoFrameCaptureErrorCode code,
    NSString *message,
    NSNumber *_Nullable ffmpegCode
) {
    NSMutableDictionary *userInfo = [@{
        NSLocalizedDescriptionKey: message ?: @"Unknown error",
    } mutableCopy];
    if (ffmpegCode) userInfo[@"ffmpeg_error_code"] = ffmpegCode;
    return [NSError errorWithDomain:AwesomeVideoFrameCaptureErrorDomain code:code userInfo:userInfo];
}

static NSString *awesome_video_frame_capture_normalize_path(NSString *_Nullable path) {
    NSString *trimmedPath = [[path ?: @"" stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet] copy];
    if (trimmedPath.length == 0) return @"";
    NSURLComponents *components = [NSURLComponents componentsWithString:trimmedPath];
    NSString *scheme = components.scheme.lowercaseString;
    if (([scheme isEqualToString:@"http"] || [scheme isEqualToString:@"https"]) && components.host.length > 0) {
        return trimmedPath;
    }
    return [[[trimmedPath stringByExpandingTildeInPath] stringByStandardizingPath] copy];
}

static BOOL awesome_video_frame_capture_is_network_url(NSString *_Nullable path) {
    NSString *trimmedPath = [path ?: @"" stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    NSURLComponents *components = [NSURLComponents componentsWithString:trimmedPath];
    NSString *scheme = components.scheme.lowercaseString;
    return ([scheme isEqualToString:@"http"] || [scheme isEqualToString:@"https"]) && components.host.length > 0;
}

static NSString *awesome_video_frame_capture_default_directory_path(void) {
    NSArray<NSString *> *paths = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES);
    NSString *documentsPath = paths.firstObject;
    return documentsPath.length > 0 ? [documentsPath stringByAppendingPathComponent:@"AwesomeGeneratedVideos"] : @"";
}

static NSString *awesome_video_frame_capture_default_file_name(NSString *inputPath, NSTimeInterval timeSeconds, NSNumber *_Nullable index) {
    NSString *baseName = nil;
    if (awesome_video_frame_capture_is_network_url(inputPath)) {
        baseName = [NSURLComponents componentsWithString:inputPath].path.lastPathComponent.stringByDeletingPathExtension;
    } else {
        baseName = inputPath.lastPathComponent.stringByDeletingPathExtension;
    }
    if (baseName.length == 0) baseName = @"video";

    long long timeMs = (long long)(MAX(timeSeconds, 0.0) * 1000.0);
    if (index) {
        return [NSString stringWithFormat:@"%@_frame_%04ld_%lldms.jpg", baseName, (long)index.integerValue, timeMs];
    }
    return [NSString stringWithFormat:@"%@_frame_%lldms.jpg", baseName, timeMs];
}

static BOOL awesome_video_frame_capture_create_directory(NSString *directoryPath, NSError **errorOut) {
    if (directoryPath.length == 0) {
        if (errorOut) {
            *errorOut = awesome_video_frame_capture_error(
                AwesomeVideoFrameCaptureErrorInvalidArguments,
                @"Invalid output directory.",
                nil
            );
        }
        return NO;
    }

    NSError *mkdirError = nil;
    if (![NSFileManager.defaultManager createDirectoryAtPath:directoryPath
                                 withIntermediateDirectories:YES
                                                  attributes:nil
                                                       error:&mkdirError]) {
        if (errorOut) {
            *errorOut = awesome_video_frame_capture_error(
                AwesomeVideoFrameCaptureErrorCreateDirectoryFailed,
                @"Failed to create output directory.",
                nil
            );
        }
        return NO;
    }
    return YES;
}

static BOOL awesome_video_frame_capture_prepare_output_path(NSString *outputPath, BOOL overwrite, NSError **errorOut) {
    NSFileManager *fileManager = NSFileManager.defaultManager;
    NSString *directoryPath = outputPath.stringByDeletingLastPathComponent;
    if (!awesome_video_frame_capture_create_directory(directoryPath, errorOut)) return NO;

    const BOOL exists = [fileManager fileExistsAtPath:outputPath];
    if (exists && !overwrite) {
        if (errorOut) {
            *errorOut = awesome_video_frame_capture_error(
                AwesomeVideoFrameCaptureErrorFileExists,
                @"outputPath already exists.",
                nil
            );
        }
        return NO;
    }

    if (exists && overwrite) {
        NSError *removeError = nil;
        if (![fileManager removeItemAtPath:outputPath error:&removeError]) {
            if (errorOut) {
                *errorOut = awesome_video_frame_capture_error(
                    AwesomeVideoFrameCaptureErrorFileExists,
                    @"Failed to overwrite existing outputPath.",
                    nil
                );
            }
            return NO;
        }
    }

    return YES;
}

static NSString *awesome_video_frame_capture_resolve_input_path(NSString *inputPath, NSError **errorOut) {
    NSString *path = awesome_video_frame_capture_normalize_path(inputPath);
    if (path.length == 0) {
        if (errorOut) {
            *errorOut = awesome_video_frame_capture_error(
                AwesomeVideoFrameCaptureErrorInvalidArguments,
                @"inputPath is required.",
                nil
            );
        }
        return @"";
    }

    if (awesome_video_frame_capture_is_network_url(path)) return path;

    BOOL isDirectory = NO;
    if (![NSFileManager.defaultManager fileExistsAtPath:path isDirectory:&isDirectory] || isDirectory) {
        if (errorOut) {
            *errorOut = awesome_video_frame_capture_error(
                AwesomeVideoFrameCaptureErrorInvalidArguments,
                @"inputPath must be an existing file.",
                nil
            );
        }
        return @"";
    }
    return path;
}

static NSString *awesome_video_frame_capture_resolve_single_output_path(
    NSString *inputPath,
    NSTimeInterval timeSeconds,
    NSString *_Nullable outputPath,
    BOOL overwrite,
    NSError **errorOut
) {
    NSString *normalizedOutputPath = awesome_video_frame_capture_normalize_path(outputPath);
    NSString *defaultFileName = awesome_video_frame_capture_default_file_name(inputPath, timeSeconds, nil);

    if (normalizedOutputPath.length == 0) {
        normalizedOutputPath = [awesome_video_frame_capture_default_directory_path() stringByAppendingPathComponent:defaultFileName];
    } else {
        BOOL isDirectory = NO;
        const BOOL exists = [NSFileManager.defaultManager fileExistsAtPath:normalizedOutputPath isDirectory:&isDirectory];
        if ((exists && isDirectory) || [normalizedOutputPath hasSuffix:@"/"]) {
            normalizedOutputPath = [normalizedOutputPath stringByAppendingPathComponent:defaultFileName];
        } else if (normalizedOutputPath.pathExtension.length == 0) {
            normalizedOutputPath = [normalizedOutputPath stringByAppendingPathExtension:@"jpg"];
        }
    }

    if (!awesome_video_frame_capture_prepare_output_path(normalizedOutputPath, overwrite, errorOut)) return @"";
    return normalizedOutputPath;
}

static NSArray<NSString *> *awesome_video_frame_capture_resolve_output_paths(
    NSString *inputPath,
    NSArray<NSNumber *> *timesSeconds,
    NSString *_Nullable outputDirectory,
    BOOL overwrite,
    NSError **errorOut
) {
    NSString *directoryPath = awesome_video_frame_capture_normalize_path(outputDirectory);
    if (directoryPath.length == 0) directoryPath = awesome_video_frame_capture_default_directory_path();
    if (!awesome_video_frame_capture_create_directory(directoryPath, errorOut)) return @[];

    NSMutableArray<NSString *> *paths = [NSMutableArray arrayWithCapacity:timesSeconds.count];
    for (NSUInteger index = 0; index < timesSeconds.count; index++) {
        NSString *fileName = awesome_video_frame_capture_default_file_name(inputPath, timesSeconds[index].doubleValue, @(index));
        NSString *outputPath = [directoryPath stringByAppendingPathComponent:fileName];
        if (!awesome_video_frame_capture_prepare_output_path(outputPath, overwrite, errorOut)) return @[];
        [paths addObject:outputPath];
    }
    return [paths copy];
}

@interface AwesomeVideoFrameCaptureManager ()
@property (atomic, readwrite) BOOL isCapturingVideoFrames;
@property (atomic, readwrite, nullable) NSString *currentOutputPath;
@end

@implementation AwesomeVideoFrameCaptureManager {
    dispatch_queue_t _workQueue;
    volatile int _cancelFlag;
}

+ (instancetype)sharedInstance {
    static AwesomeVideoFrameCaptureManager *instance = nil;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        instance = [[AwesomeVideoFrameCaptureManager alloc] init];
    });
    return instance;
}

- (instancetype)init {
    self = [super init];
    if (self) {
        _workQueue = dispatch_queue_create("awesome_video_kit.frame_capture", DISPATCH_QUEUE_SERIAL);
        _cancelFlag = 0;
    }
    return self;
}

- (BOOL)beginTaskIfPossibleWithCompletion:(void (^)(NSError *_Nullable error))completion {
    NSError *busyError = nil;
    @synchronized (self) {
        if (self.isCapturingVideoFrames) {
            busyError = awesome_video_frame_capture_error(
                AwesomeVideoFrameCaptureErrorBusy,
                @"A video frame capture task is already running.",
                nil
            );
        } else {
            self.isCapturingVideoFrames = YES;
            self.currentOutputPath = nil;
            _cancelFlag = 0;
        }
    }

    if (busyError) {
        if (completion) completion(busyError);
        return NO;
    }
    return YES;
}

- (void)resetTaskState {
    @synchronized (self) {
        self.isCapturingVideoFrames = NO;
        self.currentOutputPath = nil;
        _cancelFlag = 0;
    }
}

- (void)captureVideoFrameAtPath:(NSString *)inputPath
                    timeSeconds:(NSTimeInterval)timeSeconds
                     outputPath:(nullable NSString *)outputPath
                      overwrite:(BOOL)overwrite
                     completion:(nullable AwesomeVideoFrameCaptureCompletionBlock)completion {
    [self captureVideoFrameAtPath:inputPath
                      timeSeconds:timeSeconds
                       outputPath:outputPath
                        overwrite:overwrite
                      outputWidth:0
                     outputHeight:0
                       completion:completion];
}

- (void)captureVideoFrameAtPath:(NSString *)inputPath
                    timeSeconds:(NSTimeInterval)timeSeconds
                     outputPath:(nullable NSString *)outputPath
                      overwrite:(BOOL)overwrite
                    outputWidth:(NSInteger)outputWidth
                   outputHeight:(NSInteger)outputHeight
                     completion:(nullable AwesomeVideoFrameCaptureCompletionBlock)completion {
    [self captureVideoFrameAtPath:inputPath
                      timeSeconds:timeSeconds
                       outputPath:outputPath
                        overwrite:overwrite
                      outputWidth:outputWidth
                     outputHeight:outputHeight
                         fastMode:NO
                       completion:completion];
}

- (void)captureVideoFrameAtPath:(NSString *)inputPath
                    timeSeconds:(NSTimeInterval)timeSeconds
                     outputPath:(nullable NSString *)outputPath
                      overwrite:(BOOL)overwrite
                    outputWidth:(NSInteger)outputWidth
                   outputHeight:(NSInteger)outputHeight
                       fastMode:(BOOL)fastMode
                     completion:(nullable AwesomeVideoFrameCaptureCompletionBlock)completion {
    [self captureVideoFramesAtPath:inputPath
                       timeSeconds:@[@(timeSeconds)]
                   outputDirectory:nil
                         overwrite:overwrite
                       outputWidth:outputWidth
                      outputHeight:outputHeight
                         fastMode:fastMode
                        outputPathsResolver:^NSArray<NSString *> *(NSString *resolvedInputPath, NSError **errorOut) {
                            NSString *resolvedOutputPath = awesome_video_frame_capture_resolve_single_output_path(
                                resolvedInputPath,
                                timeSeconds,
                                outputPath,
                                overwrite,
                                errorOut
                            );
                            return resolvedOutputPath.length > 0 ? @[resolvedOutputPath] : @[];
                        }
                        completion:^(NSArray<NSString *> *outputPaths, NSError *error) {
                            if (completion) completion(outputPaths.firstObject, error);
                        }];
}

- (void)captureVideoFramesAtPath:(NSString *)inputPath
                     timeSeconds:(NSArray<NSNumber *> *)timesSeconds
                 outputDirectory:(nullable NSString *)outputDirectory
                       overwrite:(BOOL)overwrite
                      completion:(nullable AwesomeVideoFramesCaptureCompletionBlock)completion {
    [self captureVideoFramesAtPath:inputPath
                       timeSeconds:timesSeconds
                   outputDirectory:outputDirectory
                         overwrite:overwrite
                       outputWidth:0
                      outputHeight:0
                        completion:completion];
}

- (void)captureVideoFramesAtPath:(NSString *)inputPath
                     timeSeconds:(NSArray<NSNumber *> *)timesSeconds
                 outputDirectory:(nullable NSString *)outputDirectory
                       overwrite:(BOOL)overwrite
                     outputWidth:(NSInteger)outputWidth
                    outputHeight:(NSInteger)outputHeight
                      completion:(nullable AwesomeVideoFramesCaptureCompletionBlock)completion {
    [self captureVideoFramesAtPath:inputPath
                       timeSeconds:timesSeconds
                   outputDirectory:outputDirectory
                         overwrite:overwrite
                       outputWidth:outputWidth
                      outputHeight:outputHeight
                          fastMode:NO
                        completion:completion];
}

- (void)captureVideoFramesAtPath:(NSString *)inputPath
                     timeSeconds:(NSArray<NSNumber *> *)timesSeconds
                 outputDirectory:(nullable NSString *)outputDirectory
                       overwrite:(BOOL)overwrite
                     outputWidth:(NSInteger)outputWidth
                    outputHeight:(NSInteger)outputHeight
                        fastMode:(BOOL)fastMode
                      completion:(nullable AwesomeVideoFramesCaptureCompletionBlock)completion {
    [self captureVideoFramesAtPath:inputPath
                       timeSeconds:timesSeconds
                   outputDirectory:outputDirectory
                         overwrite:overwrite
                       outputWidth:outputWidth
                      outputHeight:outputHeight
                          fastMode:fastMode
                outputPathsResolver:^NSArray<NSString *> *(NSString *resolvedInputPath, NSError **errorOut) {
                    return awesome_video_frame_capture_resolve_output_paths(
                        resolvedInputPath,
                        timesSeconds,
                        outputDirectory,
                        overwrite,
                        errorOut
                    );
                }
                        completion:completion];
}

- (void)captureVideoFramesAtPath:(NSString *)inputPath
                     timeSeconds:(NSArray<NSNumber *> *)timesSeconds
                 outputDirectory:(nullable NSString *)outputDirectory
                       overwrite:(BOOL)overwrite
                    outputWidth:(NSInteger)outputWidth
                   outputHeight:(NSInteger)outputHeight
                       fastMode:(BOOL)fastMode
             outputPathsResolver:(NSArray<NSString *> *(^)(NSString *resolvedInputPath, NSError **errorOut))outputPathsResolver
                      completion:(nullable AwesomeVideoFramesCaptureCompletionBlock)completion {
    NSError *inputError = nil;
    NSString *resolvedInputPath = awesome_video_frame_capture_resolve_input_path(inputPath, &inputError);
    if (inputError) {
        if (completion) completion(nil, inputError);
        return;
    }

    if (timesSeconds.count == 0) {
        if (completion) {
            completion(nil, awesome_video_frame_capture_error(
                AwesomeVideoFrameCaptureErrorInvalidArguments,
                @"times must not be empty.",
                nil
            ));
        }
        return;
    }

    for (NSNumber *timeNumber in timesSeconds) {
        const double timeSecondsValue = timeNumber.doubleValue;
        if (!std::isfinite(timeSecondsValue) || timeSecondsValue < 0.0) {
            if (completion) {
                completion(nil, awesome_video_frame_capture_error(
                    AwesomeVideoFrameCaptureErrorInvalidArguments,
                    @"times must contain only valid non-negative values.",
                    nil
                ));
            }
            return;
        }
    }

    if (outputWidth < 0 || outputHeight < 0) {
        if (completion) {
            completion(nil, awesome_video_frame_capture_error(
                AwesomeVideoFrameCaptureErrorInvalidArguments,
                @"outputWidth and outputHeight must be non-negative.",
                nil
            ));
        }
        return;
    }

    if (![self beginTaskIfPossibleWithCompletion:^(NSError *error) {
        if (completion) completion(nil, error);
    }]) {
        return;
    }

    NSError *outputError = nil;
    NSArray<NSString *> *resolvedOutputPaths = outputPathsResolver(resolvedInputPath, &outputError);
    if (outputError || resolvedOutputPaths.count == 0) {
        [self resetTaskState];
        if (completion) {
            completion(nil, outputError ?: awesome_video_frame_capture_error(
                AwesomeVideoFrameCaptureErrorInvalidArguments,
                @"outputPaths must not be empty.",
                nil
            ));
        }
        return;
    }

    for (NSString *resolvedOutputPath in resolvedOutputPaths) {
        if (awesome_video_frame_capture_is_network_url(resolvedInputPath)) break;
        if ([[resolvedInputPath stringByStandardizingPath] isEqualToString:[resolvedOutputPath stringByStandardizingPath]]) {
            [self resetTaskState];
            if (completion) {
                completion(nil, awesome_video_frame_capture_error(
                    AwesomeVideoFrameCaptureErrorInvalidArguments,
                    @"inputPath and outputPath must be different.",
                    nil
                ));
            }
            return;
        }
    }

    @synchronized (self) {
        self.currentOutputPath = resolvedOutputPaths.firstObject;
    }

    __weak typeof(self) weakSelf = self;
    dispatch_async(_workQueue, ^{
        __strong typeof(self) strongSelf = weakSelf;
        if (!strongSelf) return;

        @autoreleasepool {
            std::vector<double> times;
            std::vector<const char *> outputPathPointers;
            times.reserve(timesSeconds.count);
            outputPathPointers.reserve(resolvedOutputPaths.count);

            for (NSNumber *timeNumber in timesSeconds) {
                times.push_back(timeNumber.doubleValue);
            }
            for (NSString *path in resolvedOutputPaths) {
                outputPathPointers.push_back(path.fileSystemRepresentation);
            }
            const char *inputPathPointer = awesome_video_frame_capture_is_network_url(resolvedInputPath)
                ? resolvedInputPath.UTF8String
                : resolvedInputPath.fileSystemRepresentation;

            const int ret = capture_video_frames_to_jpegs_cpp(
                inputPathPointer,
                times.data(),
                outputPathPointers.data(),
                (int)times.size(),
                (int)outputWidth,
                (int)outputHeight,
                fastMode ? 1 : 0,
                &strongSelf->_cancelFlag
            );

            const BOOL cancelled = (strongSelf->_cancelFlag != 0);
            if (cancelled || ret != 0) {
                for (NSString *path in resolvedOutputPaths) {
                    (void)[NSFileManager.defaultManager removeItemAtPath:path error:nil];
                }
            }

            NSError *error = nil;
            if (cancelled) {
                error = awesome_video_frame_capture_error(AwesomeVideoFrameCaptureErrorCancelled, @"Cancelled.", nil);
            } else if (ret != 0) {
                error = awesome_video_frame_capture_error(
                    AwesomeVideoFrameCaptureErrorFFmpegFailed,
                    [NSString stringWithFormat:@"FFmpeg failed (code=%d). Video frame capture requires a valid input video, valid time points, and writable JPEG output paths.", ret],
                    @(ret)
                );
            }

            dispatch_async(dispatch_get_main_queue(), ^{
                [strongSelf resetTaskState];
                if (completion) completion(error ? nil : resolvedOutputPaths, error);
            });
        }
    });
}

- (void)cancelCurrentTask {
    @synchronized (self) {
        if (!self.isCapturingVideoFrames) return;
        _cancelFlag = 1;
    }
}

@end
