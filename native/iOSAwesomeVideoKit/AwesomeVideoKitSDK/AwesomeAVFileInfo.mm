//
//  AwesomeAVFileInfo.mm
//  AwesomeVideoKitSDK
//
//  Created by dev on 2026/3/23.
//

#import "AwesomeAVFileInfo.h"

#import "ffmpeg_av_file_info.h"

#include <memory>

extern "C" {
#include <libavutil/error.h>
}

NSErrorDomain const AwesomeAVFileInfoErrorDomain = @"awesome_av_file_info";

namespace {

NSString *awesome_av_file_info_string_from_utf8(const char *value) {
    if (!value || !value[0]) return @"";
    return [NSString stringWithUTF8String:value] ?: @"";
}

NSString *awesome_av_file_info_ffmpeg_error_string(int errnum) {
    char error_buffer[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(errnum, error_buffer, sizeof(error_buffer));
    return awesome_av_file_info_string_from_utf8(error_buffer);
}

NSError *awesome_av_file_info_error(AwesomeAVFileInfoErrorCode code, NSString *message, NSNumber *_Nullable ffmpegCode) {
    NSMutableDictionary *userInfo = [@{
        NSLocalizedDescriptionKey: message ?: @"Unknown error",
    } mutableCopy];
    if (ffmpegCode) userInfo[@"ffmpeg_error_code"] = ffmpegCode;
    return [NSError errorWithDomain:AwesomeAVFileInfoErrorDomain code:code userInfo:userInfo];
}

NSString *awesome_av_file_info_normalize_path(NSString *filePath) {
    return [[[filePath ?: @"" stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet]
        stringByExpandingTildeInPath] copy];
}

AwesomeSize awesome_av_file_info_make_size(const FFmpegAVFileInfo::Size &size) {
    return AwesomeSizeMake(size.width, size.height);
}

AwesomeRational awesome_av_file_info_make_rational(const FFmpegAVFileInfo::Rational &rational) {
    return AwesomeRationalMake(rational.num, rational.den);
}

}  // namespace

@implementation AwesomeAVFileInfo {
    std::unique_ptr<FFmpegAVFileInfo> _impl;
}

+ (instancetype)infoWithFilePath:(NSString *)filePath error:(NSError * _Nullable __autoreleasing *)error {
    return [[self alloc] initWithFilePath:filePath error:error];
}

- (instancetype)init {
    self = [super init];
    if (self) {
        _impl = std::make_unique<FFmpegAVFileInfo>();
    }
    return self;
}

- (instancetype)initWithFilePath:(NSString *)filePath error:(NSError * _Nullable __autoreleasing *)error {
    self = [self init];
    if (!self) return nil;
    if (![self loadFromFile:filePath error:error]) return nil;
    return self;
}

- (BOOL)loadFromFile:(NSString *)filePath error:(NSError * _Nullable __autoreleasing *)error {
    NSString *normalizedPath = awesome_av_file_info_normalize_path(filePath);
    if (normalizedPath.length == 0) {
        if (error) {
            *error = awesome_av_file_info_error(
                AwesomeAVFileInfoErrorInvalidArguments,
                @"filePath is required.",
                nil
            );
        }
        return NO;
    }

    const char *filePathC = normalizedPath.fileSystemRepresentation;
    if (!filePathC || !filePathC[0]) {
        if (error) {
            *error = awesome_av_file_info_error(
                AwesomeAVFileInfoErrorInvalidArguments,
                @"Failed to convert filePath to file system representation.",
                nil
            );
        }
        return NO;
    }

    const int ret = _impl->loadFromFile(filePathC);
    if (ret < 0) {
        if (error) {
            NSString *message = [NSString stringWithFormat:@"Failed to load media info: %@.", awesome_av_file_info_ffmpeg_error_string(ret)];
            *error = awesome_av_file_info_error(AwesomeAVFileInfoErrorLoadFailed, message, @(ret));
        }
        return NO;
    }
    return YES;
}

- (AwesomeAVFileType)avFileType {
    return static_cast<AwesomeAVFileType>(_impl->avFileType());
}

- (int64_t)duration {
    return _impl->duration();
}

- (uint64_t)dataRate {
    return _impl->dataRate();
}

- (unsigned int)videoStreamCount {
    return _impl->videoStreamCount();
}

- (unsigned int)audioStreamCount {
    return _impl->audioStreamCount();
}

- (NSString *)sourcePath {
    return awesome_av_file_info_string_from_utf8(_impl->sourcePath().c_str());
}

- (int64_t)getVideoStreamDuration:(unsigned int)videoStreamIndex {
    return _impl->getVideoStreamDuration(videoStreamIndex);
}

- (AwesomeSize)getVideoStreamDimension:(unsigned int)videoStreamIndex {
    return awesome_av_file_info_make_size(_impl->getVideoStreamDimension(videoStreamIndex));
}

- (AwesomeRational)getVideoStreamPixelAspectRatio:(unsigned int)videoStreamIndex {
    return awesome_av_file_info_make_rational(_impl->getVideoStreamPixelAspectRatio(videoStreamIndex));
}

- (AwesomeRational)getVideoStreamFrameRate:(unsigned int)videoStreamIndex {
    return awesome_av_file_info_make_rational(_impl->getVideoStreamFrameRate(videoStreamIndex));
}

- (AwesomeVideoRotation)getVideoStreamRotation:(unsigned int)videoStreamIndex {
    return static_cast<AwesomeVideoRotation>(_impl->getVideoStreamRotation(videoStreamIndex));
}

- (int)getVideoStreamRotationDegrees:(unsigned int)videoStreamIndex {
    return _impl->getVideoStreamRotationDegrees(videoStreamIndex);
}

- (unsigned int)getVideoStreamComponentBitCount:(unsigned int)videoStreamIndex {
    return _impl->getVideoStreamComponentBitCount(videoStreamIndex);
}

- (AwesomeVideoCodecType)getVideoStreamCodecType:(unsigned int)videoStreamIndex {
    return static_cast<AwesomeVideoCodecType>(_impl->getVideoStreamCodecType(videoStreamIndex));
}

- (NSString *)getVideoStreamCodecName:(unsigned int)videoStreamIndex {
    return awesome_av_file_info_string_from_utf8(_impl->getVideoStreamCodecName(videoStreamIndex));
}

- (int)getVideoCodecProfile:(unsigned int)videoStreamIndex {
    return _impl->getVideoCodecProfile(videoStreamIndex);
}

- (int)getVideoCodecLevel:(unsigned int)videoStreamIndex {
    return _impl->getVideoCodecLevel(videoStreamIndex);
}

- (AwesomeVideoColorTransfer)getVideoStreamColorTransfer:(unsigned int)videoStreamIndex {
    return static_cast<AwesomeVideoColorTransfer>(_impl->getVideoStreamColorTransfer(videoStreamIndex));
}

- (AwesomeVideoColorTransfer)getVideoStreamColorTranfer:(unsigned int)videoStreamIndex {
    return [self getVideoStreamColorTransfer:videoStreamIndex];
}

- (AwesomeVideoHDRType)getVideoStreamHDRType:(unsigned int)videoStreamIndex {
    return static_cast<AwesomeVideoHDRType>(_impl->getVideoStreamHDRType(videoStreamIndex));
}

- (int64_t)getAudioStreamDuration:(unsigned int)audioStreamIndex {
    return _impl->getAudioStreamDuration(audioStreamIndex);
}

- (unsigned int)getAudioStreamSampleRate:(unsigned int)audioStreamIndex {
    return _impl->getAudioStreamSampleRate(audioStreamIndex);
}

- (unsigned int)getAudioStreamChannelCount:(unsigned int)audioStreamIndex {
    return _impl->getAudioStreamChannelCount(audioStreamIndex);
}

- (BOOL)getAudioStreamCodecSupport:(unsigned int)audioStreamIndex {
    return _impl->getAudioStreamCodecSupport(audioStreamIndex);
}

- (NSString *)getAudioStreamCodecName:(unsigned int)audioStreamIndex {
    return awesome_av_file_info_string_from_utf8(_impl->getAudioStreamCodecName(audioStreamIndex));
}

@end
