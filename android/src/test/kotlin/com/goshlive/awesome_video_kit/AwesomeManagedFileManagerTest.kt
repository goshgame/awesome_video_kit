package com.goshlive.awesome_video_kit

import android.content.Context
import java.io.File
import java.nio.file.Files
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertTrue
import kotlin.test.fail
import org.mockito.Mockito.`when`
import org.mockito.Mockito.mock

internal class AwesomeManagedFileManagerTest {
  @Test
  fun managedDirectory_usesApplicationCacheDirectory() {
    val context = mock(Context::class.java)
    val appContext = mock(Context::class.java)
    val cacheDirectory = Files.createTempDirectory("gosh-cache").toFile()

    try {
      `when`(context.applicationContext).thenReturn(appContext)
      `when`(appContext.cacheDir).thenReturn(cacheDirectory)

      val managedDirectory = AwesomeManagedFileManager.managedDirectory(context)

      assertEquals(
        File(cacheDirectory, "awesome_video_kit").absolutePath,
        managedDirectory.absolutePath,
      )
    } finally {
      cacheDirectory.deleteRecursively()
    }
  }

  @Test
  fun resolveMp4OutputFile_usesManagedDirectoryWhenOutputPathMissing() {
    val managedDirectory = Files.createTempDirectory("gosh-managed-video").toFile()

    try {
      val outputFile =
        AwesomeManagedFileManager.resolveMp4OutputFile(
          outputPath = null,
          overwrite = true,
          prefix = "ffmpeg_m3u8",
          managedDirectory = managedDirectory,
        )

      assertEquals(managedDirectory.absolutePath, outputFile.parentFile?.absolutePath)
      assertEquals("mp4", outputFile.extension)
      assertTrue(outputFile.name.startsWith("ffmpeg_m3u8_"))
    } finally {
      managedDirectory.deleteRecursively()
    }
  }

  @Test
  fun resolveAudioOutputFile_usesManagedDirectoryAndDerivedExtensionWhenOutputPathMissing() {
    val managedDirectory = Files.createTempDirectory("gosh-managed-audio").toFile()

    try {
      val outputFile =
        AwesomeManagedFileManager.resolveAudioOutputFile(
          inputPath = "/tmp/demo_video.mov",
          outputPath = null,
          preferredExtension = "m4a",
          overwrite = true,
          managedDirectory = managedDirectory,
        )

      assertEquals(managedDirectory.absolutePath, outputFile.parentFile?.absolutePath)
      assertEquals("demo_video_audio.m4a", outputFile.name)
    } finally {
      managedDirectory.deleteRecursively()
    }
  }

  @Test
  fun resolveFrameOutputFile_usesUrlPathBaseNameWhenInputIsNetworkUrl() {
    val managedDirectory = Files.createTempDirectory("gosh-managed-frame-url").toFile()

    try {
      val outputFile =
        AwesomeManagedFileManager.resolveFrameOutputFile(
          inputPath = "https://static.example.com/video/demo.mp4?token=abc",
          timeSeconds = 3.5,
          outputPath = null,
          overwrite = true,
          managedDirectory = managedDirectory,
        )

      assertEquals(managedDirectory.absolutePath, outputFile.parentFile?.absolutePath)
      assertEquals("demo_frame_3500ms.jpg", outputFile.name)
    } finally {
      managedDirectory.deleteRecursively()
    }
  }

  @Test
  fun resolveMp4OutputFile_rejectsExistingFileWhenOverwriteDisabled() {
    val managedDirectory = Files.createTempDirectory("gosh-managed-existing").toFile()
    val existingFile = File(managedDirectory, "existing.mp4")
    existingFile.writeText("demo")

    try {
      AwesomeManagedFileManager.resolveMp4OutputFile(
        outputPath = existingFile.absolutePath,
        overwrite = false,
        prefix = "ffmpeg_transcode",
        managedDirectory = managedDirectory,
      )
      fail("Expected ManagedOutputFileException.")
    } catch (error: ManagedOutputFileException) {
      assertEquals(ManagedOutputFileException.Kind.FILE_EXISTS, error.kind)
      assertEquals("outputPath already exists.", error.message)
    } finally {
      managedDirectory.deleteRecursively()
    }
  }
}
