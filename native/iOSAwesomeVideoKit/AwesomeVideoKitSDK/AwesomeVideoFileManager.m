//
//  AwesomeVideoFileManager.m
//  AwesomeVideoKitSDK
//
//  Created by Codex on 2026/3/31.
//

#import "AwesomeVideoFileManager.h"

NSErrorDomain const AwesomeVideoFileManagerErrorDomain = @"awesome_video_file_manager";

static NSString *const AwesomeManagedVideosDirectoryName = @"AwesomeGeneratedVideos";

static NSError *awesome_video_file_manager_error(
    AwesomeVideoFileManagerErrorCode code,
    NSString *message,
    NSError *_Nullable underlyingError
) {
    NSMutableDictionary *userInfo = [@{
        NSLocalizedDescriptionKey: message ?: @"Unknown error",
    } mutableCopy];
    if (underlyingError) userInfo[NSUnderlyingErrorKey] = underlyingError;
    return [NSError errorWithDomain:AwesomeVideoFileManagerErrorDomain code:code userInfo:userInfo];
}

static NSString *awesome_video_file_manager_normalize_path(NSString *_Nullable path) {
    NSString *trimmedPath = [[path ?: @"" stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet] copy];
    if (trimmedPath.length == 0) return @"";
    return [[[trimmedPath stringByExpandingTildeInPath] stringByStandardizingPath] copy];
}

static NSString *_Nullable awesome_video_file_manager_documents_directory_path(void) {
    NSArray<NSString *> *paths = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES);
    return paths.firstObject;
}

static NSString *awesome_video_file_manager_sanitized_prefix(NSString *_Nullable prefix) {
    NSString *candidate = [[prefix ?: @"" stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet] copy];
    if (candidate.length == 0) return @"video";

    NSCharacterSet *allowedCharacterSet = [NSCharacterSet characterSetWithCharactersInString:
        @"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-"];
    NSMutableString *result = [NSMutableString stringWithCapacity:candidate.length];
    for (NSUInteger index = 0; index < candidate.length; index++) {
        unichar character = [candidate characterAtIndex:index];
        if ([allowedCharacterSet characterIsMember:character]) {
            [result appendFormat:@"%C", character];
        } else {
            [result appendString:@"_"];
        }
    }

    while ([result containsString:@"__"]) {
        [result replaceOccurrencesOfString:@"__"
                                withString:@"_"
                                   options:0
                                     range:NSMakeRange(0, result.length)];
    }

    NSString *sanitized = [result stringByTrimmingCharactersInSet:[NSCharacterSet characterSetWithCharactersInString:@"_"]];
    return sanitized.length > 0 ? sanitized : @"video";
}

static BOOL awesome_video_file_manager_path_is_within_directory(NSString *path, NSString *directoryPath) {
    NSString *normalizedPath = awesome_video_file_manager_normalize_path(path);
    NSString *normalizedDirectory = awesome_video_file_manager_normalize_path(directoryPath);
    if (normalizedPath.length == 0 || normalizedDirectory.length == 0) return NO;
    if ([normalizedPath isEqualToString:normalizedDirectory]) return YES;

    NSString *directoryPrefix = [normalizedDirectory hasSuffix:@"/"] ? normalizedDirectory : [normalizedDirectory stringByAppendingString:@"/"];
    return [normalizedPath hasPrefix:directoryPrefix];
}

@interface AwesomeVideoFileManager ()
@property (nonatomic, copy, readwrite) NSString *managedVideosDirectoryPath;
@end

@implementation AwesomeVideoFileManager

+ (instancetype)sharedInstance {
    static AwesomeVideoFileManager *instance = nil;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        instance = [[AwesomeVideoFileManager alloc] init];
    });
    return instance;
}

- (instancetype)init {
    self = [super init];
    if (self) {
        NSString *documentsPath = awesome_video_file_manager_documents_directory_path();
        _managedVideosDirectoryPath = documentsPath.length > 0
            ? [[documentsPath stringByAppendingPathComponent:AwesomeManagedVideosDirectoryName] copy]
            : @"";
    }
    return self;
}

- (BOOL)ensureManagedVideosDirectoryExistsWithError:(NSError **)error {
    if (self.managedVideosDirectoryPath.length == 0) {
        if (error) {
            *error = awesome_video_file_manager_error(
                AwesomeVideoFileManagerErrorResolveDirectoryFailed,
                @"Failed to resolve the managed videos directory.",
                nil
            );
        }
        return NO;
    }

    NSError *mkdirError = nil;
    BOOL success = [NSFileManager.defaultManager createDirectoryAtPath:self.managedVideosDirectoryPath
                                           withIntermediateDirectories:YES
                                                            attributes:nil
                                                                 error:&mkdirError];
    if (!success) {
        if (error) {
            *error = awesome_video_file_manager_error(
                AwesomeVideoFileManagerErrorCreateDirectoryFailed,
                @"Failed to create the managed videos directory.",
                mkdirError
            );
        }
        return NO;
    }

    return YES;
}

- (nullable NSString *)createManagedVideoOutputPathWithPrefix:(nullable NSString *)prefix
                                                       error:(NSError * _Nullable * _Nullable)error {
    NSError *directoryError = nil;
    if (![self ensureManagedVideosDirectoryExistsWithError:&directoryError]) {
        if (error) *error = directoryError;
        return nil;
    }

    NSString *sanitizedPrefix = awesome_video_file_manager_sanitized_prefix(prefix);
    NSString *fileName = [NSString stringWithFormat:@"%@_%@.mp4", sanitizedPrefix, [NSUUID UUID].UUIDString];
    return [self.managedVideosDirectoryPath stringByAppendingPathComponent:fileName];
}

- (nullable NSArray<NSString *> *)managedVideoPathsWithError:(NSError * _Nullable * _Nullable)error {
    NSError *directoryError = nil;
    if (![self ensureManagedVideosDirectoryExistsWithError:&directoryError]) {
        if (error) *error = directoryError;
        return nil;
    }

    NSArray<NSURLResourceKey> *resourceKeys = @[
        NSURLIsDirectoryKey,
        NSURLContentModificationDateKey,
    ];
    NSError *listError = nil;
    NSArray<NSURL *> *urls = [NSFileManager.defaultManager contentsOfDirectoryAtURL:[NSURL fileURLWithPath:self.managedVideosDirectoryPath]
                                                         includingPropertiesForKeys:resourceKeys
                                                                            options:NSDirectoryEnumerationSkipsHiddenFiles
                                                                              error:&listError];
    if (!urls) {
        if (error) {
            *error = awesome_video_file_manager_error(
                AwesomeVideoFileManagerErrorListFilesFailed,
                @"Failed to list managed videos.",
                listError
            );
        }
        return nil;
    }

    NSArray<NSURL *> *sortedURLs = [urls sortedArrayUsingComparator:^NSComparisonResult(NSURL *lhs, NSURL *rhs) {
        NSNumber *lhsIsDirectory = nil;
        NSNumber *rhsIsDirectory = nil;
        NSDate *lhsDate = nil;
        NSDate *rhsDate = nil;
        [lhs getResourceValue:&lhsIsDirectory forKey:NSURLIsDirectoryKey error:nil];
        [rhs getResourceValue:&rhsIsDirectory forKey:NSURLIsDirectoryKey error:nil];
        [lhs getResourceValue:&lhsDate forKey:NSURLContentModificationDateKey error:nil];
        [rhs getResourceValue:&rhsDate forKey:NSURLContentModificationDateKey error:nil];

        if (lhsIsDirectory.boolValue != rhsIsDirectory.boolValue) {
            return lhsIsDirectory.boolValue ? NSOrderedDescending : NSOrderedAscending;
        }
        NSComparisonResult dateResult = [rhsDate ?: [NSDate distantPast] compare:lhsDate ?: [NSDate distantPast]];
        if (dateResult != NSOrderedSame) return dateResult;
        return [lhs.lastPathComponent compare:rhs.lastPathComponent options:NSCaseInsensitiveSearch];
    }];

    NSMutableArray<NSString *> *paths = [NSMutableArray arrayWithCapacity:sortedURLs.count];
    for (NSURL *url in sortedURLs) {
        NSNumber *isDirectory = nil;
        [url getResourceValue:&isDirectory forKey:NSURLIsDirectoryKey error:nil];
        if (isDirectory.boolValue) continue;
        [paths addObject:url.path];
    }
    return [paths copy];
}

- (BOOL)isManagedVideoPath:(nullable NSString *)videoPath {
    return awesome_video_file_manager_path_is_within_directory(videoPath ?: @"", self.managedVideosDirectoryPath);
}

- (BOOL)removeManagedVideoAtPath:(NSString *)videoPath
                           error:(NSError * _Nullable * _Nullable)error {
    NSString *normalizedPath = awesome_video_file_manager_normalize_path(videoPath);
    NSString *normalizedDirectory = awesome_video_file_manager_normalize_path(self.managedVideosDirectoryPath);
    if (![self isManagedVideoPath:normalizedPath] || [normalizedPath isEqualToString:normalizedDirectory]) {
        if (error) {
            *error = awesome_video_file_manager_error(
                AwesomeVideoFileManagerErrorInvalidManagedVideoPath,
                @"videoPath must be inside the managed videos directory.",
                nil
            );
        }
        return NO;
    }

    NSFileManager *fileManager = NSFileManager.defaultManager;
    if (![fileManager fileExistsAtPath:normalizedPath]) {
        return YES;
    }

    NSError *removeError = nil;
    if (![fileManager removeItemAtPath:normalizedPath error:&removeError]) {
        if (error) {
            *error = awesome_video_file_manager_error(
                AwesomeVideoFileManagerErrorRemoveFailed,
                @"Failed to remove the managed video.",
                removeError
            );
        }
        return NO;
    }

    return YES;
}

- (BOOL)removeAllManagedVideosWithError:(NSError * _Nullable * _Nullable)error {
    NSArray<NSString *> *videoPaths = [self managedVideoPathsWithError:error];
    if (!videoPaths) return NO;

    for (NSString *videoPath in videoPaths) {
        NSError *removeError = nil;
        if (![self removeManagedVideoAtPath:videoPath error:&removeError]) {
            if (error) *error = removeError;
            return NO;
        }
    }

    return YES;
}

@end
