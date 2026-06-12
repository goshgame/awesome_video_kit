package com.goshlive.awesome_video_kit

import android.content.Context
import android.os.Handler
import android.os.Looper
import java.io.File
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicInteger

internal class VideoTranscodeManager private constructor(private val appContext: Context) {
  class TranscodeError(val code: String, message: String) : Exception(message)

  @Volatile var isTranscoding: Boolean = false
    private set

  @Volatile var isPaused: Boolean = false
    private set

  @Volatile var currentOutputPath: String? = null
    private set

  private val lock = Any()
  private val mainHandler = Handler(Looper.getMainLooper())
  private val executor =
    Executors.newSingleThreadExecutor { runnable ->
      Thread(runnable, "awesome_video_kit_transcode").apply {
        uncaughtExceptionHandler =
          Thread.UncaughtExceptionHandler { _, _ ->
            synchronized(lock) {
              isTranscoding = false
              isPaused = false
              cancelRequested = false
              currentOutputPath = null
            }
          }
      }
    }

  @Volatile private var cancelRequested: Boolean = false

  fun transcodeToMp4(
    inputPath: String,
    outputPath: String?,
    overwrite: Boolean,
    options: AwesomeVideoKitManagerJni.TranscodeOptions =
      AwesomeVideoKitManagerJni.TranscodeOptions(),
    progress: ((Int) -> Unit)?,
    completion: (outputPath: String?, error: TranscodeError?) -> Unit,
  ) {
    val trimmedInputPath = inputPath.trim()
    if (trimmedInputPath.isEmpty()) {
      postCompletion(completion, null, TranscodeError("INVALID_ARGS", "inputPath is required."))
      return
    }

    if (!beginTask(completion)) return

    executeTask(
      progress = progress,
      completion = completion,
      prepare = {
        val inputFile = resolveExistingFile(trimmedInputPath, "inputPath")
        val outputFile =
          resolveOutputFile(
            outputPath = outputPath,
            overwrite = overwrite,
            disallowedPaths = setOf(inputFile.absolutePath),
          )
        val normalizedOptions =
          options.copy(
            preset = options.preset.normalizeOptionalArg(),
            profile = options.profile.normalizeOptionalArg(),
            level = options.level.normalizeOptionalArg(),
            watermarkImagePath = options.watermarkImagePath.normalizeOptionalArg(),
          )

        PreparedTask(
          outputFile = outputFile,
          action = { callback ->
            AwesomeVideoKitManagerJni.transcode(
              inputPath = inputFile.absolutePath,
              outputPath = outputFile.absolutePath,
              options = normalizedOptions,
              callback = callback,
            )
          },
          errorMessage = { code ->
            FfmpegErrorFormatter.format(
              code,
              watermarkEnabled = normalizedOptions.watermarkImagePath != null,
            )
          },
        )
      },
    )
  }

  fun transcodeToMp4WithConcatImage(
    inputPath: String,
    outputPath: String?,
    overwrite: Boolean,
    options: AwesomeVideoKitManagerJni.ConcatImageOptions,
    progress: ((Int) -> Unit)?,
    completion: (outputPath: String?, error: TranscodeError?) -> Unit,
  ) {
    val trimmedInputPath = inputPath.trim()
    if (trimmedInputPath.isEmpty()) {
      postCompletion(completion, null, TranscodeError("INVALID_ARGS", "inputPath is required."))
      return
    }

    if (options.imageDurationMs <= 0) {
      postCompletion(
        completion,
        null,
        TranscodeError("INVALID_ARGS", "imageDurationMs must be greater than 0."),
      )
      return
    }

    if (!beginTask(completion)) return

    executeTask(
      progress = progress,
      completion = completion,
      prepare = {
        val inputFile = resolveExistingFile(trimmedInputPath, "inputPath")
        val concatImageFile = resolveExistingFile(options.concatImagePath, "concatImagePath")
        val outputFile =
          resolveOutputFile(
            outputPath = outputPath,
            overwrite = overwrite,
            disallowedPaths = setOf(inputFile.absolutePath),
          )
        val normalizedOptions =
          options.copy(concatImagePath = concatImageFile.absolutePath)

        PreparedTask(
          outputFile = outputFile,
          action = { callback ->
            AwesomeVideoKitManagerJni.transcodeWithConcatImage(
              inputPath = inputFile.absolutePath,
              outputPath = outputFile.absolutePath,
              options = normalizedOptions,
              callback = callback,
            )
          },
          errorMessage = { code ->
            "${FfmpegErrorFormatter.format(code)} Concat image mode requires image decoding support, H.264 video encoding, and AAC audio encoding when the source video contains audio."
          },
        )
      },
    )
  }

  fun transcodeMediaWithAudio(
    inputPath: String,
    audioPath: String,
    outputPath: String?,
    overwrite: Boolean,
    options: AwesomeVideoKitManagerJni.MediaWithAudioOptions =
      AwesomeVideoKitManagerJni.MediaWithAudioOptions(),
    progress: ((Int) -> Unit)?,
    completion: (outputPath: String?, error: TranscodeError?) -> Unit,
  ) {
    val trimmedInputPath = inputPath.trim()
    if (trimmedInputPath.isEmpty()) {
      postCompletion(completion, null, TranscodeError("INVALID_ARGS", "inputPath is required."))
      return
    }

    val trimmedAudioPath = audioPath.trim()
    if (trimmedAudioPath.isEmpty()) {
      postCompletion(completion, null, TranscodeError("INVALID_ARGS", "audioPath is required."))
      return
    }

    if (!beginTask(completion)) return

    executeTask(
      progress = progress,
      completion = completion,
      prepare = {
        val inputFile = resolveExistingFile(trimmedInputPath, "inputPath")
        val audioFile = resolveExistingFile(trimmedAudioPath, "audioPath")
        val outputFile =
          resolveOutputFile(
            outputPath = outputPath,
            overwrite = overwrite,
            disallowedPaths = setOf(inputFile.absolutePath, audioFile.absolutePath),
          )

        PreparedTask(
          outputFile = outputFile,
          action = { callback ->
            AwesomeVideoKitManagerJni.transcodeWithSeparateAudio(
              inputPath = inputFile.absolutePath,
              audioPath = audioFile.absolutePath,
              outputPath = outputFile.absolutePath,
              options = options,
              callback = callback,
            )
          },
          errorMessage = { code ->
            "${FfmpegErrorFormatter.format(code)} Separate-audio mode requires a valid visual input, a valid audio input, AAC encoding support when the audio cannot be copied, and H.264 encoding support when the visual input is a still image."
          },
        )
      },
    )
  }

  fun cancelCurrentTask() {
    synchronized(lock) {
      if (!isTranscoding) return
      cancelRequested = true
      isPaused = false
    }
    runCatching {
      AwesomeVideoKitManagerJni.ensureLoaded(appContext)
      AwesomeVideoKitManagerJni.cancelTranscode()
    }
  }

  fun pauseCurrentTask() {
    synchronized(lock) {
      if (!isTranscoding || cancelRequested) return
      isPaused = true
    }
    runCatching {
      AwesomeVideoKitManagerJni.ensureLoaded(appContext)
      AwesomeVideoKitManagerJni.pauseTranscode()
    }
  }

  fun resumeCurrentTask() {
    synchronized(lock) {
      if (!isTranscoding || cancelRequested) return
      isPaused = false
    }
    runCatching {
      AwesomeVideoKitManagerJni.ensureLoaded(appContext)
      AwesomeVideoKitManagerJni.resumeTranscode()
    }
  }

  fun shutdown() {
    cancelCurrentTask()
    executor.shutdownNow()
  }

  private fun beginTask(
    completion: (outputPath: String?, error: TranscodeError?) -> Unit,
  ): Boolean {
    synchronized(lock) {
      if (isTranscoding) {
        postCompletion(
          completion,
          null,
          TranscodeError("BUSY", "A transcode task is already running."),
        )
        return false
      }
      isTranscoding = true
      isPaused = false
      cancelRequested = false
      currentOutputPath = null
    }
    return true
  }

  private fun executeTask(
    progress: ((Int) -> Unit)?,
    completion: (outputPath: String?, error: TranscodeError?) -> Unit,
    prepare: () -> PreparedTask,
  ) {
    executor.execute {
      try {
        AwesomeVideoKitManagerJni.ensureLoaded(appContext)
        val task = prepare()
        synchronized(lock) {
          currentOutputPath = task.outputFile.absolutePath
        }

        val callback = buildProgressCallback(progress)
        val ret = task.action(callback)
        finishTask(
          ret = ret,
          outputFile = task.outputFile,
          completion = completion,
          errorMessage = task.errorMessage,
        )
      } catch (error: TranscodeError) {
        postThrowable(completion, error)
      } catch (throwable: Throwable) {
        postThrowable(completion, throwable)
      }
    }
  }

  private fun finishTask(
    ret: Int,
    outputFile: File,
    completion: (outputPath: String?, error: TranscodeError?) -> Unit,
    errorMessage: (Int) -> String,
  ) {
    val cancelled = cancelRequested && ret != 0
    if (cancelled) {
      runCatching { outputFile.delete() }
    }

    mainHandler.post {
      resetTaskState()

      when {
        cancelled -> completion(null, TranscodeError("CANCELLED", "Cancelled."))
        ret != 0 -> completion(null, TranscodeError("FFMPEG_FAILED", errorMessage(ret)))
        else -> completion(outputFile.absolutePath, null)
      }
    }
  }

  private fun postThrowable(
    completion: (outputPath: String?, error: TranscodeError?) -> Unit,
    throwable: Throwable,
  ) {
    mainHandler.post {
      resetTaskState()
      completion(null, toTranscodeError(throwable))
    }
  }

  private fun toTranscodeError(throwable: Throwable): TranscodeError {
    if (throwable is TranscodeError) return throwable

    val code =
      when (throwable) {
        is UnsatisfiedLinkError -> "NATIVE_LIB_MISSING"
        else -> "NATIVE_ERROR"
      }
    return TranscodeError(code, throwable.message ?: "Unknown error")
  }

  private fun postCompletion(
    completion: (outputPath: String?, error: TranscodeError?) -> Unit,
    outputPath: String?,
    error: TranscodeError?,
  ) {
    mainHandler.post { completion(outputPath, error) }
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

  private fun resolveExistingFile(path: String, argumentName: String): File {
    val file = File(path.trim())
    if (!file.exists() || !file.isFile) {
      throw TranscodeError("INVALID_ARGS", "$argumentName must be an existing file.")
    }
    return file
  }

  private fun resolveOutputFile(
    outputPath: String?,
    overwrite: Boolean,
    disallowedPaths: Set<String>,
  ): File {
    val mp4File =
      try {
        AwesomeManagedFileManager.resolveMp4OutputFile(
          outputPath = outputPath,
          overwrite = overwrite,
          prefix = "ffmpeg_transcode",
          managedDirectory = AwesomeManagedFileManager.managedDirectory(appContext),
        )
      } catch (error: ManagedOutputFileException) {
        throw when (error.kind) {
          ManagedOutputFileException.Kind.CREATE_DIRECTORY_FAILED ->
            TranscodeError(
              "CREATE_DIRECTORY_FAILED",
              error.message ?: "Failed to create output directory.",
            )
          ManagedOutputFileException.Kind.FILE_EXISTS ->
            TranscodeError("FILE_EXISTS", error.message ?: "outputPath already exists.")
        }
      }

    if (disallowedPaths.contains(mp4File.absolutePath)) {
      throw TranscodeError(
        "INVALID_ARGS",
        "inputPath/audioPath and outputPath must be different.",
      )
    }

    return mp4File
  }

  private fun resetTaskState() {
    synchronized(lock) {
      isTranscoding = false
      isPaused = false
      cancelRequested = false
      currentOutputPath = null
    }
  }

  private data class PreparedTask(
    val outputFile: File,
    val action: (AwesomeVideoKitManagerJni.ProgressCallback?) -> Int,
    val errorMessage: (Int) -> String,
  )

  companion object {
    @Volatile private var instance: VideoTranscodeManager? = null

    fun sharedInstance(context: Context): VideoTranscodeManager {
      val appContext = context.applicationContext
      return instance
        ?: synchronized(this) {
          instance ?: VideoTranscodeManager(appContext).also { instance = it }
        }
    }
  }
}

private fun String?.normalizeOptionalArg(): String? = this?.trim()?.takeUnless { it.isEmpty() }
