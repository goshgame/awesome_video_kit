//
//  ViewController.m
//  HLSFFmpeg
//
//  Created by dev on 2026/1/14.
//

#import "ViewController.h"
#import <AwesomeVideoKitSDK/AwesomeVideoDownloadManager.h>
#import <AwesomeVideoKitSDK/AwesomeVideoFrameCaptureManager.h>
#import <AwesomeVideoKitSDK/AwesomeVideoKitManager.h>
#import <AwesomeVideoKitSDK/AwesomeVideoTranscoderManager.h>

static NSString * const AwesomeVideoDemoDownloadURL = @"https://samplelib.com/lib/preview/mp4/sample-5s.mp4";

typedef NS_ENUM(NSInteger, AwesomeVideoDemoAction) {
    AwesomeVideoDemoActionCaptureSingleFrame = 0,
    AwesomeVideoDemoActionCaptureFrames,
    AwesomeVideoDemoActionDownload,
    AwesomeVideoDemoActionTranscode,
    AwesomeVideoDemoActionReplaceAudio,
    AwesomeVideoDemoActionExtractAudio,
};

@interface ViewController ()

@property (weak, nonatomic) IBOutlet UILabel *progressLabel;
@property (strong, nonatomic) UISegmentedControl *demoSelector;
@property (strong, nonatomic) UIImageView *capturedFrameImageView;

@end

@implementation ViewController

- (void)viewDidLoad {
    [super viewDidLoad];

    self.progressLabel.numberOfLines = 0;
    self.progressLabel.textAlignment = NSTextAlignmentCenter;

    [self setupDemoSelector];
    [self setupCapturedFrameImageView];
    [self resetPreviewWithStatus:@"选择示例后点击按钮运行"];
}

- (void)setupDemoSelector {
    self.demoSelector = [[UISegmentedControl alloc] initWithItems:@[@"单帧", @"多帧", @"下载", @"转码", @"合音", @"抽音"]];
    self.demoSelector.translatesAutoresizingMaskIntoConstraints = NO;
    self.demoSelector.selectedSegmentIndex = AwesomeVideoDemoActionDownload;
    [self.view addSubview:self.demoSelector];

    UILayoutGuide *safeArea = self.view.safeAreaLayoutGuide;
    [NSLayoutConstraint activateConstraints:@[
        [self.demoSelector.leadingAnchor constraintEqualToAnchor:safeArea.leadingAnchor constant:16.0],
        [self.demoSelector.trailingAnchor constraintEqualToAnchor:safeArea.trailingAnchor constant:-16.0],
        [self.demoSelector.topAnchor constraintEqualToAnchor:safeArea.topAnchor constant:24.0],
    ]];
}

- (void)setupCapturedFrameImageView {
    self.capturedFrameImageView = [[UIImageView alloc] initWithFrame:CGRectZero];
    self.capturedFrameImageView.translatesAutoresizingMaskIntoConstraints = NO;
    self.capturedFrameImageView.contentMode = UIViewContentModeScaleAspectFit;
    self.capturedFrameImageView.clipsToBounds = YES;
    self.capturedFrameImageView.backgroundColor = [UIColor secondarySystemBackgroundColor];
    [self.view addSubview:self.capturedFrameImageView];

    UILayoutGuide *safeArea = self.view.safeAreaLayoutGuide;
    [NSLayoutConstraint activateConstraints:@[
        [self.capturedFrameImageView.leadingAnchor constraintEqualToAnchor:safeArea.leadingAnchor constant:16.0],
        [self.capturedFrameImageView.trailingAnchor constraintEqualToAnchor:safeArea.trailingAnchor constant:-16.0],
        [self.capturedFrameImageView.topAnchor constraintEqualToAnchor:self.progressLabel.bottomAnchor constant:24.0],
        [self.capturedFrameImageView.heightAnchor constraintEqualToAnchor:self.capturedFrameImageView.widthAnchor multiplier:9.0 / 16.0],
    ]];
}

- (IBAction)downloadAction:(id)sender {
    [self runSelectedDemoAction];
}

- (void)runSelectedDemoAction {
    [self resetPreviewWithStatus:@"任务开始..."];

    switch ((AwesomeVideoDemoAction)self.demoSelector.selectedSegmentIndex) {
        case AwesomeVideoDemoActionCaptureSingleFrame:
            [self captureSingleFrameDemo];
            break;
        case AwesomeVideoDemoActionCaptureFrames:
            [self captureFramesDemo];
            break;
        case AwesomeVideoDemoActionDownload:
            [self downloadVideoDemo];
            break;
        case AwesomeVideoDemoActionTranscode:
            [self transcodeVideoDemo];
            break;
        case AwesomeVideoDemoActionReplaceAudio:
            [self replaceAudioDemo];
            break;
        case AwesomeVideoDemoActionExtractAudio:
            [self extractAudioDemo];
            break;
    }
}

- (void)captureSingleFrameDemo {
    NSString *inputPath = [self bundledResourcePathWithName:@"ISHADANCING" type:@"mp4"];
    if (!inputPath) return;

    __weak typeof(self) weakSelf = self;
    [[AwesomeVideoFrameCaptureManager sharedInstance] captureVideoFrameAtPath:inputPath
                                                                  timeSeconds:8.0
                                                                   outputPath:nil
                                                                    overwrite:YES
                                                                  outputWidth:360
                                                                 outputHeight:0
                                                                     fastMode:YES
                                                                   completion:^(NSString *_Nullable outputPath, NSError *_Nullable error) {
        __strong typeof(weakSelf) self = weakSelf;
        if (!self) return;

        UIImage *image = [self imageFromOutputPath:outputPath error:error emptyMessage:@"单帧截取失败"];
        if (!image) return;

        self.capturedFrameImageView.image = image;
        [self updateStatus:@"单帧截取完成"];
        NSLog(@"单帧截取完成：%@", outputPath);
    }];
}

- (void)captureFramesDemo {
    NSString *inputPath = [self bundledResourcePathWithName:@"ISHADANCING" type:@"mp4"];
    if (!inputPath) return;

    NSArray<NSNumber *> *times = @[@1.0, @3.0, @5.0, @8.0];
    __weak typeof(self) weakSelf = self;
    [[AwesomeVideoFrameCaptureManager sharedInstance] captureVideoFramesAtPath:inputPath
                                                                   timeSeconds:times
                                                               outputDirectory:nil
                                                                     overwrite:YES
                                                                   outputWidth:360
                                                                  outputHeight:0
                                                                      fastMode:YES
                                                                    completion:^(NSArray<NSString *> *_Nullable outputPaths, NSError *_Nullable error) {
        __strong typeof(weakSelf) self = weakSelf;
        if (!self) return;

        NSArray<UIImage *> *images = [self imagesFromOutputPaths:outputPaths error:error];
        if (images.count == 0) return;

        self.capturedFrameImageView.image = images.firstObject;
        self.capturedFrameImageView.animationImages = images;
        self.capturedFrameImageView.animationDuration = MAX(images.count * 0.5, 1.0);
        self.capturedFrameImageView.animationRepeatCount = 0;
        [self.capturedFrameImageView startAnimating];

        [self updateStatus:[NSString stringWithFormat:@"多帧截取完成：%lu 张", (unsigned long)images.count]];
        NSLog(@"多帧截取完成：%@", outputPaths);
    }];
}

- (void)downloadVideoDemo {
    __weak typeof(self) weakSelf = self;
    [[AwesomeVideoDownloadManager sharedInstance] downloadVideoToMp4WithURL:AwesomeVideoDemoDownloadURL
                                                                 outputPath:nil
                                                                  overwrite:YES
                                                         watermarkImagePath:nil
                                                          watermarkPosition:VideoWatermarkPositionBottomRight
                                                                   progress:^(NSInteger progress) {
        [weakSelf updateProgress:progress prefix:@"下载"];
    } completion:^(NSString *_Nullable outputPath, NSError *_Nullable error) {
        __strong typeof(weakSelf) self = weakSelf;
        if (!self) return;

        if (error || outputPath.length == 0) {
            [self handleOutputPath:outputPath error:error successPrefix:@"下载完成" failureText:@"下载失败"];
            return;
        }

        [self handleOutputPath:outputPath error:nil successPrefix:@"下载完成" failureText:@"下载失败"];
        [self previewVideoAtPath:outputPath];
    }];
}

- (void)transcodeVideoDemo {
    NSString *inputPath = [self bundledResourcePathWithName:@"ISHADANCING" type:@"mp4"];
    NSString *watermarkPath = [self bundledResourcePathWithName:@"watermar" type:@"png"];
    if (!inputPath || !watermarkPath) return;

    AwesomeVideoTranscodeOptions *options = [AwesomeVideoTranscodeOptions new];
    options.overwrite = @YES;
    options.crf = @23;
    options.videoWidth = @720;
    options.faststart = @YES;
    options.watermarkImagePath = watermarkPath;
    options.watermarkPosition = @(VideoWatermarkPositionBottomRight);

    __weak typeof(self) weakSelf = self;
    [[AwesomeVideoTranscoderManager sharedInstance] transcodeVideoAtPath:inputPath
                                                              outputPath:nil
                                                                 options:options
                                                                progress:^(NSInteger progress) {
        [weakSelf updateProgress:progress prefix:@"转码"];
    } completion:^(NSString *_Nullable outputPath, NSError *_Nullable error) {
        [weakSelf handleOutputPath:outputPath error:error successPrefix:@"转码完成" failureText:@"转码失败"];
    }];
}

- (void)replaceAudioDemo {
    NSString *inputPath = [self bundledResourcePathWithName:@"ISHADANCING" type:@"mp4"];
    NSString *audioPath = [self bundledResourcePathWithName:@"bgm" type:@"mp3"];
    if (!inputPath || !audioPath) return;

    AwesomeVideoSeparateAudioOptions *options = [AwesomeVideoSeparateAudioOptions new];
    options.overwrite = @YES;
    options.audioBitrate = @128000;
    options.faststart = @YES;

    __weak typeof(self) weakSelf = self;
    [[AwesomeVideoTranscoderManager sharedInstance] transcodeMediaAtPath:inputPath
                                                               audioPath:audioPath
                                                              outputPath:nil
                                                                 options:options
                                                                progress:^(NSInteger progress) {
        [weakSelf updateProgress:progress prefix:@"音频合成"];
    } completion:^(NSString *_Nullable outputPath, NSError *_Nullable error) {
        [weakSelf handleOutputPath:outputPath error:error successPrefix:@"音频合成完成" failureText:@"音频合成失败"];
    }];
}

- (void)extractAudioDemo {
    NSString *inputPath = [self bundledResourcePathWithName:@"ISHADANCING" type:@"mp4"];
    if (!inputPath) return;

    __weak typeof(self) weakSelf = self;
    [[AwesomeVideoKitManager sharedInstance] extractAudioFromVideoAtPath:inputPath
                                                              outputPath:nil
                                                               overwrite:YES
                                                                progress:^(NSInteger progress) {
        [weakSelf updateProgress:progress prefix:@"抽取音频"];
    } completion:^(NSString *_Nullable outputPath, NSError *_Nullable error) {
        [weakSelf handleOutputPath:outputPath error:error successPrefix:@"抽取音频完成" failureText:@"抽取音频失败"];
    }];
}

- (void)previewVideoAtPath:(NSString *)videoPath {
    __weak typeof(self) weakSelf = self;
    [[AwesomeVideoFrameCaptureManager sharedInstance] captureVideoFrameAtPath:videoPath
                                                                  timeSeconds:1.0
                                                                   outputPath:nil
                                                                    overwrite:YES
                                                                  outputWidth:360
                                                                 outputHeight:0
                                                                     fastMode:YES
                                                                   completion:^(NSString *_Nullable outputPath, NSError *_Nullable error) {
        __strong typeof(weakSelf) self = weakSelf;
        if (!self) return;

        UIImage *image = [self imageFromOutputPath:outputPath error:error emptyMessage:@"下载成功，预览截帧失败"];
        if (!image) return;

        self.capturedFrameImageView.image = image;
        [self updateStatus:@"下载完成，预览已生成"];
        NSLog(@"下载视频预览截帧完成：%@", outputPath);
    }];
}

- (NSString *)bundledResourcePathWithName:(NSString *)name type:(NSString *)type {
    NSString *path = [[NSBundle mainBundle] pathForResource:name ofType:type];
    if (path.length == 0) {
        NSString *message = [NSString stringWithFormat:@"缺少资源：%@.%@", name, type];
        [self updateStatus:message];
        NSLog(@"%@", message);
        return nil;
    }
    return path;
}

- (UIImage *)imageFromOutputPath:(NSString *)outputPath error:(NSError *)error emptyMessage:(NSString *)emptyMessage {
    if (error || outputPath.length == 0) {
        [self updateStatus:emptyMessage];
        NSLog(@"%@：%@", emptyMessage, error);
        return nil;
    }

    UIImage *image = [UIImage imageWithContentsOfFile:outputPath];
    if (!image) {
        [self updateStatus:@"图片读取失败"];
        NSLog(@"图片读取失败：%@", outputPath);
    }
    return image;
}

- (NSArray<UIImage *> *)imagesFromOutputPaths:(NSArray<NSString *> *)outputPaths error:(NSError *)error {
    if (error || outputPaths.count == 0) {
        [self updateStatus:@"多帧截取失败"];
        NSLog(@"多帧截取失败：%@", error);
        return @[];
    }

    NSMutableArray<UIImage *> *images = [NSMutableArray arrayWithCapacity:outputPaths.count];
    for (NSString *path in outputPaths) {
        UIImage *image = [UIImage imageWithContentsOfFile:path];
        if (image) {
            [images addObject:image];
        }
    }

    if (images.count == 0) {
        [self updateStatus:@"多帧图片读取失败"];
        NSLog(@"多帧图片读取失败：%@", outputPaths);
    }
    return images;
}

- (void)handleOutputPath:(NSString *)outputPath
                   error:(NSError *)error
           successPrefix:(NSString *)successPrefix
             failureText:(NSString *)failureText {
    if (error || outputPath.length == 0) {
        [self updateStatus:failureText];
        NSLog(@"%@：%@", failureText, error);
        return;
    }

    NSString *fileName = outputPath.lastPathComponent.length > 0 ? outputPath.lastPathComponent : outputPath;
    [self updateStatus:[NSString stringWithFormat:@"%@：%@", successPrefix, fileName]];
    NSLog(@"%@：%@", successPrefix, outputPath);
}

- (void)updateProgress:(NSInteger)progress prefix:(NSString *)prefix {
    [self updateStatus:[NSString stringWithFormat:@"%@进度：%ld%%", prefix, (long)progress]];
}

- (void)resetPreviewWithStatus:(NSString *)status {
    [self.capturedFrameImageView stopAnimating];
    self.capturedFrameImageView.animationImages = nil;
    self.capturedFrameImageView.image = nil;
    [self updateStatus:status];
}

- (void)updateStatus:(NSString *)status {
    dispatch_async(dispatch_get_main_queue(), ^{
        self.progressLabel.text = status;
    });
}

@end
