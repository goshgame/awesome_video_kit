package com.goshlive.awesome_video_kit

import android.content.Context
import io.flutter.embedding.engine.plugins.FlutterPlugin
import io.flutter.plugin.common.EventChannel
import io.flutter.plugin.common.MethodCall
import io.flutter.plugin.common.MethodChannel
import io.flutter.plugin.common.MethodChannel.MethodCallHandler
import io.flutter.plugin.common.MethodChannel.Result

/** FfmpegM3u8DownloadPlugin */
class AwesomeVideoKitPlugin : FlutterPlugin, MethodCallHandler {
  private companion object {
    const val METHOD_CHANNEL_NAME = "awesome_video_kit"
    const val DOWNLOAD_PROGRESS_CHANNEL_NAME = "awesome_video_kit/eventchannel"
    const val TRANSCODE_PROGRESS_CHANNEL_NAME = "awesome_video_kit/transcode_eventchannel"
    const val EXTRACT_AUDIO_PROGRESS_CHANNEL_NAME = "awesome_video_kit/extract_audio_eventchannel"
  }

  private lateinit var channel: MethodChannel
  private lateinit var downloadProgressChannel: EventChannel
  private lateinit var transcodeProgressChannel: EventChannel
  private lateinit var extractAudioProgressChannel: EventChannel
  private var downloadEventSink: EventChannel.EventSink? = null
  private var transcodeEventSink: EventChannel.EventSink? = null
  private var extractAudioEventSink: EventChannel.EventSink? = null

  private lateinit var appContext: Context
  private var downloadManager: VideoDownloadManager? = null
  private var transcodeManager: VideoTranscodeManager? = null
  private var audioExtractManager: AudioExtractManager? = null
  private var frameCaptureManager: VideoFrameCaptureManager? = null

  override fun onAttachedToEngine(flutterPluginBinding: FlutterPlugin.FlutterPluginBinding) {
    appContext = flutterPluginBinding.applicationContext
    downloadManager = VideoDownloadManager.sharedInstance(appContext)
    transcodeManager = VideoTranscodeManager.sharedInstance(appContext)
    audioExtractManager = AudioExtractManager.sharedInstance(appContext)
    frameCaptureManager = VideoFrameCaptureManager.sharedInstance(appContext)
    channel = MethodChannel(flutterPluginBinding.binaryMessenger, METHOD_CHANNEL_NAME)
    channel.setMethodCallHandler(this)

    downloadProgressChannel =
      EventChannel(flutterPluginBinding.binaryMessenger, DOWNLOAD_PROGRESS_CHANNEL_NAME)
    downloadProgressChannel.setStreamHandler(
      object : EventChannel.StreamHandler {
        override fun onListen(arguments: Any?, events: EventChannel.EventSink) {
          downloadEventSink = events
        }

        override fun onCancel(arguments: Any?) {
          downloadEventSink = null
        }
      }
    )

    transcodeProgressChannel =
      EventChannel(flutterPluginBinding.binaryMessenger, TRANSCODE_PROGRESS_CHANNEL_NAME)
    transcodeProgressChannel.setStreamHandler(
      object : EventChannel.StreamHandler {
        override fun onListen(arguments: Any?, events: EventChannel.EventSink) {
          transcodeEventSink = events
        }

        override fun onCancel(arguments: Any?) {
          transcodeEventSink = null
        }
      }
    )

    extractAudioProgressChannel =
      EventChannel(flutterPluginBinding.binaryMessenger, EXTRACT_AUDIO_PROGRESS_CHANNEL_NAME)
    extractAudioProgressChannel.setStreamHandler(
      object : EventChannel.StreamHandler {
        override fun onListen(arguments: Any?, events: EventChannel.EventSink) {
          extractAudioEventSink = events
        }

        override fun onCancel(arguments: Any?) {
          extractAudioEventSink = null
        }
      }
    )
  }

  override fun onMethodCall(call: MethodCall, result: Result) {
    when (call.method) {
      "getPlatformVersion" -> result.success("Android ${android.os.Build.VERSION.RELEASE}")
      "downloadVideo" -> handleDownload(call, result)
      "cancelDownload" -> {
        downloadManager?.cancelCurrentTask()
        result.success(null)
      }
      "isDownloading" -> result.success(downloadManager?.isDownloading ?: false)
      "extractAudio" -> handleExtractAudio(call, result)
      "captureVideoFrame" -> handleCaptureVideoFrame(call, result)
      "captureVideoFrames" -> handleCaptureVideoFrames(call, result)
      "cancelExtractAudio" -> {
        audioExtractManager?.cancelCurrentTask()
        result.success(null)
      }
      "isExtractingAudio" -> result.success(audioExtractManager?.isExtractingAudio ?: false)
      "transcodeVideo" -> handleTranscode(call, result)
      "transcodeVideoWithConcatImage" -> handleConcatImageTranscode(call, result)
      "transcodeMediaWithAudio" -> handleSeparateAudioTranscode(call, result)
      "cancelTranscode" -> {
        transcodeManager?.cancelCurrentTask()
        result.success(null)
      }
      "isTranscoding" -> result.success(transcodeManager?.isTranscoding ?: false)
      "pauseTranscode" -> {
        transcodeManager?.pauseCurrentTask()
        result.success(null)
      }
      "resumeTranscode" -> {
        transcodeManager?.resumeCurrentTask()
        result.success(null)
      }
      "isTranscodePaused" -> result.success(transcodeManager?.isPaused ?: false)
      "getMediaInfo" -> handleGetMediaInfo(call, result)
      "getCurrentDownloadOutputPath" -> result.success(downloadManager?.currentOutputPath)
      "getCurrentTranscodeOutputPath" -> result.success(transcodeManager?.currentOutputPath)
      "getCurrentExtractAudioOutputPath" -> result.success(audioExtractManager?.currentOutputPath)
      else -> result.notImplemented()
    }
  }

  override fun onDetachedFromEngine(binding: FlutterPlugin.FlutterPluginBinding) {
    channel.setMethodCallHandler(null)
    downloadProgressChannel.setStreamHandler(null)
    transcodeProgressChannel.setStreamHandler(null)
    extractAudioProgressChannel.setStreamHandler(null)
    downloadEventSink = null
    transcodeEventSink = null
    extractAudioEventSink = null
    downloadManager?.cancelCurrentTask()
    transcodeManager?.cancelCurrentTask()
    audioExtractManager?.cancelCurrentTask()
    frameCaptureManager?.cancelCurrentTask()
    downloadManager = null
    transcodeManager = null
    audioExtractManager = null
    frameCaptureManager = null
  }

  private fun handleDownload(call: MethodCall, result: Result) {
    val args = call.arguments as? Map<*, *>
    if (args == null) {
      result.error("INVALID_ARGS", "Arguments must be a map.", null)
      return
    }

    val url = (args["url"] as? String)?.trim().orEmpty()
    if (url.isEmpty()) {
      result.error("INVALID_ARGS", "Missing url.", null)
      return
    }

    val outputPath = optionalStringArg(args, "outputPath")
    val overwrite = booleanArg(args, "overwrite", true)
    val watermarkImagePath = optionalStringArg(args, "watermarkImagePath")
    val watermarkPosition =
      intArg(args, "watermarkPosition", AwesomeVideoKitManagerJni.WatermarkPosition.BOTTOM_RIGHT)

    val mgr = downloadManager
    if (mgr == null) {
      result.error("NO_MANAGER", "Plugin not attached to engine.", null)
      return
    }

    mgr.downloadM3u8ToMp4(
      url = url,
      outputPath = outputPath,
      overwrite = overwrite,
      watermarkImagePath = watermarkImagePath,
      watermarkPosition = watermarkPosition,
      progress = { p -> downloadEventSink?.success(p) },
      completion = { out, err ->
        runCatching {
          if (err != null) {
            result.error(err.code, err.message, null)
          } else if (out.isNullOrEmpty()) {
            result.error("NO_OUTPUT", "Native download returned empty outputPath.", null)
          } else {
            result.success(out)
          }
        }
      },
    )
  }

  private fun handleTranscode(call: MethodCall, result: Result) {
    val args = call.arguments as? Map<*, *>
    if (args == null) {
      result.error("INVALID_ARGS", "Arguments must be a map.", null)
      return
    }

    val inputPath = optionalStringArg(args, "inputPath").orEmpty()
    if (inputPath.isEmpty()) {
      result.error("INVALID_ARGS", "Missing inputPath.", null)
      return
    }

    val mgr = transcodeManager
    if (mgr == null) {
      result.error("NO_MANAGER", "Plugin not attached to engine.", null)
      return
    }

    val options =
      AwesomeVideoKitManagerJni.TranscodeOptions(
        crf = intArg(args, "crf", 0),
        preset = optionalStringArg(args, "preset"),
        profile = optionalStringArg(args, "profile"),
        level = optionalStringArg(args, "level"),
        videoWidth = intArg(args, "videoWidth", 0),
        videoHeight = intArg(args, "videoHeight", 0),
        audioBitrate = intArg(args, "audioBitrate", 0),
        faststart = booleanArg(args, "faststart", true),
        frameRate = intArg(args, "frameRate", 0),
        watermarkImagePath = optionalStringArg(args, "watermarkImagePath"),
        watermarkPosition =
          intArg(args, "watermarkPosition", AwesomeVideoKitManagerJni.WatermarkPosition.BOTTOM_RIGHT),
      )

    mgr.transcodeToMp4(
      inputPath = inputPath,
      outputPath = optionalStringArg(args, "outputPath"),
      overwrite = booleanArg(args, "overwrite", true),
      options = options,
      progress = { p -> transcodeEventSink?.success(p) },
      completion = { out, err ->
        runCatching {
          if (err != null) {
            result.error(err.code, err.message, null)
          } else if (out.isNullOrEmpty()) {
            result.error("NO_OUTPUT", "Native transcode returned empty outputPath.", null)
          } else {
            result.success(out)
          }
        }
      },
    )
  }

  private fun handleExtractAudio(call: MethodCall, result: Result) {
    val args = call.arguments as? Map<*, *>
    if (args == null) {
      result.error("INVALID_ARGS", "Arguments must be a map.", null)
      return
    }

    val inputPath = optionalStringArg(args, "inputPath").orEmpty()
    if (inputPath.isEmpty()) {
      result.error("INVALID_ARGS", "Missing inputPath.", null)
      return
    }

    val mgr = audioExtractManager
    if (mgr == null) {
      result.error("NO_MANAGER", "Plugin not attached to engine.", null)
      return
    }

    mgr.extractAudio(
      inputPath = inputPath,
      outputPath = optionalStringArg(args, "outputPath"),
      overwrite = booleanArg(args, "overwrite", true),
      progress = { p -> extractAudioEventSink?.success(p) },
      completion = { out, err ->
        runCatching {
          if (err != null) {
            result.error(err.code, err.message, null)
          } else if (out.isNullOrEmpty()) {
            result.error("NO_OUTPUT", "Native extractAudio returned empty outputPath.", null)
          } else {
            result.success(out)
          }
        }
      },
    )
  }

  private fun handleCaptureVideoFrame(call: MethodCall, result: Result) {
    val args = call.arguments as? Map<*, *>
    if (args == null) {
      result.error("INVALID_ARGS", "Arguments must be a map.", null)
      return
    }

    val inputPath = optionalStringArg(args, "inputPath").orEmpty()
    if (inputPath.isEmpty()) {
      result.error("INVALID_ARGS", "Missing inputPath.", null)
      return
    }

    val timeSeconds = doubleArg(args, "timeSeconds", Double.NaN)
    if (timeSeconds.isNaN() || timeSeconds < 0.0 || timeSeconds.isInfinite()) {
      result.error("INVALID_ARGS", "timeSeconds must be a valid non-negative value.", null)
      return
    }

    val mgr = frameCaptureManager
    if (mgr == null) {
      result.error("NO_MANAGER", "Plugin not attached to engine.", null)
      return
    }

    mgr.captureImage(
      inputPath = inputPath,
      timeSeconds = timeSeconds,
      outputPath = optionalStringArg(args, "outputPath"),
      overwrite = booleanArg(args, "overwrite", true),
      outputWidth = intArg(args, "outputWidth", 0),
      outputHeight = intArg(args, "outputHeight", 0),
      fastMode = booleanArg(args, "fastMode", false),
      completion = { out, err ->
        runCatching {
          if (err != null) {
            result.error(err.code, err.message, null)
          } else if (out.isNullOrEmpty()) {
            result.error("NO_OUTPUT", "Native captureVideoFrame returned empty outputPath.", null)
          } else {
            result.success(out)
          }
        }
      },
    )
  }

  private fun handleCaptureVideoFrames(call: MethodCall, result: Result) {
    val args = call.arguments as? Map<*, *>
    if (args == null) {
      result.error("INVALID_ARGS", "Arguments must be a map.", null)
      return
    }

    val inputPath = optionalStringArg(args, "inputPath").orEmpty()
    if (inputPath.isEmpty()) {
      result.error("INVALID_ARGS", "Missing inputPath.", null)
      return
    }

    val timesSeconds = doubleListArg(args, "timesSeconds")
    if (timesSeconds.isEmpty()) {
      result.error("INVALID_ARGS", "timesSeconds must not be empty.", null)
      return
    }
    if (timesSeconds.any { it < 0.0 || it.isNaN() || it.isInfinite() }) {
      result.error("INVALID_ARGS", "timesSeconds must contain only valid non-negative values.", null)
      return
    }

    val mgr = frameCaptureManager
    if (mgr == null) {
      result.error("NO_MANAGER", "Plugin not attached to engine.", null)
      return
    }

    mgr.captureImages(
      inputPath = inputPath,
      timesSeconds = timesSeconds,
      outputDirectory = optionalStringArg(args, "outputDirectory"),
      overwrite = booleanArg(args, "overwrite", true),
      outputWidth = intArg(args, "outputWidth", 0),
      outputHeight = intArg(args, "outputHeight", 0),
      fastMode = booleanArg(args, "fastMode", false),
      completion = { out, err ->
        runCatching {
          if (err != null) {
            result.error(err.code, err.message, null)
          } else if (out.isNullOrEmpty()) {
            result.error("NO_OUTPUT", "Native captureVideoFrames returned empty outputPaths.", null)
          } else {
            result.success(out)
          }
        }
      },
    )
  }

  private fun handleConcatImageTranscode(call: MethodCall, result: Result) {
    val args = call.arguments as? Map<*, *>
    if (args == null) {
      result.error("INVALID_ARGS", "Arguments must be a map.", null)
      return
    }

    val inputPath = optionalStringArg(args, "inputPath").orEmpty()
    if (inputPath.isEmpty()) {
      result.error("INVALID_ARGS", "Missing inputPath.", null)
      return
    }

    val concatImagePath = optionalStringArg(args, "concatImagePath").orEmpty()
    if (concatImagePath.isEmpty()) {
      result.error("INVALID_ARGS", "Missing concatImagePath.", null)
      return
    }

    val imageDurationMs = longArg(args, "imageDurationMs", 0L)
    if (imageDurationMs <= 0L) {
      result.error("INVALID_ARGS", "imageDurationMs must be greater than 0.", null)
      return
    }

    val mgr = transcodeManager
    if (mgr == null) {
      result.error("NO_MANAGER", "Plugin not attached to engine.", null)
      return
    }

    val options =
      AwesomeVideoKitManagerJni.ConcatImageOptions(
        concatImagePath = concatImagePath,
        imageDurationMs = imageDurationMs,
        concatPosition =
          intArg(args, "concatPosition", AwesomeVideoKitManagerJni.ConcatImagePosition.TAIL),
        watermarkImagePath = optionalStringArg(args, "watermarkImagePath"),
        watermarkPosition =
          intArg(args, "watermarkPosition", AwesomeVideoKitManagerJni.WatermarkPosition.BOTTOM_RIGHT),
      )

    mgr.transcodeToMp4WithConcatImage(
      inputPath = inputPath,
      outputPath = optionalStringArg(args, "outputPath"),
      overwrite = booleanArg(args, "overwrite", true),
      options = options,
      progress = { p -> transcodeEventSink?.success(p) },
      completion = { out, err ->
        runCatching {
          if (err != null) {
            result.error(err.code, err.message, null)
          } else if (out.isNullOrEmpty()) {
            result.error(
              "NO_OUTPUT",
              "Native transcodeVideoWithConcatImage returned empty outputPath.",
              null,
            )
          } else {
            result.success(out)
          }
        }
      },
    )
  }

  private fun handleSeparateAudioTranscode(call: MethodCall, result: Result) {
    val args = call.arguments as? Map<*, *>
    if (args == null) {
      result.error("INVALID_ARGS", "Arguments must be a map.", null)
      return
    }

    val inputPath = optionalStringArg(args, "inputPath").orEmpty()
    if (inputPath.isEmpty()) {
      result.error("INVALID_ARGS", "Missing inputPath.", null)
      return
    }

    val audioPath = optionalStringArg(args, "audioPath").orEmpty()
    if (audioPath.isEmpty()) {
      result.error("INVALID_ARGS", "Missing audioPath.", null)
      return
    }

    val mgr = transcodeManager
    if (mgr == null) {
      result.error("NO_MANAGER", "Plugin not attached to engine.", null)
      return
    }

    val options =
      AwesomeVideoKitManagerJni.MediaWithAudioOptions(
        audioBitrate = intArg(args, "audioBitrate", 0),
        faststart = booleanArg(args, "faststart", true),
        frameRate = intArg(args, "frameRate", 0),
      )

    mgr.transcodeMediaWithAudio(
      inputPath = inputPath,
      audioPath = audioPath,
      outputPath = optionalStringArg(args, "outputPath"),
      overwrite = booleanArg(args, "overwrite", true),
      options = options,
      progress = { p -> transcodeEventSink?.success(p) },
      completion = { out, err ->
        runCatching {
          if (err != null) {
            result.error(err.code, err.message, null)
          } else if (out.isNullOrEmpty()) {
            result.error(
              "NO_OUTPUT",
              "Native transcodeMediaWithAudio returned empty outputPath.",
              null,
            )
          } else {
            result.success(out)
          }
        }
      },
    )
  }

  private fun handleGetMediaInfo(call: MethodCall, result: Result) {
    val args = call.arguments as? Map<*, *>
    if (args == null) {
      result.error("INVALID_ARGS", "Arguments must be a map.", null)
      return
    }

    val filePath = optionalStringArg(args, "filePath").orEmpty()
    if (filePath.isEmpty()) {
      result.error("INVALID_ARGS", "Missing filePath.", null)
      return
    }

    try {
      AwesomeVideoKitManagerJni.ensureLoaded(appContext)
      result.success(AwesomeVideoKitManagerJni.inspectMediaInfoJson(filePath))
    } catch (t: Throwable) {
      val code =
        when (t) {
          is IllegalArgumentException -> "INVALID_ARGS"
          is IllegalStateException -> "LOAD_FAILED"
          is UnsatisfiedLinkError -> "NATIVE_LIB_MISSING"
          else -> "NATIVE_ERROR"
        }
      result.error(code, t.message ?: "Failed to load media info.", null)
    }
  }

  private fun optionalStringArg(args: Map<*, *>, key: String): String? =
    (args[key] as? String)?.trim()?.takeUnless { it.isEmpty() }

  private fun intArg(args: Map<*, *>, key: String, defaultValue: Int): Int =
    when (val value = args[key]) {
      is Int -> value
      is Long -> value.toInt()
      is Float -> value.toInt()
      is Double -> value.toInt()
      is String -> value.toIntOrNull() ?: defaultValue
      else -> defaultValue
    }

  private fun booleanArg(args: Map<*, *>, key: String, defaultValue: Boolean): Boolean =
    when (val value = args[key]) {
      is Boolean -> value
      is Number -> value.toInt() != 0
      is String -> value.toBooleanStrictOrNull() ?: defaultValue
      else -> defaultValue
    }

  private fun longArg(args: Map<*, *>, key: String, defaultValue: Long): Long =
    when (val value = args[key]) {
      is Int -> value.toLong()
      is Long -> value
      is Float -> value.toLong()
      is Double -> value.toLong()
      is String -> value.toLongOrNull() ?: defaultValue
      else -> defaultValue
    }

  private fun doubleArg(args: Map<*, *>, key: String, defaultValue: Double): Double =
    when (val value = args[key]) {
      is Int -> value.toDouble()
      is Long -> value.toDouble()
      is Float -> value.toDouble()
      is Double -> value
      is String -> value.toDoubleOrNull() ?: defaultValue
      else -> defaultValue
    }

  private fun doubleListArg(args: Map<*, *>, key: String): List<Double> {
    val values = args[key] as? List<*> ?: return emptyList()
    return values.mapNotNull { value ->
      when (value) {
        is Int -> value.toDouble()
        is Long -> value.toDouble()
        is Float -> value.toDouble()
        is Double -> value
        is String -> value.toDoubleOrNull()
        else -> null
      }
    }
  }
}
