//
//  AwesomeVideoKitManager.m
//  AwesomeVideoKitSDK
//
//  Created by dev on 2026/3/23.
//

#import "AwesomeVideoKitManager.h"

#import "AwesomeAVFileInfo.h"
#import "ffmpeg_mp4.h"

NSErrorDomain const AwesomeVideoKitErrorDomain = @"awesome_video_kit";

@interface _AwesomeVideoKitProgressContext : NSObject
@property (nonatomic, copy, nullable) AwesomeVideoKitProgressBlock progress;
@property (nonatomic, assign) volatile int *cancelFlag;
@end

@implementation _AwesomeVideoKitProgressContext
@end

@interface AwesomeVideoKitManager ()
@property (atomic, readwrite) BOOL isExtractingAudio;
@property (atomic, readwrite, nullable) NSString *currentOutputPath;
@end

static NSError *awesome_video_kit_error(AwesomeVideoKitErrorCode code, NSString *message, NSNumber *_Nullable ffmpegCode) {
    NSMutableDictionary *userInfo = [@{
        NSLocalizedDescriptionKey: message ?: @"Unknown error",
    } mutableCopy];
    if (ffmpegCode) userInfo[@"ffmpeg_error_code"] = ffmpegCode;
    return [NSError errorWithDomain:AwesomeVideoKitErrorDomain code:code userInfo:userInfo];
}

static void awesome_video_kit_progress_cb(int percentage, void *user_data) {
    _AwesomeVideoKitProgressContext *ctx = (__bridge _AwesomeVideoKitProgressContext *)user_data;
    if (!ctx.progress) return;
    if (ctx.cancelFlag && *(ctx.cancelFlag)) return;

    NSInteger clamped = (NSInteger)percentage;
    if (clamped < 0) clamped = 0;
    if (clamped > 100) clamped = 100;

    dispatch_async(dispatch_get_main_queue(), ^{
        ctx.progress(clamped);
    });
}

static NSString *awesome_video_kit_normalize_path(NSString *_Nullable path) {
    return [[[path ?: @"" stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet]
        stringByExpandingTildeInPath] copy];
}

static NSString *awesome_video_kit_resolve_input_path(NSString *inputPath, NSError **errorOut) {
    NSString *path = awesome_video_kit_normalize_path(inputPath);
    if (path.length == 0) {
        if (errorOut) {
            *errorOut = awesome_video_kit_error(AwesomeVideoKitErrorInvalidArguments, @"inputPath is required.", nil);
        }
        return @"";
    }

    NSFileManager *fileManager = NSFileManager.defaultManager;
    BOOL isDirectory = NO;
    if (![fileManager fileExistsAtPath:path isDirectory:&isDirectory] || isDirectory) {
        if (errorOut) {
            *errorOut = awesome_video_kit_error(
                AwesomeVideoKitErrorInvalidArguments,
                @"inputPath must be an existing file.",
                nil
            );
        }
        return @"";
    }

    return path;
}

static NSString *awesome_video_kit_default_extension_for_codec_name(NSString *_Nullable codecName) {
    NSString *normalizedCodecName = [[codecName ?: @"" lowercaseString] copy];
    if (normalizedCodecName.length == 0) return @"mka";

    if ([normalizedCodecName isEqualToString:@"aac"] || [normalizedCodecName isEqualToString:@"alac"]) {
        return @"m4a";
    }
    if ([normalizedCodecName isEqualToString:@"mp3"]) {
        return @"mp3";
    }
    if ([normalizedCodecName isEqualToString:@"flac"]) {
        return @"flac";
    }
    if ([normalizedCodecName isEqualToString:@"opus"]) {
        return @"opus";
    }
    if ([normalizedCodecName isEqualToString:@"vorbis"]) {
        return @"ogg";
    }
    if ([normalizedCodecName isEqualToString:@"ac3"]) {
        return @"ac3";
    }
    if ([normalizedCodecName hasPrefix:@"pcm_"]) {
        return @"wav";
    }

    return @"mka";
}

static NSString *awesome_video_kit_probe_preferred_output_extension(NSString *inputPath, NSError **errorOut) {
    NSError *infoError = nil;
    AwesomeAVFileInfo *fileInfo = [AwesomeAVFileInfo infoWithFilePath:inputPath error:&infoError];
    if (!fileInfo) {
        if (errorOut) {
            NSString *message = infoError.localizedDescription ?: @"Failed to load media info.";
            NSNumber *ffmpegCode = infoError.userInfo[@"ffmpeg_error_code"];
            *errorOut = awesome_video_kit_error(AwesomeVideoKitErrorFFmpegFailed, message, ffmpegCode);
        }
        return @"";
    }

    if (fileInfo.audioStreamCount == 0) {
        if (errorOut) {
            *errorOut = awesome_video_kit_error(
                AwesomeVideoKitErrorNoAudioStream,
                @"No audio stream found in input file.",
                nil
            );
        }
        return @"";
    }

    return awesome_video_kit_default_extension_for_codec_name([fileInfo getAudioStreamCodecName:0]);
}

static NSString *awesome_video_kit_default_output_file_name(NSString *inputPath, NSString *extension) {
    NSString *baseName = inputPath.lastPathComponent.stringByDeletingPathExtension;
    if (baseName.length == 0) baseName = @"audio_extract";
    return [NSString stringWithFormat:@"%@_audio.%@", baseName, extension];
}

static NSString *awesome_video_kit_resolve_output_path(
    NSString *inputPath,
    NSString *_Nullable outputPath,
    NSString *preferredExtension,
    NSError **errorOut
) {
    NSFileManager *fileManager = NSFileManager.defaultManager;
    NSString *normalizedOutputPath = awesome_video_kit_normalize_path(outputPath);
    NSString *defaultFileName = awesome_video_kit_default_output_file_name(inputPath, preferredExtension);

    if (normalizedOutputPath.length == 0) {
        NSArray<NSString *> *paths = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES);
        NSString *documentsPath = paths.firstObject;
        if (documentsPath.length == 0) {
            if (errorOut) {
                *errorOut = awesome_video_kit_error(
                    AwesomeVideoKitErrorInvalidArguments,
                    @"Failed to resolve the default output directory.",
                    nil
                );
            }
            return @"";
        }
        normalizedOutputPath = [documentsPath stringByAppendingPathComponent:defaultFileName];
    } else {
        BOOL isDirectory = NO;
        const BOOL exists = [fileManager fileExistsAtPath:normalizedOutputPath isDirectory:&isDirectory];
        if ((exists && isDirectory) || [normalizedOutputPath hasSuffix:@"/"]) {
            normalizedOutputPath = [normalizedOutputPath stringByAppendingPathComponent:defaultFileName];
        } else if (normalizedOutputPath.pathExtension.length == 0) {
            normalizedOutputPath = [normalizedOutputPath stringByAppendingPathExtension:preferredExtension];
        }
    }

    NSString *directoryPath = normalizedOutputPath.stringByDeletingLastPathComponent;
    if (directoryPath.length == 0) {
        if (errorOut) {
            *errorOut = awesome_video_kit_error(
                AwesomeVideoKitErrorInvalidArguments,
                @"Invalid outputPath.",
                nil
            );
        }
        return @"";
    }

    NSError *mkdirError = nil;
    if (![fileManager createDirectoryAtPath:directoryPath withIntermediateDirectories:YES attributes:nil error:&mkdirError]) {
        if (errorOut) {
            NSMutableDictionary *userInfo = [@{
                NSLocalizedDescriptionKey: @"Failed to create output directory.",
            } mutableCopy];
            if (mkdirError) userInfo[NSUnderlyingErrorKey] = mkdirError;
            *errorOut = [NSError errorWithDomain:AwesomeVideoKitErrorDomain
                                            code:AwesomeVideoKitErrorCreateDirectoryFailed
                                        userInfo:userInfo];
        }
        return @"";
    }

    return normalizedOutputPath;
}

@implementation AwesomeVideoKitManager {
    dispatch_queue_t _workQueue;
    volatile int _cancelFlag;
}

+ (instancetype)sharedInstance {
    static AwesomeVideoKitManager *instance = nil;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        instance = [[AwesomeVideoKitManager alloc] init];
    });
    return instance;
}

- (instancetype)init {
    self = [super init];
    if (self) {
        _workQueue = dispatch_queue_create("awesome_video_kit.audio_extract", DISPATCH_QUEUE_SERIAL);
        _cancelFlag = 0;
    }
    return self;
}

- (BOOL)beginTaskIfPossibleWithCompletion:(nullable AwesomeVideoKitCompletionBlock)completion {
    NSError *busyError = nil;
    @synchronized (self) {
        if (self.isExtractingAudio) {
            busyError = awesome_video_kit_error(
                AwesomeVideoKitErrorBusy,
                @"An audio extraction task is already running.",
                nil
            );
        } else {
            self.isExtractingAudio = YES;
            self.currentOutputPath = nil;
            _cancelFlag = 0;
        }
    }

    if (busyError) {
        if (completion) completion(nil, busyError);
        return NO;
    }
    return YES;
}

- (void)resetTaskStateLocked {
    self.isExtractingAudio = NO;
    self.currentOutputPath = nil;
    _cancelFlag = 0;
}

- (void)resetTaskState {
    @synchronized (self) {
        [self resetTaskStateLocked];
    }
}

- (void)extractAudioFromVideoAtPath:(NSString *)inputPath
                         outputPath:(nullable NSString *)outputPath
                          overwrite:(BOOL)overwrite
                           progress:(nullable AwesomeVideoKitProgressBlock)progress
                         completion:(nullable AwesomeVideoKitCompletionBlock)completion {
    NSError *inputError = nil;
    NSString *resolvedInputPath = awesome_video_kit_resolve_input_path(inputPath, &inputError);
    if (inputError) {
        if (completion) completion(nil, inputError);
        return;
    }

    NSError *probeError = nil;
    NSString *preferredExtension = awesome_video_kit_probe_preferred_output_extension(resolvedInputPath, &probeError);
    if (probeError) {
        if (completion) completion(nil, probeError);
        return;
    }

    if (![self beginTaskIfPossibleWithCompletion:completion]) return;

    NSError *outputError = nil;
    NSString *resolvedOutputPath = awesome_video_kit_resolve_output_path(
        resolvedInputPath,
        outputPath,
        preferredExtension,
        &outputError
    );
    if (outputError) {
        [self resetTaskState];
        if (completion) completion(nil, outputError);
        return;
    }

    if ([[resolvedInputPath stringByStandardizingPath] isEqualToString:[resolvedOutputPath stringByStandardizingPath]]) {
        [self resetTaskState];
        if (completion) {
            completion(nil, awesome_video_kit_error(
                AwesomeVideoKitErrorInvalidArguments,
                @"inputPath and outputPath must be different.",
                nil
            ));
        }
        return;
    }

    NSFileManager *fileManager = NSFileManager.defaultManager;
    const BOOL outputExists = [fileManager fileExistsAtPath:resolvedOutputPath];
    if (outputExists && !overwrite) {
        [self resetTaskState];
        if (completion) {
            completion(nil, awesome_video_kit_error(
                AwesomeVideoKitErrorFileExists,
                @"outputPath already exists.",
                nil
            ));
        }
        return;
    }
    if (outputExists && overwrite) {
        (void)[fileManager removeItemAtPath:resolvedOutputPath error:nil];
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
            if (!inputPathC || !inputPathC[0] || !outputPathC || !outputPathC[0]) {
                dispatch_async(dispatch_get_main_queue(), ^{
                    @synchronized (strongSelf) {
                        [strongSelf resetTaskStateLocked];
                    }
                    if (completion) {
                        completion(nil, awesome_video_kit_error(
                            AwesomeVideoKitErrorInvalidArguments,
                            @"Failed to convert input/output paths to file system representation.",
                            nil
                        ));
                    }
                });
                return;
            }

            _AwesomeVideoKitProgressContext *ctx = [_AwesomeVideoKitProgressContext new];
            ctx.progress = progress;
            ctx.cancelFlag = &strongSelf->_cancelFlag;

            void *ctxRef = (__bridge_retained void *)ctx;
            const int ret = extract_audio_stream_from_media(
                inputPathC,
                outputPathC,
                progress ? awesome_video_kit_progress_cb : NULL,
                ctxRef,
                &strongSelf->_cancelFlag,
                NULL
            );
            (void)CFBridgingRelease(ctxRef);

            const BOOL cancelled = (strongSelf->_cancelFlag != 0);
            if (cancelled) {
                (void)[fileManager removeItemAtPath:resolvedOutputPath error:nil];
            }

            NSError *error = nil;
            if (cancelled) {
                error = awesome_video_kit_error(AwesomeVideoKitErrorCancelled, @"Cancelled.", nil);
            } else if (ret != 0) {
                NSString *message = [NSString stringWithFormat:@"FFmpeg failed (code=%d).", ret];
                message = [message stringByAppendingString:@" Audio extraction requires a valid input file, an existing audio stream, and a target container compatible with the source audio codec."];
                error = awesome_video_kit_error(AwesomeVideoKitErrorFFmpegFailed, message, @(ret));
                (void)[fileManager removeItemAtPath:resolvedOutputPath error:nil];
            }

            dispatch_async(dispatch_get_main_queue(), ^{
                [strongSelf resetTaskState];
                if (completion) completion(error ? nil : resolvedOutputPath, error);
            });
        }
    });
}

- (void)cancelCurrentTask {
    @synchronized (self) {
        if (!self.isExtractingAudio) return;
        _cancelFlag = 1;
    }
}

@end
