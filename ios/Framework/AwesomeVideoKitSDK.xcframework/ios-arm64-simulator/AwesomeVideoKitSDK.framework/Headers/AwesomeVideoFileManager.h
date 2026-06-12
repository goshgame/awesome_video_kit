//
//  AwesomeVideoFileManager.h
//  AwesomeVideoKitSDK
//
//  Created by Codex on 2026/3/31.
//

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

FOUNDATION_EXPORT NSErrorDomain const AwesomeVideoFileManagerErrorDomain;

typedef NS_ENUM(NSInteger, AwesomeVideoFileManagerErrorCode) {
    AwesomeVideoFileManagerErrorResolveDirectoryFailed = 1,
    AwesomeVideoFileManagerErrorCreateDirectoryFailed = 2,
    AwesomeVideoFileManagerErrorListFilesFailed = 3,
    AwesomeVideoFileManagerErrorInvalidManagedVideoPath = 4,
    AwesomeVideoFileManagerErrorRemoveFailed = 5,
};

@interface AwesomeVideoFileManager : NSObject

+ (instancetype)sharedInstance;

// SDK 在未传 outputPath 时生成视频的统一目录。
@property (nonatomic, copy, readonly) NSString *managedVideosDirectoryPath;

// 生成一个受管理的 mp4 输出路径，文件名格式为 prefix_UUID.mp4。
- (nullable NSString *)createManagedVideoOutputPathWithPrefix:(nullable NSString *)prefix
                                                       error:(NSError * _Nullable * _Nullable)error;

// 返回所有受管理的视频路径，按最近修改时间倒序排列。
- (nullable NSArray<NSString *> *)managedVideoPathsWithError:(NSError * _Nullable * _Nullable)error;

// 判断指定路径是否位于受管理目录内。
- (BOOL)isManagedVideoPath:(nullable NSString *)videoPath;

// 删除受管理目录中的单个视频。
- (BOOL)removeManagedVideoAtPath:(NSString *)videoPath
                           error:(NSError * _Nullable * _Nullable)error;

// 删除受管理目录中的全部视频。
- (BOOL)removeAllManagedVideosWithError:(NSError * _Nullable * _Nullable)error;

@end

NS_ASSUME_NONNULL_END
