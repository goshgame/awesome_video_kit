package com.goshlive.awesome_video_kit
import android.content.Context
import android.os.Handler
import android.os.Looper
import java.io.File
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicInteger

internal class VideoDownloadManager private constructor(private val appContext: Context) {
  class DownloadError(val code: String, message: String) : Exception(message)

  @Volatile var isDownloading: Boolean = false
    private set

  @Volatile var currentOutputPath: String? = null
    private set

  private val lock = Any()
  private val mainHandler = Handler(Looper.getMainLooper())
  private val executor =
    Executors.newSingleThreadExecutor { runnable ->
      Thread(runnable, "awesome_video_kit").apply {
        uncaughtExceptionHandler =
          Thread.UncaughtExceptionHandler { _, _ ->
            synchronized(lock) {
              isDownloading = false
              cancelRequested = false
              currentOutputPath = null
            }
          }
      }
    }

  @Volatile private var cancelRequested: Boolean = false

  fun downloadM3u8ToMp4(
    url: String,
    outputPath: String?,
    overwrite: Boolean,
    progress: ((Int) -> Unit)?,
    completion: (outputPath: String?, error: DownloadError?) -> Unit,
  ) {
    downloadM3u8ToMp4(
      url = url,
      outputPath = outputPath,
      overwrite = overwrite,
      watermarkImagePath = null,
      watermarkPosition = AwesomeVideoKitManagerJni.WatermarkPosition.BOTTOM_RIGHT,
      progress = progress,
      completion = completion,
    )
  }

  fun downloadM3u8ToMp4(
    url: String,
    outputPath: String?,
    overwrite: Boolean,
    watermarkImagePath: String?,
    watermarkPosition: Int = AwesomeVideoKitManagerJni.WatermarkPosition.BOTTOM_RIGHT,
    progress: ((Int) -> Unit)?,
    completion: (outputPath: String?, error: DownloadError?) -> Unit,
  ) {
    val trimmedUrl = url.trim()
    if (trimmedUrl.isEmpty()) {
      mainHandler.post { completion(null, DownloadError("INVALID_ARGS", "url is required.")) }
      return
    }

    synchronized(lock) {
      if (isDownloading) {
        mainHandler.post {
          completion(null, DownloadError("BUSY", "A download task is already running."))
        }
        return
      }
      isDownloading = true
      cancelRequested = false
      currentOutputPath = null
    }

    executor.execute {
      try {
        AwesomeVideoKitManagerJni.ensureLoaded(appContext)
        val normalizedWatermarkPath = watermarkImagePath?.trim()?.takeUnless { it.isEmpty() }
        val outputFile = resolveOutputFile(outputPath, overwrite)
        synchronized(lock) { currentOutputPath = outputFile.absolutePath }

        val lastProgress = AtomicInteger(-1)
        val callback =
          if (progress == null) {
            null
          } else {
            object : AwesomeVideoKitManagerJni.ProgressCallback {
              override fun onProgress(percent: Int) {
                val clamped = percent.coerceIn(0, 100)
                if (lastProgress.getAndSet(clamped) == clamped) return
                mainHandler.post { progress.invoke(clamped) }
              }
            }
          }

        val ret =
          AwesomeVideoKitManagerJni.download(
            url = trimmedUrl,
            outputPath = outputFile.absolutePath,
            watermarkImagePath = normalizedWatermarkPath,
            watermarkPosition = watermarkPosition,
            callback = callback,
          )
        val cancelled = cancelRequested && ret != 0

        if (cancelled) {
          runCatching { outputFile.delete() }
        }

        mainHandler.post {
          synchronized(lock) {
            isDownloading = false
            cancelRequested = false
            currentOutputPath = null
          }

          when {
            cancelled -> completion(null, DownloadError("CANCELLED", "Cancelled."))
            ret != 0 ->
              completion(
                null,
                DownloadError(
                  "FFMPEG_FAILED",
                  FfmpegErrorFormatter.format(
                    ret,
                    watermarkEnabled = normalizedWatermarkPath != null,
                  ),
                ),
              )
            else -> completion(outputFile.absolutePath, null)
          }
        }
      } catch (t: Throwable) {
        mainHandler.post {
          synchronized(lock) {
            isDownloading = false
            cancelRequested = false
            currentOutputPath = null
          }
          val error =
            when (t) {
              is DownloadError -> t
              is UnsatisfiedLinkError ->
                DownloadError("NATIVE_LIB_MISSING", t.message ?: "Failed to load native library.")
              else -> DownloadError("NATIVE_ERROR", t.message ?: "Unknown error")
            }
          completion(null, error)
        }
      }
    }
  }

  fun cancelCurrentTask() {
    synchronized(lock) {
      if (!isDownloading) return
      cancelRequested = true
    }
    runCatching {
      AwesomeVideoKitManagerJni.ensureLoaded(appContext)
      AwesomeVideoKitManagerJni.cancel()
    }
  }

  fun shutdown() {
    cancelCurrentTask()
    executor.shutdownNow()
  }

  private fun resolveOutputFile(outputPath: String?, overwrite: Boolean): File {
    return try {
      AwesomeManagedFileManager.resolveMp4OutputFile(
        outputPath = outputPath,
        overwrite = overwrite,
        prefix = "ffmpeg_m3u8",
        managedDirectory = AwesomeManagedFileManager.managedDirectory(appContext),
      )
    } catch (error: ManagedOutputFileException) {
      throw when (error.kind) {
        ManagedOutputFileException.Kind.CREATE_DIRECTORY_FAILED ->
          DownloadError("CREATE_DIRECTORY_FAILED", error.message ?: "Failed to create output directory.")
        ManagedOutputFileException.Kind.FILE_EXISTS ->
          DownloadError("FILE_EXISTS", error.message ?: "outputPath already exists.")
      }
    }
  }

  companion object {
    @Volatile private var instance: VideoDownloadManager? = null

    fun sharedInstance(context: Context): VideoDownloadManager {
      val appContext = context.applicationContext
      return instance
        ?: synchronized(this) {
          instance ?: VideoDownloadManager(appContext).also { instance = it }
        }
    }
  }
}
