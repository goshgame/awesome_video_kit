package com.goshlive.awesome_video_kit

import android.annotation.SuppressLint
import android.content.Context
import android.os.Build
import com.goshlive.awesomevideokit.internal.NativeBridge
import java.io.File

internal object AwesomeVideoKitManagerJni {
    @Volatile private var loaded = false

    object WatermarkPosition {
        const val TOP_LEFT = 0
        const val TOP_RIGHT = 1
        const val BOTTOM_LEFT = 2
        const val BOTTOM_RIGHT = 3
        const val CENTER = 4
        const val ALTERNATING_TOP_LEFT_BOTTOM_RIGHT = 5
    }

    object ConcatImagePosition {
        const val HEAD = 0
        const val TAIL = 1
    }

    @SuppressLint("UnsafeDynamicallyLoadedCode")
    @Synchronized
    fun ensureLoaded(context: Context) {
        if (loaded) return

        try {
            System.loadLibrary("AwesomeVideoKit")
            loaded = true
            return
        } catch (_: UnsatisfiedLinkError) {
            // Fallback: allow a non-standard filename (e.g. AwesomeVideoKit.so) in jniLibs.
        }

        val nativeDir = context.applicationInfo.nativeLibraryDir
        val candidates =
            listOf(
                File(nativeDir, "libAwesomeVideoKit.so"),
                File(nativeDir, "AwesomeVideoKit.so"),
            )

        val found = candidates.firstOrNull { it.exists() }
        if (found != null) {
            System.load(found.absolutePath)
            loaded = true
            return
        }

        throw UnsatisfiedLinkError(
            "Failed to load native library. " +
                    "Expected libAwesomeVideoKit.so (System.loadLibrary(\"AwesomeVideoKit\")) " +
                    "or AwesomeVideoKit.so in nativeLibraryDir=$nativeDir. " +
                    "supportedAbis=${Build.SUPPORTED_ABIS?.joinToString() ?: "unknown"}",
        )
    }

    interface ProgressCallback {
        fun onProgress(percent: Int)
    }

    data class TranscodeOptions(
        val crf: Int = 0,
        val preset: String? = null,
        val profile: String? = null,
        val level: String? = null,
        val videoWidth: Int = 0,
        val videoHeight: Int = 0,
        val audioBitrate: Int = 0,
        val faststart: Boolean = true,
        val frameRate: Int = 0,
        val watermarkImagePath: String? = null,
        val watermarkPosition: Int = WatermarkPosition.BOTTOM_RIGHT,
    )

    data class ConcatImageOptions(
        val concatImagePath: String,
        val imageDurationMs: Long,
        val concatPosition: Int = ConcatImagePosition.TAIL,
        val watermarkImagePath: String? = null,
        val watermarkPosition: Int = WatermarkPosition.BOTTOM_RIGHT,
    )

    data class MediaWithAudioOptions(
        val audioBitrate: Int = 0,
        val faststart: Boolean = true,
        val frameRate: Int = 0,
    )

    @JvmStatic
    fun download(url: String, outputPath: String, callback: ProgressCallback?): Int =
        download(
            url = url,
            outputPath = outputPath,
            watermarkImagePath = null,
            watermarkPosition = WatermarkPosition.BOTTOM_RIGHT,
            callback = callback,
        )

    @JvmStatic
    fun download(
        url: String,
        outputPath: String,
        watermarkImagePath: String?,
        watermarkPosition: Int = WatermarkPosition.BOTTOM_RIGHT,
        callback: ProgressCallback? = null,
    ): Int =
        NativeBridge.download(
            url = url,
            outputPath = outputPath,
            watermarkImagePath = watermarkImagePath.normalizeOptionalArg(),
            watermarkPosition = watermarkPosition,
            callback = callback,
        )

    @JvmStatic
    fun cancel() {
        NativeBridge.cancelDownload()
    }

    @JvmStatic
    fun extractAudio(
        inputPath: String,
        outputPath: String,
        callback: ProgressCallback? = null,
    ): Int =
        NativeBridge.extractAudio(
            inputPath = inputPath,
            outputPath = outputPath,
            callback = callback,
        )

    @JvmStatic
    fun captureVideoFrames(
        inputPath: String,
        timesSeconds: DoubleArray,
        outputPaths: Array<String>,
        outputWidth: Int = 0,
        outputHeight: Int = 0,
        fastMode: Boolean = false,
    ): Int =
        NativeBridge.captureVideoFrames(
            inputPath = inputPath.trim(),
            timesSeconds = timesSeconds,
            outputPaths = outputPaths,
            outputWidth = outputWidth,
            outputHeight = outputHeight,
            fastMode = fastMode,
        )

    @JvmStatic
    fun cancelCaptureVideoFrames() {
        NativeBridge.cancelCaptureVideoFrames()
    }

    @JvmStatic
    fun cancelExtractAudio() {
        NativeBridge.cancelExtractAudio()
    }

    @JvmStatic
    fun transcode(
        inputPath: String,
        outputPath: String,
        options: TranscodeOptions = TranscodeOptions(),
        callback: ProgressCallback? = null,
    ): Int =
        NativeBridge.transcode(
            inputPath = inputPath,
            outputPath = outputPath,
            crf = options.crf,
            preset = options.preset.normalizeOptionalArg(),
            profile = options.profile.normalizeOptionalArg(),
            level = options.level.normalizeOptionalArg(),
            videoWidth = options.videoWidth,
            videoHeight = options.videoHeight,
            audioBitrate = options.audioBitrate,
            faststart = options.faststart,
            frameRate = options.frameRate,
            watermarkImagePath = options.watermarkImagePath.normalizeOptionalArg(),
            watermarkPosition = options.watermarkPosition,
            callback = callback,
        )

    @JvmStatic
    fun transcode(
        inputPath: String,
        outputPath: String,
        crf: Int = 0,
        preset: String? = null,
        profile: String? = null,
        level: String? = null,
        videoWidth: Int = 0,
        videoHeight: Int = 0,
        audioBitrate: Int = 0,
        faststart: Boolean = true,
        frameRate: Int = 0,
        watermarkImagePath: String? = null,
        watermarkPosition: Int = WatermarkPosition.BOTTOM_RIGHT,
        callback: ProgressCallback? = null,
    ): Int =
        transcode(
            inputPath = inputPath,
            outputPath = outputPath,
            options =
                TranscodeOptions(
                    crf = crf,
                    preset = preset,
                    profile = profile,
                    level = level,
                    videoWidth = videoWidth,
                    videoHeight = videoHeight,
                    audioBitrate = audioBitrate,
                    faststart = faststart,
                    frameRate = frameRate,
                    watermarkImagePath = watermarkImagePath,
                    watermarkPosition = watermarkPosition,
                ),
            callback = callback,
        )

    @JvmStatic
    fun cancelTranscode() {
        NativeBridge.cancelTranscode()
    }

    @JvmStatic
    fun pauseTranscode() {
        NativeBridge.pauseTranscode()
    }

    @JvmStatic
    fun resumeTranscode() {
        NativeBridge.resumeTranscode()
    }

    @JvmStatic
    fun transcodeWithConcatImage(
        inputPath: String,
        outputPath: String,
        options: ConcatImageOptions,
        callback: ProgressCallback? = null,
    ): Int =
        NativeBridge.transcodeWithConcatImage(
            inputPath = inputPath,
            outputPath = outputPath,
            concatImagePath = options.concatImagePath.trim(),
            imageDurationUs = options.imageDurationMs.coerceAtLeast(0L) * 1000L,
            concatPosition = options.concatPosition,
            watermarkImagePath = options.watermarkImagePath.normalizeOptionalArg(),
            watermarkPosition = options.watermarkPosition,
            callback = callback,
        )

    @JvmStatic
    fun transcodeWithSeparateAudio(
        inputPath: String,
        audioPath: String,
        outputPath: String,
        options: MediaWithAudioOptions = MediaWithAudioOptions(),
        callback: ProgressCallback? = null,
    ): Int =
        NativeBridge.transcodeWithSeparateAudio(
            inputPath = inputPath,
            audioPath = audioPath,
            outputPath = outputPath,
            audioBitrate = options.audioBitrate,
            faststart = options.faststart,
            frameRate = options.frameRate,
            callback = callback,
        )

    @JvmStatic
    fun inspectMediaInfoJson(filePath: String): String =
        NativeBridge.getMediaInfoJson(filePath.trim())

    private fun String?.normalizeOptionalArg(): String? = this?.trim()?.takeUnless { it.isEmpty() }
}
