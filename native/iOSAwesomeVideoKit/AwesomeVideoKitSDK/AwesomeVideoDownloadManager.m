//
//  AwesomeVideoDownloadManager.m
//  video_download
//
//  Created by dev on 2026/1/13.
//

#import "AwesomeVideoDownloadManager.h"
#import "AwesomeVideoFileManager.h"
#import "ffmpeg_hls.h"

NSErrorDomain const VideoDownloadErrorDomain = @"video_download";

@interface _VideoDownloadProgressContext : NSObject
@property (nonatomic, copy, nullable) VideoDownloadProgressBlock progress;
@property (nonatomic, assign) volatile int *cancelFlag;
@end

@implementation _VideoDownloadProgressContext
@end

static FFmpegWatermarkPosition video_download_to_c_watermark_position(VideoWatermarkPosition position) {
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

static void video_download_progress_cb(int percentage, void *user_data) {
    _VideoDownloadProgressContext *ctx = (__bridge _VideoDownloadProgressContext *)user_data;
    if (!ctx.progress) return;
    if (ctx.cancelFlag && *(ctx.cancelFlag)) return;

    NSInteger clamped = (NSInteger)percentage;
    if (clamped < 0) clamped = 0;
    if (clamped > 100) clamped = 100;

    dispatch_async(dispatch_get_main_queue(), ^{
        ctx.progress(clamped);
    });
}

@interface AwesomeVideoDownloadManager ()
@property (atomic, readwrite) BOOL isDownloading;
@property (atomic, readwrite, nullable) NSString *currentOutputPath;
@end

@implementation AwesomeVideoDownloadManager {
    dispatch_queue_t _workQueue;
    volatile int _cancelFlag;
}

static AwesomeVideoDownloadManager *_instance = nil;

+ (instancetype)sharedInstance {
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        _instance = [[AwesomeVideoDownloadManager alloc] init];
    });
    return _instance;
}

- (instancetype)init {
    self = [super init];
    if (self) {
        _workQueue = dispatch_queue_create("video_download.work", DISPATCH_QUEUE_SERIAL);
        _cancelFlag = 0;
    }
    return self;
}

static NSError *video_download_error(VideoDownloadErrorCode code, NSString *message) {
    return [NSError errorWithDomain:VideoDownloadErrorDomain
                               code:code
                           userInfo:@{NSLocalizedDescriptionKey : message ?: @"Unknown error"}];
}

static NSString *video_download_random_file_stem(void) {
    return [NSString stringWithFormat:@"video_%@", [NSUUID UUID].UUIDString];
}

static NSError *video_download_wrap_managed_video_error(NSError *_Nullable error) {
    NSMutableDictionary *userInfo = [@{
        NSLocalizedDescriptionKey : @"Failed to create output directory.",
    } mutableCopy];
    if (error) userInfo[NSUnderlyingErrorKey] = error;
    return [NSError errorWithDomain:VideoDownloadErrorDomain
                               code:VideoDownloadErrorCreateDirectoryFailed
                           userInfo:userInfo];
}

static NSString *video_download_ensure_mp4_extension(NSString *path) {
    if (path.length == 0) return path;
    if ([[path.pathExtension lowercaseString] isEqualToString:@"mp4"]) return path;
    return [path stringByAppendingPathExtension:@"mp4"];
}

static NSString *video_download_resolve_output_path(NSString *_Nullable outputPath, NSError **errorOut) {
    NSFileManager *fm = NSFileManager.defaultManager;

    NSString *path = outputPath;
    if (path) {
        path = [[path stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet]
            stringByExpandingTildeInPath];
    }

    if (!path || path.length == 0) {
        NSError *managedPathError = nil;
        path = [[AwesomeVideoFileManager sharedInstance] createManagedVideoOutputPathWithPrefix:@"video_download"
                                                                                       error:&managedPathError];
        if (path.length == 0) {
            if (errorOut) *errorOut = video_download_wrap_managed_video_error(managedPathError);
            return @"";
        }
    }

    BOOL isDir = NO;
    if ([fm fileExistsAtPath:path isDirectory:&isDir] && isDir) {
        path = [path stringByAppendingPathComponent:video_download_random_file_stem()];
    } else if ([path hasSuffix:@"/"]) {
        NSString *trimmed = [path stringByTrimmingCharactersInSet:[NSCharacterSet characterSetWithCharactersInString:@"/"]];
        path = [trimmed stringByAppendingPathComponent:video_download_random_file_stem()];
    }

    path = video_download_ensure_mp4_extension(path);

    NSString *dir = [path stringByDeletingLastPathComponent];
    if (dir.length == 0) {
        if (errorOut) *errorOut = video_download_error(VideoDownloadErrorInvalidArguments, @"Invalid outputPath.");
        return @"";
    }

    NSError *mkdirErr = nil;
    if (![fm createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:&mkdirErr]) {
        if (errorOut) {
            NSMutableDictionary *info = [@{NSLocalizedDescriptionKey : @"Failed to create output directory."} mutableCopy];
            if (mkdirErr) info[NSUnderlyingErrorKey] = mkdirErr;
            *errorOut = [NSError errorWithDomain:VideoDownloadErrorDomain
                                            code:VideoDownloadErrorCreateDirectoryFailed
                                        userInfo:info];
        }
        return @"";
    }

    return path;
}

- (void)downloadVideoToMp4WithURL:(NSString *)url
                       outputPath:(nullable NSString *)outputPath
                        overwrite:(BOOL)overwrite
                         progress:(nullable VideoDownloadProgressBlock)progress
                       completion:(nullable VideoDownloadCompletionBlock)completion {
    [self downloadVideoToMp4WithURL:url
                         outputPath:outputPath
                          overwrite:overwrite
                watermarkImagePath:nil
                 watermarkPosition:VideoWatermarkPositionBottomRight
                           progress:progress
                         completion:completion];
}

- (void)downloadVideoToMp4WithURL:(NSString *)url
                       outputPath:(nullable NSString *)outputPath
                        overwrite:(BOOL)overwrite
              watermarkImagePath:(nullable NSString *)watermarkImagePath
               watermarkPosition:(VideoWatermarkPosition)watermarkPosition
                         progress:(nullable VideoDownloadProgressBlock)progress
                       completion:(nullable VideoDownloadCompletionBlock)completion {
    NSString *trimmedURL = [[url ?: @"" stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet] copy];
    NSString *trimmedWatermarkPath = nil;
    if (watermarkImagePath.length > 0) {
        trimmedWatermarkPath = [[[watermarkImagePath stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet]
            stringByExpandingTildeInPath] copy];
    }

    if (trimmedURL.length == 0) {
        if (completion) completion(nil, video_download_error(VideoDownloadErrorInvalidArguments, @"url is required."));
        return;
    }

    @synchronized (self) {
        if (self.isDownloading) {
            if (completion) completion(nil, video_download_error(VideoDownloadErrorBusy, @"A download task is already running."));
            return;
        }
        self.isDownloading = YES;
        _cancelFlag = 0;
    }

    NSError *pathErr = nil;
    NSString *resolvedPath = video_download_resolve_output_path(outputPath, &pathErr);
    if (pathErr) {
        @synchronized (self) {
            self.isDownloading = NO;
            self.currentOutputPath = nil;
        }
        if (completion) completion(nil, pathErr);
        return;
    }

    NSFileManager *fm = NSFileManager.defaultManager;
    BOOL exists = [fm fileExistsAtPath:resolvedPath];
    if (exists && !overwrite) {
        @synchronized (self) {
            self.isDownloading = NO;
            self.currentOutputPath = nil;
        }
        if (completion) completion(nil, video_download_error(VideoDownloadErrorFileExists, @"outputPath already exists."));
        return;
    }
    if (exists && overwrite) {
        (void)[fm removeItemAtPath:resolvedPath error:nil];
    }

    self.currentOutputPath = resolvedPath;

    __weak typeof(self) weakSelf = self;
    dispatch_async(_workQueue, ^{
        __strong typeof(self) strongSelf = weakSelf;
        if (!strongSelf) return;

        @autoreleasepool {
            const char *urlC = trimmedURL.UTF8String;
            const char *outC = resolvedPath.fileSystemRepresentation;
            const char *watermarkPathC = trimmedWatermarkPath.fileSystemRepresentation;
            const BOOL needsWatermark = (trimmedWatermarkPath.length > 0 && watermarkPathC && *watermarkPathC);

            if (!urlC || !outC) {
                dispatch_async(dispatch_get_main_queue(), ^{
                    @synchronized (strongSelf) {
                        strongSelf.isDownloading = NO;
                        strongSelf.currentOutputPath = nil;
                        strongSelf->_cancelFlag = 0;
                    }
                    if (completion) {
                        completion(nil, video_download_error(VideoDownloadErrorInvalidArguments, @"Failed to convert arguments to C strings."));
                    }
                });
                return;
            }

            _VideoDownloadProgressContext *ctx = [_VideoDownloadProgressContext new];
            ctx.progress = progress;
            ctx.cancelFlag = &strongSelf->_cancelFlag;

            void *ctxRef = (__bridge_retained void *)ctx;
            int ret = 0;
            if (needsWatermark) {
                FFmpegWatermarkConfig watermark = {0};
                watermark.image_path = watermarkPathC;
                watermark.position = video_download_to_c_watermark_position(watermarkPosition);
                watermark.margin_x = 20;
                watermark.margin_y = 20;

                ret = download_m3u8_to_mp4_with_watermark(
                    urlC,
                    outC,
                    &watermark,
                    progress ? video_download_progress_cb : NULL,
                    ctxRef,
                    &strongSelf->_cancelFlag
                );
            } else {
                ret = download_m3u8_to_mp4(
                    urlC,
                    outC,
                    progress ? video_download_progress_cb : NULL,
                    ctxRef,
                    &strongSelf->_cancelFlag
                );
            }
            (void)CFBridgingRelease(ctxRef);

            const BOOL cancelled = (strongSelf->_cancelFlag != 0);
            if (cancelled) {
                (void)[fm removeItemAtPath:resolvedPath error:nil];
            }

            NSError *err = nil;
            if (cancelled) {
                err = video_download_error(VideoDownloadErrorCancelled, @"Cancelled.");
            } else if (ret != 0) {
                NSString *message = [NSString stringWithFormat:@"FFmpeg failed (code=%d).", ret];
                if (needsWatermark) {
                    message = [message stringByAppendingString:@" Watermark mode requires FFmpeg built with libavfilter/libswscale and image decoding support."];
                }
                err = video_download_error(
                    VideoDownloadErrorFFmpegFailed,
                    message
                );
            }

            dispatch_async(dispatch_get_main_queue(), ^{
                @synchronized (strongSelf) {
                    strongSelf.isDownloading = NO;
                    strongSelf.currentOutputPath = nil;
                    strongSelf->_cancelFlag = 0;
                }
                if (completion) completion(err ? nil : resolvedPath, err);
            });
        }
    });
}

- (void)cancelCurrentTask {
    @synchronized (self) {
        if (!self.isDownloading) return;
        _cancelFlag = 1;
    }
}

@end
