package com.goshlive.awesomevideokit.internal

import com.goshlive.awesome_video_kit.AwesomeVideoKitManagerJni

internal object NativeBridge {
    @JvmStatic
    external fun download(
        url: String,
        outputPath: String,
        watermarkImagePath: String?,
        watermarkPosition: Int,
        callback: AwesomeVideoKitManagerJni.ProgressCallback?,
    ): Int

    @JvmStatic external fun cancelDownload()

    @JvmStatic
    external fun extractAudio(
        inputPath: String,
        outputPath: String,
        callback: AwesomeVideoKitManagerJni.ProgressCallback?,
    ): Int

    @JvmStatic external fun cancelExtractAudio()

    @JvmStatic
    external fun captureVideoFrames(
        inputPath: String,
        timesSeconds: DoubleArray,
        outputPaths: Array<String>,
        outputWidth: Int,
        outputHeight: Int,
        fastMode: Boolean,
    ): Int

    @JvmStatic external fun cancelCaptureVideoFrames()

    @JvmStatic
    external fun transcode(
        inputPath: String,
        outputPath: String,
        crf: Int,
        preset: String?,
        profile: String?,
        level: String?,
        videoWidth: Int,
        videoHeight: Int,
        audioBitrate: Int,
        faststart: Boolean,
        frameRate: Int,
        watermarkImagePath: String?,
        watermarkPosition: Int,
        callback: AwesomeVideoKitManagerJni.ProgressCallback?,
    ): Int

    @JvmStatic external fun cancelTranscode()
    @JvmStatic external fun pauseTranscode()
    @JvmStatic external fun resumeTranscode()

    @JvmStatic
    external fun transcodeWithConcatImage(
        inputPath: String,
        outputPath: String,
        concatImagePath: String,
        imageDurationUs: Long,
        concatPosition: Int,
        watermarkImagePath: String?,
        watermarkPosition: Int,
        callback: AwesomeVideoKitManagerJni.ProgressCallback?,
    ): Int

    @JvmStatic
    external fun transcodeWithSeparateAudio(
        inputPath: String,
        audioPath: String,
        outputPath: String,
        audioBitrate: Int,
        faststart: Boolean,
        frameRate: Int,
        callback: AwesomeVideoKitManagerJni.ProgressCallback?,
    ): Int

    @JvmStatic external fun getMediaInfoJson(filePath: String): String
}
