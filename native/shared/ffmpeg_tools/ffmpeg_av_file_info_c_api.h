#ifndef AWESOME_FFMPEG_AV_FILE_INFO_C_API_H
#define AWESOME_FFMPEG_AV_FILE_INFO_C_API_H

#ifdef __cplusplus
extern "C" {
#endif

int ffmpeg_av_file_info_inspect_json(
    const char *file_path,
    char **out_json,
    char **out_error_message,
    int *out_ffmpeg_error_code
);

void ffmpeg_av_file_info_free_string(char *value);

#ifdef __cplusplus
}
#endif

#endif /* AWESOME_FFMPEG_AV_FILE_INFO_C_API_H */
