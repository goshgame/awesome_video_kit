package com.goshlive.awesome_video_kit

import android.content.Context
import java.io.File
import java.net.URI
import java.util.UUID

internal object AwesomeManagedFileManager {
  private const val MANAGED_DIRECTORY_NAME = "awesome_video_kit"

  fun managedDirectory(context: Context): File {
    return File(context.applicationContext.cacheDir, MANAGED_DIRECTORY_NAME)
  }

  fun resolveMp4OutputFile(
    outputPath: String?,
    overwrite: Boolean,
    prefix: String,
    managedDirectory: File,
  ): File {
    val defaultFileName = buildManagedMp4FileName(prefix)
    return resolveOutputFile(
      outputPath = outputPath,
      overwrite = overwrite,
      defaultFileName = defaultFileName,
      preferredExtension = "mp4",
      managedDirectory = managedDirectory,
    )
  }

  fun resolveAudioOutputFile(
    inputPath: String,
    outputPath: String?,
    preferredExtension: String,
    overwrite: Boolean,
    managedDirectory: File,
  ): File {
    val defaultFileName = defaultAudioOutputFileName(inputPath, preferredExtension)
    return resolveOutputFile(
      outputPath = outputPath,
      overwrite = overwrite,
      defaultFileName = defaultFileName,
      preferredExtension = preferredExtension,
      managedDirectory = managedDirectory,
    )
  }

  fun resolveFrameOutputFile(
    inputPath: String,
    timeSeconds: Double,
    outputPath: String?,
    overwrite: Boolean,
    managedDirectory: File,
  ): File {
    val defaultFileName = defaultFrameOutputFileName(inputPath, timeSeconds, null)
    return resolveOutputFile(
      outputPath = outputPath,
      overwrite = overwrite,
      defaultFileName = defaultFileName,
      preferredExtension = "jpg",
      managedDirectory = managedDirectory,
    )
  }

  fun resolveFrameOutputFiles(
    inputPath: String,
    timesSeconds: List<Double>,
    outputDirectory: String?,
    overwrite: Boolean,
    managedDirectory: File,
  ): List<File> {
    val cleaned = outputDirectory?.trim().orEmpty()
    val directory = ensureDirectory(if (cleaned.isEmpty()) managedDirectory else File(cleaned))
    return timesSeconds.mapIndexed { index, timeSeconds ->
      val outputFile = File(directory, defaultFrameOutputFileName(inputPath, timeSeconds, index))
      prepareOutputFile(outputFile, overwrite)
      outputFile
    }
  }

  private fun resolveOutputFile(
    outputPath: String?,
    overwrite: Boolean,
    defaultFileName: String,
    preferredExtension: String,
    managedDirectory: File,
  ): File {
    val cleaned = outputPath?.trim().orEmpty()
    val initial =
      if (cleaned.isEmpty()) {
        File(ensureDirectory(managedDirectory), defaultFileName)
      } else {
        File(cleaned)
      }

    val file =
      when {
        cleaned.endsWith("/") -> File(initial, defaultFileName)
        initial.exists() && initial.isDirectory -> File(initial, defaultFileName)
        cleaned.isNotEmpty() && initial.extension.isBlank() ->
          File("${initial.absolutePath}.$preferredExtension")
        else -> initial
      }

    file.parentFile?.let { ensureDirectory(it) }
    prepareOutputFile(file, overwrite)
    return file
  }

  private fun buildManagedMp4FileName(prefix: String): String {
    val normalizedPrefix = prefix.trim().takeUnless { it.isEmpty() } ?: "ffmpeg_output"
    return "${normalizedPrefix}_${UUID.randomUUID()}.mp4"
  }

  private fun defaultAudioOutputFileName(inputPath: String, extension: String): String {
    val baseName = File(inputPath).nameWithoutExtension.ifBlank { "audio_extract" }
    return "${baseName}_audio.$extension"
  }

  private fun defaultFrameOutputFileName(
    inputPath: String,
    timeSeconds: Double,
    index: Int?,
  ): String {
    val baseName = inputBaseName(inputPath).ifBlank { "video" }
    val timeMs = (timeSeconds.coerceAtLeast(0.0) * 1000.0).toLong()
    val indexPart = index?.let { "_${it.toString().padStart(4, '0')}" } ?: ""
    return "${baseName}_frame${indexPart}_${timeMs}ms.jpg"
  }

  private fun inputBaseName(inputPath: String): String {
    val trimmed = inputPath.trim()
    val uri = runCatching { URI(trimmed) }.getOrNull()
    val scheme = uri?.scheme?.lowercase()
    if ((scheme == "http" || scheme == "https") && !uri.host.isNullOrBlank()) {
      return File(uri.path.orEmpty()).nameWithoutExtension
    }
    return File(trimmed).nameWithoutExtension
  }

  private fun ensureDirectory(directory: File): File {
    if (directory.exists()) {
      if (!directory.isDirectory) {
        throw ManagedOutputFileException(
          kind = ManagedOutputFileException.Kind.CREATE_DIRECTORY_FAILED,
          message = "Output directory is not a directory: ${directory.absolutePath}",
        )
      }
      return directory
    }

    if (!directory.mkdirs()) {
      throw ManagedOutputFileException(
        kind = ManagedOutputFileException.Kind.CREATE_DIRECTORY_FAILED,
        message = "Failed to create output directory: ${directory.absolutePath}",
      )
    }

    return directory
  }

  private fun prepareOutputFile(file: File, overwrite: Boolean) {
    if (!file.exists()) return
    if (!overwrite) {
      throw ManagedOutputFileException(
        kind = ManagedOutputFileException.Kind.FILE_EXISTS,
        message = "outputPath already exists.",
      )
    }
    if (!file.delete()) {
      throw ManagedOutputFileException(
        kind = ManagedOutputFileException.Kind.FILE_EXISTS,
        message = "Failed to overwrite existing file: ${file.absolutePath}",
      )
    }
  }
}

internal class ManagedOutputFileException(
  val kind: Kind,
  message: String,
) : Exception(message) {
  enum class Kind {
    CREATE_DIRECTORY_FAILED,
    FILE_EXISTS,
  }
}
