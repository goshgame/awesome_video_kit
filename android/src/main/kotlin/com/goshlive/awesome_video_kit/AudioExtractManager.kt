package com.goshlive.awesome_video_kit

import android.content.Context
import android.os.Handler
import android.os.Looper
import java.io.File
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicInteger
import org.json.JSONObject

internal class AudioExtractManager private constructor(private val appContext: Context) {
  class ExtractAudioError(val code: String, message: String) : Exception(message)

  @Volatile var isExtractingAudio: Boolean = false
    private set

  @Volatile var currentOutputPath: String? = null
    private set

  private val lock = Any()
  private val mainHandler = Handler(Looper.getMainLooper())
  private val executor =
    Executors.newSingleThreadExecutor { runnable ->
      Thread(runnable, "awesome_video_kit_extract_audio").apply {
        uncaughtExceptionHandler =
          Thread.UncaughtExceptionHandler { _, _ ->
            synchronized(lock) {
              isExtractingAudio = false
              cancelRequested = false
              currentOutputPath = null
            }
          }
      }
    }

  @Volatile private var cancelRequested: Boolean = false

  fun extractAudio(
    inputPath: String,
    outputPath: String?,
    overwrite: Boolean,
    progress: ((Int) -> Unit)?,
    completion: (outputPath: String?, error: ExtractAudioError?) -> Unit,
  ) {
    val trimmedInputPath = inputPath.trim()
    if (trimmedInputPath.isEmpty()) {
      postCompletion(
        completion,
        null,
        ExtractAudioError("INVALID_ARGS", "inputPath is required."),
      )
      return
    }

    synchronized(lock) {
      if (isExtractingAudio) {
        postCompletion(
          completion,
          null,
          ExtractAudioError("BUSY", "An audio extraction task is already running."),
        )
        return
      }
      isExtractingAudio = true
      cancelRequested = false
      currentOutputPath = null
    }

    executor.execute {
      try {
        AwesomeVideoKitManagerJni.ensureLoaded(appContext)
        val inputFile = resolveExistingFile(trimmedInputPath, "inputPath")
        val preferredExtension = probePreferredOutputExtension(inputFile.absolutePath)
        val outputFile =
          resolveOutputFile(
            inputPath = inputFile.absolutePath,
            outputPath = outputPath,
            preferredExtension = preferredExtension,
            overwrite = overwrite,
          )

        if (inputFile.absolutePath == outputFile.absolutePath) {
          throw ExtractAudioError(
            "INVALID_ARGS",
            "inputPath and outputPath must be different.",
          )
        }

        synchronized(lock) {
          currentOutputPath = outputFile.absolutePath
        }

        val callback = buildProgressCallback(progress)
        val ret =
          AwesomeVideoKitManagerJni.extractAudio(
            inputPath = inputFile.absolutePath,
            outputPath = outputFile.absolutePath,
            callback = callback,
          )

        val cancelled = cancelRequested
        if (ret != 0 || cancelled) {
          runCatching { outputFile.delete() }
        }

        mainHandler.post {
          resetTaskState()
          when {
            cancelled ->
              completion(null, ExtractAudioError("CANCELLED", "Cancelled."))
            ret != 0 ->
              completion(
                null,
                ExtractAudioError(
                  "FFMPEG_FAILED",
                  "FFmpeg failed (code=$ret). Audio extraction requires a valid input file, an existing audio stream, and a target container compatible with the source audio codec.",
                ),
              )
            else -> completion(outputFile.absolutePath, null)
          }
        }
      } catch (error: ExtractAudioError) {
        postError(completion, error)
      } catch (throwable: Throwable) {
        postError(
          completion,
          when (throwable) {
            is UnsatisfiedLinkError ->
              ExtractAudioError(
                "NATIVE_LIB_MISSING",
                throwable.message ?: "Failed to load native library.",
              )
            else ->
              ExtractAudioError(
                "NATIVE_ERROR",
                throwable.message ?: "Unknown error",
              )
          },
        )
      }
    }
  }

  fun cancelCurrentTask() {
    synchronized(lock) {
      if (!isExtractingAudio) return
      cancelRequested = true
    }
    runCatching {
      AwesomeVideoKitManagerJni.ensureLoaded(appContext)
      AwesomeVideoKitManagerJni.cancelExtractAudio()
    }
  }

  fun shutdown() {
    cancelCurrentTask()
    executor.shutdownNow()
  }

  private fun resolveExistingFile(path: String, argumentName: String): File {
    val file = File(path.trim())
    if (!file.exists() || !file.isFile) {
      throw ExtractAudioError("INVALID_ARGS", "$argumentName must be an existing file.")
    }
    return file
  }

  private fun probePreferredOutputExtension(inputPath: String): String {
    val payload = AwesomeVideoKitManagerJni.inspectMediaInfoJson(inputPath)
    val json = JSONObject(payload)
    if (json.optInt("audioStreamCount", 0) <= 0) {
      throw ExtractAudioError("NO_AUDIO_STREAM", "No audio stream found in input file.")
    }

    val codecName =
      json.optJSONArray("audioStreams")
        ?.optJSONObject(0)
        ?.optString("codecName")
        ?.trim()
        .orEmpty()
    return defaultExtensionForCodecName(codecName)
  }

  private fun defaultExtensionForCodecName(codecName: String): String {
    return when (codecName.lowercase()) {
      "aac", "alac" -> "m4a"
      "mp3" -> "mp3"
      "flac" -> "flac"
      "opus" -> "opus"
      "vorbis" -> "ogg"
      "ac3" -> "ac3"
      else -> if (codecName.startsWith("pcm_")) "wav" else "mka"
    }
  }

  private fun resolveOutputFile(
    inputPath: String,
    outputPath: String?,
    preferredExtension: String,
    overwrite: Boolean,
  ): File {
    return try {
      AwesomeManagedFileManager.resolveAudioOutputFile(
        inputPath = inputPath,
        outputPath = outputPath,
        preferredExtension = preferredExtension,
        overwrite = overwrite,
        managedDirectory = AwesomeManagedFileManager.managedDirectory(appContext),
      )
    } catch (error: ManagedOutputFileException) {
      throw when (error.kind) {
        ManagedOutputFileException.Kind.CREATE_DIRECTORY_FAILED ->
          ExtractAudioError(
            "CREATE_DIRECTORY_FAILED",
            error.message ?: "Failed to create output directory.",
          )
        ManagedOutputFileException.Kind.FILE_EXISTS ->
          ExtractAudioError("FILE_EXISTS", error.message ?: "outputPath already exists.")
      }
    }
  }

  private fun buildProgressCallback(
    progress: ((Int) -> Unit)?,
  ): AwesomeVideoKitManagerJni.ProgressCallback? {
    if (progress == null) return null

    val lastProgress = AtomicInteger(-1)
    return object : AwesomeVideoKitManagerJni.ProgressCallback {
      override fun onProgress(percent: Int) {
        val clamped = percent.coerceIn(0, 100)
        if (lastProgress.getAndSet(clamped) == clamped) return
        mainHandler.post { progress.invoke(clamped) }
      }
    }
  }

  private fun postCompletion(
    completion: (outputPath: String?, error: ExtractAudioError?) -> Unit,
    outputPath: String?,
    error: ExtractAudioError?,
  ) {
    mainHandler.post { completion(outputPath, error) }
  }

  private fun postError(
    completion: (outputPath: String?, error: ExtractAudioError?) -> Unit,
    error: ExtractAudioError,
  ) {
    mainHandler.post {
      resetTaskState()
      completion(null, error)
    }
  }

  private fun resetTaskState() {
    synchronized(lock) {
      isExtractingAudio = false
      cancelRequested = false
      currentOutputPath = null
    }
  }

  companion object {
    @Volatile private var instance: AudioExtractManager? = null

    fun sharedInstance(context: Context): AudioExtractManager {
      val appContext = context.applicationContext
      return instance
        ?: synchronized(this) {
          instance ?: AudioExtractManager(appContext).also { instance = it }
        }
    }
  }
}
