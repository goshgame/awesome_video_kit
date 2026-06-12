package com.goshlive.awesome_video_kit

import android.content.Context
import android.os.Handler
import android.os.Looper
import java.io.File
import java.net.URI
import java.util.concurrent.Executors

internal class VideoFrameCaptureManager private constructor(private val appContext: Context) {
  class FrameCaptureError(val code: String, message: String) : Exception(message)

  @Volatile var isCapturingVideoFrames: Boolean = false
    private set

  @Volatile var currentOutputPath: String? = null
    private set

  private val lock = Any()
  private val mainHandler = Handler(Looper.getMainLooper())
  private val executor =
    Executors.newSingleThreadExecutor { runnable ->
      Thread(runnable, "awesome_video_kit_capture_frames").apply {
        uncaughtExceptionHandler =
          Thread.UncaughtExceptionHandler { _, _ -> resetTaskState() }
      }
    }

  @Volatile private var cancelRequested: Boolean = false

  fun captureImage(
    inputPath: String,
    timeSeconds: Double,
    outputPath: String?,
    overwrite: Boolean,
    outputWidth: Int = 0,
    outputHeight: Int = 0,
    fastMode: Boolean = false,
    completion: (outputPath: String?, error: FrameCaptureError?) -> Unit,
  ) {
    captureImagesResolved(
      inputPath = inputPath,
      timesSeconds = listOf(timeSeconds),
      outputWidth = outputWidth,
      outputHeight = outputHeight,
      fastMode = fastMode,
      outputFilesResolver = { resolvedInputPath ->
        listOf(
          resolveFrameOutputFile(
            inputPath = resolvedInputPath,
            timeSeconds = timeSeconds,
            outputPath = outputPath,
            overwrite = overwrite,
          )
        )
      },
      completion = { outputPaths, error -> completion(outputPaths?.firstOrNull(), error) },
    )
  }

  fun captureImages(
    inputPath: String,
    timesSeconds: List<Double>,
    outputDirectory: String?,
    overwrite: Boolean,
    outputWidth: Int = 0,
    outputHeight: Int = 0,
    fastMode: Boolean = false,
    completion: (outputPaths: List<String>?, error: FrameCaptureError?) -> Unit,
  ) {
    captureImagesResolved(
      inputPath = inputPath,
      timesSeconds = timesSeconds,
      outputWidth = outputWidth,
      outputHeight = outputHeight,
      fastMode = fastMode,
      outputFilesResolver = { resolvedInputPath ->
        resolveFrameOutputFiles(
          inputPath = resolvedInputPath,
          timesSeconds = timesSeconds,
          outputDirectory = outputDirectory,
          overwrite = overwrite,
        )
      },
      completion = completion,
    )
  }

  fun cancelCurrentTask() {
    synchronized(lock) {
      if (!isCapturingVideoFrames) return
      cancelRequested = true
    }
    runCatching {
      AwesomeVideoKitManagerJni.ensureLoaded(appContext)
      AwesomeVideoKitManagerJni.cancelCaptureVideoFrames()
    }
  }

  fun shutdown() {
    cancelCurrentTask()
    executor.shutdownNow()
  }

  private fun captureImagesResolved(
    inputPath: String,
    timesSeconds: List<Double>,
    outputWidth: Int,
    outputHeight: Int,
    fastMode: Boolean,
    outputFilesResolver: (String) -> List<File>,
    completion: (outputPaths: List<String>?, error: FrameCaptureError?) -> Unit,
  ) {
    val trimmedInputPath = inputPath.trim()
    if (trimmedInputPath.isEmpty()) {
      postCompletion(completion, null, FrameCaptureError("INVALID_ARGS", "inputPath is required."))
      return
    }

    if (timesSeconds.isEmpty()) {
      postCompletion(completion, null, FrameCaptureError("INVALID_ARGS", "times must not be empty."))
      return
    }

    if (timesSeconds.any { it < 0.0 || it.isNaN() || it.isInfinite() }) {
      postCompletion(
        completion,
        null,
        FrameCaptureError("INVALID_ARGS", "times must contain only valid non-negative values."),
      )
      return
    }

    if (outputWidth < 0 || outputHeight < 0) {
      postCompletion(
        completion,
        null,
        FrameCaptureError("INVALID_ARGS", "outputWidth and outputHeight must be non-negative."),
      )
      return
    }

    synchronized(lock) {
      if (isCapturingVideoFrames) {
        postCompletion(
          completion,
          null,
          FrameCaptureError("BUSY", "A video frame capture task is already running."),
        )
        return
      }
      isCapturingVideoFrames = true
      cancelRequested = false
      currentOutputPath = null
    }

    executor.execute {
      try {
        AwesomeVideoKitManagerJni.ensureLoaded(appContext)
        val resolvedInputPath = resolveInputPath(trimmedInputPath, "inputPath")
        val outputFiles = outputFilesResolver(resolvedInputPath)

        if (outputFiles.isEmpty()) {
          throw FrameCaptureError("INVALID_ARGS", "outputPaths must not be empty.")
        }

        if (!isNetworkUrl(resolvedInputPath)) {
          outputFiles.forEach { outputFile ->
            if (resolvedInputPath == outputFile.absolutePath) {
              throw FrameCaptureError("INVALID_ARGS", "inputPath and outputPath must be different.")
            }
          }
        }

        synchronized(lock) {
          currentOutputPath = outputFiles.firstOrNull()?.absolutePath
        }

        val ret =
          AwesomeVideoKitManagerJni.captureVideoFrames(
            inputPath = resolvedInputPath,
            timesSeconds = timesSeconds.toDoubleArray(),
            outputPaths = outputFiles.map { it.absolutePath }.toTypedArray(),
            outputWidth = outputWidth,
            outputHeight = outputHeight,
            fastMode = fastMode,
          )

        val cancelled = cancelRequested
        if (ret != 0 || cancelled) {
          outputFiles.forEach { runCatching { it.delete() } }
        }

        mainHandler.post {
          resetTaskState()
          when {
            cancelled ->
              completion(null, FrameCaptureError("CANCELLED", "Cancelled."))
            ret != 0 ->
              completion(
                null,
                FrameCaptureError(
                  "FFMPEG_FAILED",
                  "FFmpeg failed (code=$ret). Video frame capture requires a valid input video, valid time points, and writable JPEG output paths.",
                ),
              )
            else -> completion(outputFiles.map { it.absolutePath }, null)
          }
        }
      } catch (error: FrameCaptureError) {
        postError(completion, error)
      } catch (throwable: Throwable) {
        postError(
          completion,
          when (throwable) {
            is UnsatisfiedLinkError ->
              FrameCaptureError(
                "NATIVE_LIB_MISSING",
                throwable.message ?: "Failed to load native library.",
              )
            else ->
              FrameCaptureError(
                "NATIVE_ERROR",
                throwable.message ?: "Unknown error",
              )
          },
        )
      }
    }
  }

  private fun resolveInputPath(path: String, argumentName: String): String {
    if (isNetworkUrl(path)) return path

    val file = File(path.trim())
    if (!file.exists() || !file.isFile) {
      throw FrameCaptureError("INVALID_ARGS", "$argumentName must be an existing file.")
    }
    return file.absolutePath
  }

  private fun isNetworkUrl(path: String): Boolean {
    val uri = runCatching { URI(path.trim()) }.getOrNull() ?: return false
    val scheme = uri.scheme?.lowercase() ?: return false
    return (scheme == "http" || scheme == "https") && !uri.host.isNullOrBlank()
  }

  private fun resolveFrameOutputFile(
    inputPath: String,
    timeSeconds: Double,
    outputPath: String?,
    overwrite: Boolean,
  ): File {
    return try {
      AwesomeManagedFileManager.resolveFrameOutputFile(
        inputPath = inputPath,
        timeSeconds = timeSeconds,
        outputPath = outputPath,
        overwrite = overwrite,
        managedDirectory = AwesomeManagedFileManager.managedDirectory(appContext),
      )
    } catch (error: ManagedOutputFileException) {
      frameCaptureErrorFromManagedError(error)
    }
  }

  private fun resolveFrameOutputFiles(
    inputPath: String,
    timesSeconds: List<Double>,
    outputDirectory: String?,
    overwrite: Boolean,
  ): List<File> {
    return try {
      AwesomeManagedFileManager.resolveFrameOutputFiles(
        inputPath = inputPath,
        timesSeconds = timesSeconds,
        outputDirectory = outputDirectory,
        overwrite = overwrite,
        managedDirectory = AwesomeManagedFileManager.managedDirectory(appContext),
      )
    } catch (error: ManagedOutputFileException) {
      frameCaptureErrorFromManagedError(error)
    }
  }

  private fun frameCaptureErrorFromManagedError(error: ManagedOutputFileException): Nothing {
    throw when (error.kind) {
      ManagedOutputFileException.Kind.CREATE_DIRECTORY_FAILED ->
        FrameCaptureError(
          "CREATE_DIRECTORY_FAILED",
          error.message ?: "Failed to create output directory.",
        )
      ManagedOutputFileException.Kind.FILE_EXISTS ->
        FrameCaptureError("FILE_EXISTS", error.message ?: "outputPath already exists.")
    }
  }

  private fun postCompletion(
    completion: (outputPaths: List<String>?, error: FrameCaptureError?) -> Unit,
    outputPaths: List<String>?,
    error: FrameCaptureError?,
  ) {
    mainHandler.post { completion(outputPaths, error) }
  }

  private fun postError(
    completion: (outputPaths: List<String>?, error: FrameCaptureError?) -> Unit,
    error: FrameCaptureError,
  ) {
    mainHandler.post {
      resetTaskState()
      completion(null, error)
    }
  }

  private fun resetTaskState() {
    synchronized(lock) {
      isCapturingVideoFrames = false
      cancelRequested = false
      currentOutputPath = null
    }
  }

  companion object {
    @Volatile private var instance: VideoFrameCaptureManager? = null

    fun sharedInstance(context: Context): VideoFrameCaptureManager =
      instance
        ?: synchronized(this) {
          instance ?: VideoFrameCaptureManager(context.applicationContext).also { instance = it }
        }
  }
}
