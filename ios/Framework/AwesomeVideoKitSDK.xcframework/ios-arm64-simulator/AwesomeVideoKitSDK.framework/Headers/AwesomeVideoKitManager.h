//
//  AwesomeVideoKitManager.h
//  AwesomeVideoKitSDK
//
//  Created by dev on 2026/3/23.
//

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

FOUNDATION_EXPORT NSErrorDomain const AwesomeVideoKitErrorDomain;

typedef NS_ENUM(NSInteger, AwesomeVideoKitErrorCode) {
    AwesomeVideoKitErrorInvalidArguments = 1,
    AwesomeVideoKitErrorBusy = 2,
    AwesomeVideoKitErrorFileExists = 3,
    AwesomeVideoKitErrorCreateDirectoryFailed = 4,
    AwesomeVideoKitErrorNoAudioStream = 5,
    AwesomeVideoKitErrorFFmpegFailed = 6,
    AwesomeVideoKitErrorCancelled = 7,
};

typedef void (^AwesomeVideoKitProgressBlock)(NSInteger progress);
typedef void (^AwesomeVideoKitCompletionBlock)(NSString *_Nullable outputPath, NSError *_Nullable error);

@interface AwesomeVideoKitManager : NSObject

// 单例入口，内部串行执行抽取任务。
+ (instancetype)sharedInstance;

// 是否存在正在执行中的音频抽取任务。
@property (atomic, readonly) BOOL isExtractingAudio;
// 当前任务输出路径，空表示暂无活跃任务。
@property (atomic, readonly, nullable) NSString *currentOutputPath;

// 从本地视频中提取首路音频流。
// 默认不做重新编码，仅将音频流抽取到音频容器；当 outputPath 为空时，会按源音频编码自动推导输出扩展名。
- (void)extractAudioFromVideoAtPath:(NSString *)inputPath
                         outputPath:(nullable NSString *)outputPath
                          overwrite:(BOOL)overwrite
                           progress:(nullable AwesomeVideoKitProgressBlock)progress
                         completion:(nullable AwesomeVideoKitCompletionBlock)completion;

// 取消当前正在执行的抽取任务。
- (void)cancelCurrentTask;

@end

NS_ASSUME_NONNULL_END
