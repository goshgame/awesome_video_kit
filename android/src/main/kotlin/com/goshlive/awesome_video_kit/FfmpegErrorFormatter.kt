package com.goshlive.awesome_video_kit

internal object FfmpegErrorFormatter {
  fun format(code: Int, watermarkEnabled: Boolean = false): String {
    val tag = decodeFferrTag(code)

    val base =
      if (tag == null) {
        "FFmpeg failed (code=$code)."
      } else {
        val hint =
          when (tag) {
            "PRO" ->
              "Protocol not found. Your FFmpeg build likely lacks network protocol support (e.g. https/tls)."
            else -> "FFmpeg tag=$tag."
          }
        "FFmpeg failed (code=$code, tag=$tag). $hint"
      }

    if (!watermarkEnabled) return base
    return "$base Watermark mode requires FFmpeg built with libavfilter/libswscale and image decoding support."
  }

  private fun decodeFferrTag(code: Int): String? {
    if (code >= 0) return null

    val tag = (0u - code.toUInt())
    val a = (tag and 0xFFu).toInt()
    if (a != 0xF8) return null

    val b = ((tag shr 8) and 0xFFu).toInt()
    val c = ((tag shr 16) and 0xFFu).toInt()
    val d = ((tag shr 24) and 0xFFu).toInt()

    if (b !in 32..126 || c !in 32..126 || d !in 32..126) return null
    return "${b.toChar()}${c.toChar()}${d.toChar()}"
  }
}
