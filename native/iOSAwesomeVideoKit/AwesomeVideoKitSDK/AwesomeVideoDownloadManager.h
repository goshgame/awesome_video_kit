//
//  AwesomeVideoDownloadManager.h
//  video_download
//
//  Created by dev on 2026/1/13.
//

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

FOUNDATION_EXPORT NSErrorDomain const VideoDownloadErrorDomain;

typedef NS_ENUM(NSInteger, VideoDownloadErrorCode) {
    VideoDownloadErrorInvalidArguments = 1,
    VideoDownloadErrorBusy = 2,
    VideoDownloadErrorFileExists = 3,
    VideoDownloadErrorCreateDirectoryFailed = 4,
    VideoDownloadErrorFFmpegFailed = 5,
    VideoDownloadErrorCancelled = 6,
};

typedef NS_ENUM(NSInteger, VideoWatermarkPosition) {
    VideoWatermarkPositionTopLeft = 0,
    VideoWatermarkPositionTopRight = 1,
    VideoWatermarkPositionBottomLeft = 2,
    VideoWatermarkPositionBottomRight = 3,
    VideoWatermarkPositionCenter = 4,
    // 0-5 秒左上角，5-10 秒右下角，按 5 秒周期循环切换。
    VideoWatermarkPositionAlternatingTopLeftBottomRight = 5,
};

typedef void (^VideoDownloadProgressBlock)(NSInteger progress);
typedef void (^VideoDownloadCompletionBlock)(NSString *_Nullable outputPath, NSError *_Nullable error);

@interface AwesomeVideoDownloadManager : NSObject
+ (instancetype)sharedInstance;

// outputPath 为空时，生成视频会落到 AwesomeVideoFileManager 管理目录。
@property (atomic, readonly) BOOL isDownloading;
@property (atomic, readonly, nullable) NSString *currentOutputPath;

- (void)downloadVideoToMp4WithURL:(NSString *)url
                       outputPath:(nullable NSString *)outputPath
                        overwrite:(BOOL)overwrite
               watermarkImagePath:(nullable NSString *)watermarkImagePath
                watermarkPosition:(VideoWatermarkPosition)watermarkPosition
                         progress:(nullable VideoDownloadProgressBlock)progress
                       completion:(nullable VideoDownloadCompletionBlock)completion;

- (void)cancelCurrentTask;
@end

NS_ASSUME_NONNULL_END
