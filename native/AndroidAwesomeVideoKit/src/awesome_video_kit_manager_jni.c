#include <jni.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ffmpeg_hls.h"
#include "ffmpeg_av_file_info_c_api.h"
#include "ffmpeg_mp4.h"
#include "ffmpeg_mp4_concat_image.h"
#include "ffmpeg_mp4_watermark.h"
#include "ffmpeg_video_frame_extractor.h"

static JavaVM *g_jvm = NULL;
static volatile int g_download_cancel_flag = 0;
static volatile int g_extract_audio_cancel_flag = 0;
static volatile int g_capture_video_frames_cancel_flag = 0;
static volatile int g_transcode_cancel_flag = 0;
static volatile int g_transcode_pause_flag = 0;

#if defined(__ANDROID__)
#include <android/log.h>
#define JNI_LOG_TAG "AwesomeVideoKitJNI"
#define JNI_LOGI(...) __android_log_print(ANDROID_LOG_INFO, JNI_LOG_TAG, __VA_ARGS__)
#define JNI_LOGW(...) __android_log_print(ANDROID_LOG_WARN, JNI_LOG_TAG, __VA_ARGS__)
#define JNI_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, JNI_LOG_TAG, __VA_ARGS__)
#else
#define JNI_LOGI(...) fprintf(stderr, "[I] " __VA_ARGS__), fprintf(stderr, "\n")
#define JNI_LOGW(...) fprintf(stderr, "[W] " __VA_ARGS__), fprintf(stderr, "\n")
#define JNI_LOGE(...) fprintf(stderr, "[E] " __VA_ARGS__), fprintf(stderr, "\n")
#endif

typedef struct {
  jobject callback_global;
  jmethodID on_progress;
  volatile int *cancel_flag;
} ProgressCtx;

static void release_utf_chars(JNIEnv *env, jstring value, const char *chars) {
  if (!env || !value || !chars) return;
  (*env)->ReleaseStringUTFChars(env, value, chars);
}

static int read_utf_chars(JNIEnv *env, jstring value, const char **out_chars) {
  if (!out_chars) return -1;
  *out_chars = NULL;

  if (!value) return 0;

  *out_chars = (*env)->GetStringUTFChars(env, value, NULL);
  if (!*out_chars) {
    JNI_LOGE("GetStringUTFChars failed");
    return -1;
  }
  return 0;
}

static void cleanup_progress_ctx(JNIEnv *env, ProgressCtx *ctx) {
  if (!env || !ctx || !ctx->callback_global) return;
  (*env)->DeleteGlobalRef(env, ctx->callback_global);
  ctx->callback_global = NULL;
}

static void throw_java_exception(JNIEnv *env, const char *class_name, const char *message) {
  if (!env || !class_name) return;
  jclass exception_class = (*env)->FindClass(env, class_name);
  if (!exception_class) {
    (*env)->ExceptionClear(env);
    return;
  }
  (*env)->ThrowNew(env, exception_class, message ? message : "");
  (*env)->DeleteLocalRef(env, exception_class);
}

static void init_progress_ctx(JNIEnv *env, jobject callback, volatile int *cancel_flag, ProgressCtx *ctx) {
  if (!ctx) return;

  memset(ctx, 0, sizeof(*ctx));
  ctx->cancel_flag = cancel_flag;
  if (!env || !callback) return;

  jclass callback_class = (*env)->GetObjectClass(env, callback);
  if (!callback_class) {
    JNI_LOGW("GetObjectClass(callback) failed, progress disabled");
    return;
  }

  jmethodID method = (*env)->GetMethodID(env, callback_class, "onProgress", "(I)V");
  if (!method) {
    JNI_LOGW("callback missing onProgress(int), progress disabled");
    (*env)->DeleteLocalRef(env, callback_class);
    return;
  }

  jobject callback_global = (*env)->NewGlobalRef(env, callback);
  if (!callback_global) {
    JNI_LOGW("NewGlobalRef(callback) failed, progress disabled");
    (*env)->DeleteLocalRef(env, callback_class);
    return;
  }

  ctx->callback_global = callback_global;
  ctx->on_progress = method;
  (*env)->DeleteLocalRef(env, callback_class);
}

static void progress_cb_trampoline(int percentage, void *user_data) {
  ProgressCtx *ctx = (ProgressCtx *)user_data;
  if (!ctx || !g_jvm || !ctx->callback_global || !ctx->on_progress) return;
  if (ctx->cancel_flag && *ctx->cancel_flag) return;

  JNIEnv *env = NULL;
  int attached = 0;
  jint env_status = (*g_jvm)->GetEnv(g_jvm, (void **)&env, JNI_VERSION_1_6);
  if (env_status == JNI_EDETACHED) {
    if ((*g_jvm)->AttachCurrentThread(g_jvm, &env, NULL) != JNI_OK) {
      JNI_LOGE("AttachCurrentThread failed");
      return;
    }
    attached = 1;
  } else if (env_status != JNI_OK) {
    JNI_LOGE("GetEnv failed (status=%d)", (int)env_status);
    return;
  }

  int clamped = percentage;
  if (clamped < 0) clamped = 0;
  if (clamped > 100) clamped = 100;

  (*env)->CallVoidMethod(env, ctx->callback_global, ctx->on_progress, (jint)clamped);
  if ((*env)->ExceptionCheck(env)) {
    JNI_LOGW("Java progress callback threw, clearing exception");
    (*env)->ExceptionClear(env);
  }

  if (attached) {
    (*g_jvm)->DetachCurrentThread(g_jvm);
  }
}

static void init_watermark_config(
    FFmpegWatermarkConfig *config,
    const char *watermark_path,
    jint watermark_position) {
  if (!config) return;

  memset(config, 0, sizeof(*config));
  config->image_path = watermark_path;
  config->position = (FFmpegWatermarkPosition)watermark_position;
  config->margin_x = 20;
  config->margin_y = 20;
  config->width = 0;
  config->height = 0;
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) {
  (void)reserved;
  g_jvm = vm;
  JNI_LOGI("JNI_OnLoad ok");
  return JNI_VERSION_1_6;
}

JNIEXPORT void JNICALL JNI_OnUnload(JavaVM *vm, void *reserved) {
  (void)vm;
  (void)reserved;
  JNI_LOGI("JNI_OnUnload");
  g_jvm = NULL;
}

JNIEXPORT jint JNICALL
Java_com_goshlive_awesomevideokit_internal_NativeBridge_download(
    JNIEnv *env,
    jclass clazz,
    jstring url,
    jstring outputPath,
    jstring watermarkImagePath,
    jint watermarkPosition,
    jobject callback) {
  (void)clazz;

  if (!env || !url || !outputPath) {
    JNI_LOGE("download: invalid args env=%p url=%p outputPath=%p", env, url, outputPath);
    return -1;
  }

  const char *url_c = NULL;
  const char *output_path_c = NULL;
  const char *watermark_path_c = NULL;
  ProgressCtx progress_ctx = {0};
  int ret = -2;

  if (read_utf_chars(env, url, &url_c) != 0 ||
      read_utf_chars(env, outputPath, &output_path_c) != 0 ||
      read_utf_chars(env, watermarkImagePath, &watermark_path_c) != 0) {
    goto cleanup;
  }

  g_download_cancel_flag = 0;
  init_progress_ctx(env, callback, &g_download_cancel_flag, &progress_ctx);

  if (watermark_path_c && *watermark_path_c) {
    FFmpegWatermarkConfig watermark;
    init_watermark_config(&watermark, watermark_path_c, watermarkPosition);
    ret = download_m3u8_to_mp4_with_watermark(
        url_c,
        output_path_c,
        &watermark,
        progress_ctx.callback_global ? progress_cb_trampoline : NULL,
        progress_ctx.callback_global ? &progress_ctx : NULL,
        &g_download_cancel_flag);
  } else {
    ret = download_m3u8_to_mp4(
        url_c,
        output_path_c,
        progress_ctx.callback_global ? progress_cb_trampoline : NULL,
        progress_ctx.callback_global ? &progress_ctx : NULL,
        &g_download_cancel_flag);
  }

cleanup:
  cleanup_progress_ctx(env, &progress_ctx);
  release_utf_chars(env, watermarkImagePath, watermark_path_c);
  release_utf_chars(env, outputPath, output_path_c);
  release_utf_chars(env, url, url_c);
  return (jint)ret;
}

JNIEXPORT void JNICALL
Java_com_goshlive_awesomevideokit_internal_NativeBridge_cancelDownload(JNIEnv *env, jclass clazz) {
  (void)env;
  (void)clazz;
  g_download_cancel_flag = 1;
  JNI_LOGI("cancelDownload: set cancel flag");
}

JNIEXPORT jint JNICALL
Java_com_goshlive_awesomevideokit_internal_NativeBridge_extractAudio(
    JNIEnv *env,
    jclass clazz,
    jstring inputPath,
    jstring outputPath,
    jobject callback) {
  (void)clazz;

  if (!env || !inputPath || !outputPath) {
    JNI_LOGE("extractAudio: invalid args env=%p inputPath=%p outputPath=%p", env, inputPath, outputPath);
    return -1;
  }

  const char *input_path_c = NULL;
  const char *output_path_c = NULL;
  ProgressCtx progress_ctx = {0};
  int ret = -2;

  if (read_utf_chars(env, inputPath, &input_path_c) != 0 ||
      read_utf_chars(env, outputPath, &output_path_c) != 0) {
    goto cleanup;
  }

  g_extract_audio_cancel_flag = 0;
  init_progress_ctx(env, callback, &g_extract_audio_cancel_flag, &progress_ctx);

  ret = extract_audio_stream_from_media(
      input_path_c,
      output_path_c,
      progress_ctx.callback_global ? progress_cb_trampoline : NULL,
      progress_ctx.callback_global ? &progress_ctx : NULL,
      &g_extract_audio_cancel_flag,
      NULL);

cleanup:
  cleanup_progress_ctx(env, &progress_ctx);
  release_utf_chars(env, outputPath, output_path_c);
  release_utf_chars(env, inputPath, input_path_c);
  return (jint)ret;
}

JNIEXPORT void JNICALL
Java_com_goshlive_awesomevideokit_internal_NativeBridge_cancelExtractAudio(JNIEnv *env, jclass clazz) {
  (void)env;
  (void)clazz;
  g_extract_audio_cancel_flag = 1;
  JNI_LOGI("cancelExtractAudio: set cancel flag");
}

JNIEXPORT jint JNICALL
Java_com_goshlive_awesomevideokit_internal_NativeBridge_captureVideoFrames(
    JNIEnv *env,
    jclass clazz,
    jstring inputPath,
    jdoubleArray timesSeconds,
    jobjectArray outputPaths,
    jint outputWidth,
    jint outputHeight,
    jboolean fastMode) {
  (void)clazz;

  if (!env || !inputPath || !timesSeconds || !outputPaths) {
    JNI_LOGE(
        "captureVideoFrames: invalid args env=%p inputPath=%p timesSeconds=%p outputPaths=%p",
        env,
        inputPath,
        timesSeconds,
        outputPaths);
    return -1;
  }

  if (outputWidth < 0 || outputHeight < 0) {
    JNI_LOGE("captureVideoFrames: output dimensions must be non-negative width=%d height=%d", (int)outputWidth, (int)outputHeight);
    return -1;
  }

  jsize count = (*env)->GetArrayLength(env, timesSeconds);
  if (count <= 0 || count != (*env)->GetArrayLength(env, outputPaths)) {
    JNI_LOGE("captureVideoFrames: times/outputPaths length mismatch count=%d", (int)count);
    return -1;
  }

  const char *input_path_c = NULL;
  jdouble *times_c = NULL;
  jstring *output_path_strings = NULL;
  const char **output_paths_c = NULL;
  int ret = -2;

  if (read_utf_chars(env, inputPath, &input_path_c) != 0) {
    goto cleanup;
  }

  times_c = (*env)->GetDoubleArrayElements(env, timesSeconds, NULL);
  if (!times_c) {
    JNI_LOGE("captureVideoFrames: GetDoubleArrayElements failed");
    ret = -3;
    goto cleanup;
  }

  output_path_strings = (jstring *)calloc((size_t)count, sizeof(jstring));
  output_paths_c = (const char **)calloc((size_t)count, sizeof(const char *));
  if (!output_path_strings || !output_paths_c) {
    ret = -4;
    goto cleanup;
  }

  for (jsize index = 0; index < count; ++index) {
    output_path_strings[index] = (jstring)(*env)->GetObjectArrayElement(env, outputPaths, index);
    if (!output_path_strings[index]) {
      JNI_LOGE("captureVideoFrames: output path at index %d is null", (int)index);
      ret = -1;
      goto cleanup;
    }
    if (read_utf_chars(env, output_path_strings[index], &output_paths_c[index]) != 0) {
      ret = -3;
      goto cleanup;
    }
  }

  g_capture_video_frames_cancel_flag = 0;
  ret = capture_video_frames_to_jpegs_cpp(
      input_path_c,
      (const double *)times_c,
      output_paths_c,
      (int)count,
      (int)outputWidth,
      (int)outputHeight,
      fastMode ? 1 : 0,
      &g_capture_video_frames_cancel_flag);

cleanup:
  if (output_path_strings && output_paths_c) {
    for (jsize index = 0; index < count; ++index) {
      release_utf_chars(env, output_path_strings[index], output_paths_c[index]);
      if (output_path_strings[index]) {
        (*env)->DeleteLocalRef(env, output_path_strings[index]);
      }
    }
  }
  free(output_paths_c);
  free(output_path_strings);
  if (times_c) {
    (*env)->ReleaseDoubleArrayElements(env, timesSeconds, times_c, JNI_ABORT);
  }
  release_utf_chars(env, inputPath, input_path_c);
  return (jint)ret;
}

JNIEXPORT void JNICALL
Java_com_goshlive_awesomevideokit_internal_NativeBridge_cancelCaptureVideoFrames(JNIEnv *env, jclass clazz) {
  (void)env;
  (void)clazz;
  g_capture_video_frames_cancel_flag = 1;
  JNI_LOGI("cancelCaptureVideoFrames: set cancel flag");
}

JNIEXPORT jint JNICALL
Java_com_goshlive_awesomevideokit_internal_NativeBridge_transcode(
    JNIEnv *env,
    jclass clazz,
    jstring inputPath,
    jstring outputPath,
    jint crf,
    jstring preset,
    jstring profile,
    jstring level,
    jint videoWidth,
    jint videoHeight,
    jint audioBitrate,
    jboolean faststart,
    jint frameRate,
    jstring watermarkImagePath,
    jint watermarkPosition,
    jobject callback) {
  (void)clazz;

  if (!env || !inputPath || !outputPath) {
    JNI_LOGE("transcode: invalid args env=%p inputPath=%p outputPath=%p", env, inputPath, outputPath);
    return -1;
  }

  const char *input_path_c = NULL;
  const char *output_path_c = NULL;
  const char *preset_c = NULL;
  const char *profile_c = NULL;
  const char *level_c = NULL;
  const char *watermark_path_c = NULL;
  ProgressCtx progress_ctx = {0};
  int ret = -2;

  if (read_utf_chars(env, inputPath, &input_path_c) != 0 ||
      read_utf_chars(env, outputPath, &output_path_c) != 0 ||
      read_utf_chars(env, preset, &preset_c) != 0 ||
      read_utf_chars(env, profile, &profile_c) != 0 ||
      read_utf_chars(env, level, &level_c) != 0 ||
      read_utf_chars(env, watermarkImagePath, &watermark_path_c) != 0) {
    goto cleanup;
  }

  g_transcode_cancel_flag = 0;
  g_transcode_pause_flag = 0;
  init_progress_ctx(env, callback, &g_transcode_cancel_flag, &progress_ctx);

  FFmpegMp4TranscodeConfig config;
  memset(&config, 0, sizeof(config));
  config.crf = (int)crf;
  config.preset = preset_c;
  config.profile = profile_c;
  config.level = level_c;
  config.audio_bitrate = (int)audioBitrate;
  config.faststart = faststart ? 1 : -1;
  config.frame_rate = (int)frameRate;
  config.scale_width = (int)videoWidth;
  config.scale_height = (int)videoHeight;

  if (watermark_path_c && *watermark_path_c) {
    FFmpegWatermarkConfig watermark;
    init_watermark_config(&watermark, watermark_path_c, watermarkPosition);
    ret = transcode_file_to_mp4_with_watermark(
        input_path_c,
        output_path_c,
        &config,
        &watermark,
        progress_ctx.callback_global ? progress_cb_trampoline : NULL,
        progress_ctx.callback_global ? &progress_ctx : NULL,
        &g_transcode_cancel_flag,
        &g_transcode_pause_flag);
  } else {
    ret = transcode_file_to_mp4(
        input_path_c,
        output_path_c,
        &config,
        progress_ctx.callback_global ? progress_cb_trampoline : NULL,
        progress_ctx.callback_global ? &progress_ctx : NULL,
        &g_transcode_cancel_flag,
        &g_transcode_pause_flag);
  }

cleanup:
  cleanup_progress_ctx(env, &progress_ctx);
  release_utf_chars(env, watermarkImagePath, watermark_path_c);
  release_utf_chars(env, level, level_c);
  release_utf_chars(env, profile, profile_c);
  release_utf_chars(env, preset, preset_c);
  release_utf_chars(env, outputPath, output_path_c);
  release_utf_chars(env, inputPath, input_path_c);
  return (jint)ret;
}

JNIEXPORT jint JNICALL
Java_com_goshlive_awesomevideokit_internal_NativeBridge_transcodeWithConcatImage(
    JNIEnv *env,
    jclass clazz,
    jstring inputPath,
    jstring outputPath,
    jstring concatImagePath,
    jlong imageDurationUs,
    jint concatPosition,
    jstring watermarkImagePath,
    jint watermarkPosition,
    jobject callback) {
  (void)clazz;

  if (!env || !inputPath || !outputPath || !concatImagePath || imageDurationUs <= 0) {
    JNI_LOGE(
        "transcodeWithConcatImage: invalid args env=%p inputPath=%p outputPath=%p concatImagePath=%p imageDurationUs=%lld",
        env,
        inputPath,
        outputPath,
        concatImagePath,
        (long long)imageDurationUs);
    return -1;
  }

  const char *input_path_c = NULL;
  const char *output_path_c = NULL;
  const char *concat_image_path_c = NULL;
  const char *watermark_path_c = NULL;
  ProgressCtx progress_ctx = {0};
  int ret = -2;

  if (read_utf_chars(env, inputPath, &input_path_c) != 0 ||
      read_utf_chars(env, outputPath, &output_path_c) != 0 ||
      read_utf_chars(env, concatImagePath, &concat_image_path_c) != 0 ||
      read_utf_chars(env, watermarkImagePath, &watermark_path_c) != 0) {
    goto cleanup;
  }

  g_transcode_cancel_flag = 0;
  g_transcode_pause_flag = 0;
  init_progress_ctx(env, callback, &g_transcode_cancel_flag, &progress_ctx);

  FFmpegMp4ConcatImageConfig concat_image;
  memset(&concat_image, 0, sizeof(concat_image));
  concat_image.image_path = concat_image_path_c;
  concat_image.image_duration_us = (int64_t)imageDurationUs;
  concat_image.position = (FFmpegConcatImagePosition)concatPosition;

  if (watermark_path_c && *watermark_path_c) {
    FFmpegMp4TranscodeConfig config;
    memset(&config, 0, sizeof(config));

    FFmpegWatermarkConfig watermark;
    init_watermark_config(&watermark, watermark_path_c, watermarkPosition);
    ret = transcode_file_to_mp4_with_watermark_and_concat_image(
        input_path_c,
        output_path_c,
        &config,
        &watermark,
        &concat_image,
        progress_ctx.callback_global ? progress_cb_trampoline : NULL,
        progress_ctx.callback_global ? &progress_ctx : NULL,
        &g_transcode_cancel_flag,
        &g_transcode_pause_flag);
  } else {
    ret = transcode_file_to_mp4_with_concat_image(
        input_path_c,
        output_path_c,
        &concat_image,
        progress_ctx.callback_global ? progress_cb_trampoline : NULL,
        progress_ctx.callback_global ? &progress_ctx : NULL,
        &g_transcode_cancel_flag,
        &g_transcode_pause_flag);
  }

cleanup:
  cleanup_progress_ctx(env, &progress_ctx);
  release_utf_chars(env, watermarkImagePath, watermark_path_c);
  release_utf_chars(env, concatImagePath, concat_image_path_c);
  release_utf_chars(env, outputPath, output_path_c);
  release_utf_chars(env, inputPath, input_path_c);
  return (jint)ret;
}

JNIEXPORT jint JNICALL
Java_com_goshlive_awesomevideokit_internal_NativeBridge_transcodeWithSeparateAudio(
    JNIEnv *env,
    jclass clazz,
    jstring inputPath,
    jstring audioPath,
    jstring outputPath,
    jint audioBitrate,
    jboolean faststart,
    jint frameRate,
    jobject callback) {
  (void)clazz;

  if (!env || !inputPath || !audioPath || !outputPath) {
    JNI_LOGE(
        "transcodeWithSeparateAudio: invalid args env=%p inputPath=%p audioPath=%p outputPath=%p",
        env,
        inputPath,
        audioPath,
        outputPath);
    return -1;
  }

  const char *input_path_c = NULL;
  const char *audio_path_c = NULL;
  const char *output_path_c = NULL;
  ProgressCtx progress_ctx = {0};
  int ret = -2;

  if (read_utf_chars(env, inputPath, &input_path_c) != 0 ||
      read_utf_chars(env, audioPath, &audio_path_c) != 0 ||
      read_utf_chars(env, outputPath, &output_path_c) != 0) {
    goto cleanup;
  }

  g_transcode_cancel_flag = 0;
  g_transcode_pause_flag = 0;
  init_progress_ctx(env, callback, &g_transcode_cancel_flag, &progress_ctx);

  FFmpegMp4TranscodeConfig config;
  memset(&config, 0, sizeof(config));
  config.audio_bitrate = (int)audioBitrate;
  config.faststart = faststart ? 1 : -1;
  config.frame_rate = (int)frameRate;

  ret = transcode_file_with_separate_audio_to_mp4(
      input_path_c,
      audio_path_c,
      output_path_c,
      &config,
      progress_ctx.callback_global ? progress_cb_trampoline : NULL,
      progress_ctx.callback_global ? &progress_ctx : NULL,
      &g_transcode_cancel_flag,
      &g_transcode_pause_flag);

cleanup:
  cleanup_progress_ctx(env, &progress_ctx);
  release_utf_chars(env, outputPath, output_path_c);
  release_utf_chars(env, audioPath, audio_path_c);
  release_utf_chars(env, inputPath, input_path_c);
  return (jint)ret;
}

JNIEXPORT jstring JNICALL
Java_com_goshlive_awesomevideokit_internal_NativeBridge_getMediaInfoJson(
    JNIEnv *env,
    jclass clazz,
    jstring filePath) {
  (void)clazz;

  if (!env || !filePath) {
    throw_java_exception(env, "java/lang/IllegalArgumentException", "filePath is required.");
    return NULL;
  }

  const char *file_path_c = NULL;
  char *json_c = NULL;
  char *error_message_c = NULL;
  int ffmpeg_error_code = 0;
  jstring result = NULL;

  if (read_utf_chars(env, filePath, &file_path_c) != 0) {
    throw_java_exception(env, "java/lang/IllegalStateException", "Failed to read filePath.");
    goto cleanup;
  }

  int ret = ffmpeg_av_file_info_inspect_json(
      file_path_c,
      &json_c,
      &error_message_c,
      &ffmpeg_error_code);

  if (ret == 0 && json_c) {
    result = (*env)->NewStringUTF(env, json_c);
    if (!result) {
      throw_java_exception(env, "java/lang/IllegalStateException", "Failed to create JSON string.");
    }
    goto cleanup;
  }

  if (ret == 1) {
    throw_java_exception(
        env,
        "java/lang/IllegalArgumentException",
        error_message_c ? error_message_c : "filePath is required.");
  } else {
    char message_buffer[256];
    if (ffmpeg_error_code != 0) {
      snprintf(
          message_buffer,
          sizeof(message_buffer),
          "%s (ffmpegCode=%d)",
          error_message_c ? error_message_c : "Failed to load media info.",
          ffmpeg_error_code);
      throw_java_exception(env, "java/lang/IllegalStateException", message_buffer);
    } else {
      throw_java_exception(
          env,
          "java/lang/IllegalStateException",
          error_message_c ? error_message_c : "Failed to load media info.");
    }
  }

cleanup:
  ffmpeg_av_file_info_free_string(error_message_c);
  ffmpeg_av_file_info_free_string(json_c);
  release_utf_chars(env, filePath, file_path_c);
  return result;
}

JNIEXPORT void JNICALL
Java_com_goshlive_awesomevideokit_internal_NativeBridge_cancelTranscode(JNIEnv *env, jclass clazz) {
  (void)env;
  (void)clazz;
  g_transcode_cancel_flag = 1;
  g_transcode_pause_flag = 0;
  JNI_LOGI("cancelTranscode: set cancel flag");
}

JNIEXPORT void JNICALL
Java_com_goshlive_awesomevideokit_internal_NativeBridge_pauseTranscode(JNIEnv *env, jclass clazz) {
  (void)env;
  (void)clazz;
  g_transcode_pause_flag = 1;
  JNI_LOGI("pauseTranscode: set pause flag");
}

JNIEXPORT void JNICALL
Java_com_goshlive_awesomevideokit_internal_NativeBridge_resumeTranscode(JNIEnv *env, jclass clazz) {
  (void)env;
  (void)clazz;
  g_transcode_pause_flag = 0;
  JNI_LOGI("resumeTranscode: clear pause flag");
}
