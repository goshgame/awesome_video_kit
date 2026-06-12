import Flutter
import AwesomeVideoKitSDK
import UIKit

private final class EventSinkStreamHandler: NSObject, FlutterStreamHandler {
    private let onListenHandler: (@escaping FlutterEventSink) -> Void
    private let onCancelHandler: () -> Void

    init(
        onListenHandler: @escaping (@escaping FlutterEventSink) -> Void,
        onCancelHandler: @escaping () -> Void
    ) {
        self.onListenHandler = onListenHandler
        self.onCancelHandler = onCancelHandler
    }

    func onListen(withArguments arguments: Any?, eventSink events: @escaping FlutterEventSink) -> FlutterError? {
        onListenHandler(events)
        return nil
    }

    func onCancel(withArguments arguments: Any?) -> FlutterError? {
        onCancelHandler()
        return nil
    }
}

public class AwesomeVideoKitPlugin: NSObject, FlutterPlugin {
    private let videoKitManager = AwesomeVideoKitManager.sharedInstance()
    private let downloadManager = AwesomeVideoDownloadManager.sharedInstance()
    private let transcodeManager = AwesomeVideoTranscoderManager.sharedInstance()
    private let frameCaptureManager = AwesomeVideoFrameCaptureManager.sharedInstance()
    private var downloadEventSink: FlutterEventSink?
    private var transcodeEventSink: FlutterEventSink?
    private var extractAudioEventSink: FlutterEventSink?
    private var downloadStreamHandler: EventSinkStreamHandler?
    private var transcodeStreamHandler: EventSinkStreamHandler?
    private var extractAudioStreamHandler: EventSinkStreamHandler?

    public static func register(with registrar: FlutterPluginRegistrar) {
        let channel = FlutterMethodChannel(
            name: "awesome_video_kit",
            binaryMessenger: registrar.messenger()
        )
        let downloadEventChannel = FlutterEventChannel(
            name: "awesome_video_kit/eventchannel",
            binaryMessenger: registrar.messenger()
        )
        let transcodeEventChannel = FlutterEventChannel(
            name: "awesome_video_kit/transcode_eventchannel",
            binaryMessenger: registrar.messenger()
        )
        let extractAudioEventChannel = FlutterEventChannel(
            name: "awesome_video_kit/extract_audio_eventchannel",
            binaryMessenger: registrar.messenger()
        )
        let instance = AwesomeVideoKitPlugin()
        registrar.addMethodCallDelegate(instance, channel: channel)

        instance.downloadStreamHandler = EventSinkStreamHandler(
            onListenHandler: { [weak instance] events in
                instance?.downloadEventSink = events
            },
            onCancelHandler: { [weak instance] in
                instance?.downloadEventSink = nil
            }
        )
        instance.transcodeStreamHandler = EventSinkStreamHandler(
            onListenHandler: { [weak instance] events in
                instance?.transcodeEventSink = events
            },
            onCancelHandler: { [weak instance] in
                instance?.transcodeEventSink = nil
            }
        )
        instance.extractAudioStreamHandler = EventSinkStreamHandler(
            onListenHandler: { [weak instance] events in
                instance?.extractAudioEventSink = events
            },
            onCancelHandler: { [weak instance] in
                instance?.extractAudioEventSink = nil
            }
        )

        downloadEventChannel.setStreamHandler(instance.downloadStreamHandler)
        transcodeEventChannel.setStreamHandler(instance.transcodeStreamHandler)
        extractAudioEventChannel.setStreamHandler(instance.extractAudioStreamHandler)
    }

    public func handle(_ call: FlutterMethodCall, result: @escaping FlutterResult) {
        switch call.method {
        case "getPlatformVersion":
            result("iOS " + UIDevice.current.systemVersion)
        case "downloadVideo", "download":
            handleDownload(call, result: result)
        case "cancelDownload", "canceDownload", "cancel":
            downloadManager.cancelCurrentTask()
            result(nil)
        case "isDownloading":
            result(NSNumber(value: downloadManager.isDownloading))
        case "extractAudio":
            handleExtractAudio(call, result: result)
        case "captureVideoFrame":
            handleCaptureVideoFrame(call, result: result)
        case "captureVideoFrames":
            handleCaptureVideoFrames(call, result: result)
        case "cancelExtractAudio":
            videoKitManager.cancelCurrentTask()
            result(nil)
        case "isExtractingAudio":
            result(NSNumber(value: videoKitManager.isExtractingAudio))
        case "transcodeVideo", "transcode":
            handleTranscode(call, result: result)
        case "transcodeVideoWithConcatImage":
            handleConcatImageTranscode(call, result: result)
        case "transcodeMediaWithAudio":
            handleSeparateAudioTranscode(call, result: result)
        case "cancelTranscode":
            transcodeManager.cancelCurrentTask()
            result(nil)
        case "isTranscoding":
            result(NSNumber(value: transcodeManager.isTranscoding))
        case "pauseTranscode":
            transcodeManager.pauseCurrentTask()
            result(nil)
        case "resumeTranscode":
            transcodeManager.resumeCurrentTask()
            result(nil)
        case "isTranscodePaused":
            result(NSNumber(value: transcodeManager.isPaused))
        case "getMediaInfo":
            handleGetMediaInfo(call, result: result)
        case "getCurrentDownloadOutputPath":
            result(downloadManager.currentOutputPath)
        case "getCurrentTranscodeOutputPath":
            result(transcodeManager.currentOutputPath)
        case "getCurrentExtractAudioOutputPath":
            result(videoKitManager.currentOutputPath)
        default:
            result(FlutterMethodNotImplemented)
        }
    }

    private func handleDownload(_ call: FlutterMethodCall, result: @escaping FlutterResult) {
        guard let args = call.arguments as? [String: Any] else {
            result(FlutterError(code: "INVALID_ARGS", message: "Arguments must be a map.", details: nil))
            return
        }

        let url = optionalStringArg(args, key: "url") ?? ""
        guard !url.isEmpty else {
            result(FlutterError(code: "INVALID_ARGS", message: "Missing url.", details: nil))
            return
        }

        let outputPath = optionalStringArg(args, key: "outputPath")
        let overwrite = booleanArg(args, key: "overwrite", defaultValue: true)
        let watermarkImagePath = optionalStringArg(args, key: "watermarkImagePath")
        let watermarkPosition = watermarkPositionArg(args, key: "watermarkPosition")
        let progress = downloadProgressBlock()
        let completion = downloadCompletion(result)

        downloadManager.downloadVideoToMp4(
            withURL: url,
            outputPath: outputPath,
            overwrite: overwrite,
            watermarkImagePath: watermarkImagePath,
            watermarkPosition: watermarkPosition,
            progress: progress,
            completion: completion
        )
    }

    private func handleExtractAudio(_ call: FlutterMethodCall, result: @escaping FlutterResult) {
        guard let args = call.arguments as? [String: Any] else {
            result(FlutterError(code: "INVALID_ARGS", message: "Arguments must be a map.", details: nil))
            return
        }

        let inputPath = optionalStringArg(args, key: "inputPath") ?? ""
        guard !inputPath.isEmpty else {
            result(FlutterError(code: "INVALID_ARGS", message: "Missing inputPath.", details: nil))
            return
        }

        videoKitManager.extractAudioFromVideo(
            atPath: inputPath,
            outputPath: optionalStringArg(args, key: "outputPath"),
            overwrite: booleanArg(args, key: "overwrite", defaultValue: true),
            progress: extractAudioProgressBlock(),
            completion: { [weak self] outputPath, error in
                self?.complete(
                    result,
                    outputPath: outputPath,
                    error: error as NSError?,
                    emptyOutputMessage: "Native extractAudio returned empty outputPath."
                )
            }
        )
    }

    private func handleCaptureVideoFrame(_ call: FlutterMethodCall, result: @escaping FlutterResult) {
        guard let args = call.arguments as? [String: Any] else {
            result(FlutterError(code: "INVALID_ARGS", message: "Arguments must be a map.", details: nil))
            return
        }

        let inputPath = optionalStringArg(args, key: "inputPath") ?? ""
        guard !inputPath.isEmpty else {
            result(FlutterError(code: "INVALID_ARGS", message: "Missing inputPath.", details: nil))
            return
        }

        let timeSeconds = doubleArg(args, key: "timeSeconds", defaultValue: .nan)
        guard timeSeconds.isFinite && timeSeconds >= 0 else {
            result(FlutterError(code: "INVALID_ARGS", message: "timeSeconds must be a valid non-negative value.", details: nil))
            return
        }

        frameCaptureManager.captureVideoFrame(
            atPath: inputPath,
            timeSeconds: timeSeconds,
            outputPath: optionalStringArg(args, key: "outputPath"),
            overwrite: booleanArg(args, key: "overwrite", defaultValue: true),
            outputWidth: intArg(args, key: "outputWidth", defaultValue: 0),
            outputHeight: intArg(args, key: "outputHeight", defaultValue: 0),
            fastMode: booleanArg(args, key: "fastMode", defaultValue: false),
            completion: { [weak self] outputPath, error in
                self?.complete(
                    result,
                    outputPath: outputPath,
                    error: error as NSError?,
                    emptyOutputMessage: "Native captureVideoFrame returned empty outputPath."
                )
            }
        )
    }

    private func handleCaptureVideoFrames(_ call: FlutterMethodCall, result: @escaping FlutterResult) {
        guard let args = call.arguments as? [String: Any] else {
            result(FlutterError(code: "INVALID_ARGS", message: "Arguments must be a map.", details: nil))
            return
        }

        let inputPath = optionalStringArg(args, key: "inputPath") ?? ""
        guard !inputPath.isEmpty else {
            result(FlutterError(code: "INVALID_ARGS", message: "Missing inputPath.", details: nil))
            return
        }

        let timesSeconds = doubleListArg(args, key: "timesSeconds")
        guard !timesSeconds.isEmpty else {
            result(FlutterError(code: "INVALID_ARGS", message: "timesSeconds must not be empty.", details: nil))
            return
        }
        guard timesSeconds.allSatisfy({ $0.isFinite && $0 >= 0 }) else {
            result(FlutterError(code: "INVALID_ARGS", message: "timesSeconds must contain only valid non-negative values.", details: nil))
            return
        }

        frameCaptureManager.captureVideoFrames(
            atPath: inputPath,
            timeSeconds: timesSeconds.map { NSNumber(value: $0) },
            outputDirectory: optionalStringArg(args, key: "outputDirectory"),
            overwrite: booleanArg(args, key: "overwrite", defaultValue: true),
            outputWidth: intArg(args, key: "outputWidth", defaultValue: 0),
            outputHeight: intArg(args, key: "outputHeight", defaultValue: 0),
            fastMode: booleanArg(args, key: "fastMode", defaultValue: false),
            completion: { [weak self] outputPaths, error in
                self?.completeList(
                    result,
                    outputPaths: outputPaths,
                    error: error as NSError?,
                    emptyOutputMessage: "Native captureVideoFrames returned empty outputPaths."
                )
            }
        )
    }

    private func handleTranscode(_ call: FlutterMethodCall, result: @escaping FlutterResult) {
        guard let args = call.arguments as? [String: Any] else {
            result(FlutterError(code: "INVALID_ARGS", message: "Arguments must be a map.", details: nil))
            return
        }

        let inputPath = optionalStringArg(args, key: "inputPath") ?? ""
        guard !inputPath.isEmpty else {
            result(FlutterError(code: "INVALID_ARGS", message: "Missing inputPath.", details: nil))
            return
        }

        let options = AwesomeVideoTranscodeOptions()
        options.overwrite = NSNumber(value: booleanArg(args, key: "overwrite", defaultValue: true))
        options.crf = NSNumber(value: intArg(args, key: "crf", defaultValue: 0))
        options.preset = optionalStringArg(args, key: "preset")
        options.profile = optionalStringArg(args, key: "profile")
        options.level = optionalStringArg(args, key: "level")
        options.videoWidth = NSNumber(value: intArg(args, key: "videoWidth", defaultValue: 0))
        options.videoHeight = NSNumber(value: intArg(args, key: "videoHeight", defaultValue: 0))
        options.audioBitrate = NSNumber(value: intArg(args, key: "audioBitrate", defaultValue: 0))
        options.faststart = NSNumber(value: booleanArg(args, key: "faststart", defaultValue: true))
        options.frameRate = NSNumber(value: intArg(args, key: "frameRate", defaultValue: 0))
        options.watermarkImagePath = optionalStringArg(args, key: "watermarkImagePath")
        options.watermarkPosition = NSNumber(value: watermarkPositionArg(args, key: "watermarkPosition").rawValue)

        let progress = transcodeProgressBlock()
        let completion = transcodeCompletion(
            result,
            emptyOutputMessage: "Native transcode returned empty outputPath."
        )

        transcodeManager.transcodeVideo(
            atPath: inputPath,
            outputPath: optionalStringArg(args, key: "outputPath"),
            options: options,
            progress: progress,
            completion: completion
        )
    }

    private func handleConcatImageTranscode(_ call: FlutterMethodCall, result: @escaping FlutterResult) {
        guard let args = call.arguments as? [String: Any] else {
            result(FlutterError(code: "INVALID_ARGS", message: "Arguments must be a map.", details: nil))
            return
        }

        let inputPath = optionalStringArg(args, key: "inputPath") ?? ""
        guard !inputPath.isEmpty else {
            result(FlutterError(code: "INVALID_ARGS", message: "Missing inputPath.", details: nil))
            return
        }

        let concatImagePath = optionalStringArg(args, key: "concatImagePath") ?? ""
        guard !concatImagePath.isEmpty else {
            result(FlutterError(code: "INVALID_ARGS", message: "Missing concatImagePath.", details: nil))
            return
        }

        let imageDurationMs = intArg(args, key: "imageDurationMs", defaultValue: 0)
        guard imageDurationMs > 0 else {
            result(FlutterError(code: "INVALID_ARGS", message: "imageDurationMs must be greater than 0.", details: nil))
            return
        }

        let options = AwesomeVideoConcatOptions()
        options.overwrite = NSNumber(value: booleanArg(args, key: "overwrite", defaultValue: true))
        options.imageDuration = NSNumber(value: TimeInterval(imageDurationMs) / 1000.0)
        options.concatPosition = NSNumber(value: concatPositionArg(args, key: "concatPosition").rawValue)
        options.watermarkImagePath = optionalStringArg(args, key: "watermarkImagePath")
        options.watermarkPosition = NSNumber(value: watermarkPositionArg(args, key: "watermarkPosition").rawValue)

        transcodeManager.transcodeVideo(
            atPath: inputPath,
            outputPath: optionalStringArg(args, key: "outputPath"),
            concatImagePath: concatImagePath,
            options: options,
            progress: transcodeProgressBlock(),
            completion: transcodeCompletion(
                result,
                emptyOutputMessage: "Native transcodeVideoWithConcatImage returned empty outputPath."
            )
        )
    }

    private func handleSeparateAudioTranscode(_ call: FlutterMethodCall, result: @escaping FlutterResult) {
        guard let args = call.arguments as? [String: Any] else {
            result(FlutterError(code: "INVALID_ARGS", message: "Arguments must be a map.", details: nil))
            return
        }

        let inputPath = optionalStringArg(args, key: "inputPath") ?? ""
        guard !inputPath.isEmpty else {
            result(FlutterError(code: "INVALID_ARGS", message: "Missing inputPath.", details: nil))
            return
        }

        let audioPath = optionalStringArg(args, key: "audioPath") ?? ""
        guard !audioPath.isEmpty else {
            result(FlutterError(code: "INVALID_ARGS", message: "Missing audioPath.", details: nil))
            return
        }

        let options = AwesomeVideoSeparateAudioOptions()
        options.overwrite = NSNumber(value: booleanArg(args, key: "overwrite", defaultValue: true))
        options.audioBitrate = NSNumber(value: intArg(args, key: "audioBitrate", defaultValue: 0))
        options.faststart = NSNumber(value: booleanArg(args, key: "faststart", defaultValue: true))
        options.frameRate = NSNumber(value: intArg(args, key: "frameRate", defaultValue: 0))

        transcodeManager.transcodeMedia(
            atPath: inputPath,
            audioPath: audioPath,
            outputPath: optionalStringArg(args, key: "outputPath"),
            options: options,
            progress: transcodeProgressBlock(),
            completion: transcodeCompletion(
                result,
                emptyOutputMessage: "Native transcodeMediaWithAudio returned empty outputPath."
            )
        )
    }

    private func handleGetMediaInfo(_ call: FlutterMethodCall, result: @escaping FlutterResult) {
        guard let args = call.arguments as? [String: Any] else {
            result(FlutterError(code: "INVALID_ARGS", message: "Arguments must be a map.", details: nil))
            return
        }

        let filePath = optionalStringArg(args, key: "filePath") ?? ""
        guard !filePath.isEmpty else {
            result(FlutterError(code: "INVALID_ARGS", message: "Missing filePath.", details: nil))
            return
        }

        let info = AwesomeAVFileInfo()
        do {
            try info.load(fromFile: filePath)
            result(mediaInfoDictionary(from: info))
        } catch let error as NSError {
            result(flutterError(from: error))
        } catch {
            result(FlutterError(
                code: "LOAD_FAILED",
                message: error.localizedDescription,
                details: nil
            ))
        }
    }

    private func mediaInfoDictionary(from info: AwesomeAVFileInfo) -> [String: Any] {
        let videoStreamCount = Int(info.videoStreamCount)
        let audioStreamCount = Int(info.audioStreamCount)

        let videoStreams = (0..<videoStreamCount).map { index in
            videoStreamDictionary(from: info, streamIndex: UInt32(index))
        }
        let audioStreams = (0..<audioStreamCount).map { index in
            audioStreamDictionary(from: info, streamIndex: UInt32(index))
        }

        return [
            "fileType": NSNumber(value: info.avFileType.rawValue),
            "duration": NSNumber(value: info.duration),
            "dataRate": NSNumber(value: info.dataRate),
            "videoStreamCount": NSNumber(value: info.videoStreamCount),
            "audioStreamCount": NSNumber(value: info.audioStreamCount),
            "sourcePath": info.sourcePath,
            "videoStreams": videoStreams,
            "audioStreams": audioStreams,
        ]
    }

    private func videoStreamDictionary(from info: AwesomeAVFileInfo, streamIndex: UInt32) -> [String: Any] {
        let dimension = info.getVideoStreamDimension(streamIndex)
        let pixelAspectRatio = info.getVideoStreamPixelAspectRatio(streamIndex)
        let frameRate = info.getVideoStreamFrameRate(streamIndex)

        return [
            "duration": NSNumber(value: info.getVideoStreamDuration(streamIndex)),
            "dimension": sizeDictionary(dimension),
            "pixelAspectRatio": rationalDictionary(pixelAspectRatio),
            "frameRate": rationalDictionary(frameRate),
            "rotation": NSNumber(value: info.getVideoStreamRotation(streamIndex).rawValue),
            "rotationDegrees": NSNumber(value: info.getVideoStreamRotationDegrees(streamIndex)),
            "componentBitCount": NSNumber(value: info.getVideoStreamComponentBitCount(streamIndex)),
            "codecType": NSNumber(value: info.getVideoStreamCodecType(streamIndex).rawValue),
            "codecName": info.getVideoStreamCodecName(streamIndex),
            "codecProfile": NSNumber(value: info.getVideoCodecProfile(streamIndex)),
            "codecLevel": NSNumber(value: info.getVideoCodecLevel(streamIndex)),
            "colorTransfer": NSNumber(value: info.getVideoStreamColorTransfer(streamIndex).rawValue),
            "hdrType": NSNumber(value: info.getVideoStreamHDRType(streamIndex).rawValue),
        ]
    }

    private func audioStreamDictionary(from info: AwesomeAVFileInfo, streamIndex: UInt32) -> [String: Any] {
        return [
            "duration": NSNumber(value: info.getAudioStreamDuration(streamIndex)),
            "sampleRate": NSNumber(value: info.getAudioStreamSampleRate(streamIndex)),
            "channelCount": NSNumber(value: info.getAudioStreamChannelCount(streamIndex)),
            "codecSupported": NSNumber(value: info.getAudioStreamCodecSupport(streamIndex)),
            "codecName": info.getAudioStreamCodecName(streamIndex),
        ]
    }

    private func sizeDictionary(_ size: AwesomeSize) -> [String: Any] {
        return [
            "width": NSNumber(value: size.width),
            "height": NSNumber(value: size.height),
        ]
    }

    private func rationalDictionary(_ rational: AwesomeRational) -> [String: Any] {
        return [
            "num": NSNumber(value: rational.num),
            "den": NSNumber(value: rational.den),
        ]
    }

    private func downloadProgressBlock() -> VideoDownloadProgressBlock {
        return { [weak self] percent in
            self?.emitProgress(percent, sink: self?.downloadEventSink)
        }
    }

    private func transcodeProgressBlock() -> VideoTranscoderProgressBlock {
        return { [weak self] percent in
            self?.emitProgress(percent, sink: self?.transcodeEventSink)
        }
    }

    private func extractAudioProgressBlock() -> AwesomeVideoKitProgressBlock {
        return { [weak self] percent in
            self?.emitProgress(percent, sink: self?.extractAudioEventSink)
        }
    }

    private func downloadCompletion(_ result: @escaping FlutterResult) -> VideoDownloadCompletionBlock {
        return { [weak self] outputPath, error in
            self?.complete(
                result,
                outputPath: outputPath,
                error: error as NSError?,
                emptyOutputMessage: "Native download returned empty outputPath."
            )
        }
    }

    private func transcodeCompletion(
        _ result: @escaping FlutterResult,
        emptyOutputMessage: String
    ) -> VideoTranscoderCompletionBlock {
        return { [weak self] outputPath, error in
            self?.complete(
                result,
                outputPath: outputPath,
                error: error as NSError?,
                emptyOutputMessage: emptyOutputMessage
            )
        }
    }

    private func emitProgress(_ percent: Int, sink: FlutterEventSink?) {
        dispatchToMain {
            sink?(percent)
        }
    }

    private func complete(
        _ result: @escaping FlutterResult,
        outputPath: String?,
        error: NSError?,
        emptyOutputMessage: String
    ) {
        dispatchToMain {
            if let error {
                result(self.flutterError(from: error))
                return
            }

            if let outputPath, !outputPath.isEmpty {
                result(outputPath)
                return
            }

            result(FlutterError(code: "NO_OUTPUT", message: emptyOutputMessage, details: nil))
        }
    }

    private func completeList(
        _ result: @escaping FlutterResult,
        outputPaths: [String]?,
        error: NSError?,
        emptyOutputMessage: String
    ) {
        dispatchToMain {
            if let error {
                result(self.flutterError(from: error))
                return
            }

            if let outputPaths, !outputPaths.isEmpty {
                result(outputPaths)
                return
            }

            result(FlutterError(code: "NO_OUTPUT", message: emptyOutputMessage, details: nil))
        }
    }

    private func flutterError(from error: NSError) -> FlutterError {
        FlutterError(
            code: pluginErrorCode(for: error),
            message: error.localizedDescription,
            details: nil
        )
    }

    private func pluginErrorCode(for error: NSError) -> String {
        switch error.domain {
        case VideoDownloadErrorDomain, VideoTranscoderErrorDomain:
            return nativeTaskErrorCode(for: error.code)
        case AwesomeVideoKitErrorDomain:
            return goshFfmpegToolErrorCode(for: error.code)
        case AwesomeVideoFrameCaptureErrorDomain:
            return nativeTaskErrorCode(for: error.code)
        case AwesomeAVFileInfoErrorDomain:
            return avFileInfoErrorCode(for: error.code)
        default:
            return "NATIVE_ERROR"
        }
    }

    private func nativeTaskErrorCode(for code: Int) -> String {
        switch code {
        case 1:
            return "INVALID_ARGS"
        case 2:
            return "BUSY"
        case 3:
            return "FILE_EXISTS"
        case 4:
            return "CREATE_DIRECTORY_FAILED"
        case 5:
            return "FFMPEG_FAILED"
        case 6:
            return "CANCELLED"
        default:
            return "NATIVE_ERROR"
        }
    }

    private func avFileInfoErrorCode(for code: Int) -> String {
        switch code {
        case 1:
            return "INVALID_ARGS"
        case 2:
            return "LOAD_FAILED"
        default:
            return "NATIVE_ERROR"
        }
    }

    private func goshFfmpegToolErrorCode(for code: Int) -> String {
        switch code {
        case 1:
            return "INVALID_ARGS"
        case 2:
            return "BUSY"
        case 3:
            return "FILE_EXISTS"
        case 4:
            return "CREATE_DIRECTORY_FAILED"
        case 5:
            return "NO_AUDIO_STREAM"
        case 6:
            return "FFMPEG_FAILED"
        case 7:
            return "CANCELLED"
        default:
            return "NATIVE_ERROR"
        }
    }

    private func optionalStringArg(_ args: [String: Any], key: String) -> String? {
        normalizeOptionalString(args[key] as? String)
    }

    private func intArg(_ args: [String: Any], key: String, defaultValue: Int) -> Int {
        switch args[key] {
        case let value as Int:
            return value
        case let value as Int8:
            return Int(value)
        case let value as Int16:
            return Int(value)
        case let value as Int32:
            return Int(value)
        case let value as Int64:
            return Int(value)
        case let value as UInt:
            return Int(value)
        case let value as UInt8:
            return Int(value)
        case let value as UInt16:
            return Int(value)
        case let value as UInt32:
            return Int(value)
        case let value as UInt64:
            return Int(value)
        case let value as Float:
            return Int(value)
        case let value as Double:
            return Int(value)
        case let value as NSNumber:
            return value.intValue
        case let value as String:
            return Int(value) ?? defaultValue
        default:
            return defaultValue
        }
    }

    private func doubleArg(_ args: [String: Any], key: String, defaultValue: Double) -> Double {
        switch args[key] {
        case let value as Int:
            return Double(value)
        case let value as Int8:
            return Double(value)
        case let value as Int16:
            return Double(value)
        case let value as Int32:
            return Double(value)
        case let value as Int64:
            return Double(value)
        case let value as UInt:
            return Double(value)
        case let value as UInt8:
            return Double(value)
        case let value as UInt16:
            return Double(value)
        case let value as UInt32:
            return Double(value)
        case let value as UInt64:
            return Double(value)
        case let value as Float:
            return Double(value)
        case let value as Double:
            return value
        case let value as NSNumber:
            return value.doubleValue
        case let value as String:
            return Double(value) ?? defaultValue
        default:
            return defaultValue
        }
    }

    private func doubleListArg(_ args: [String: Any], key: String) -> [Double] {
        guard let values = args[key] as? [Any] else { return [] }
        return values.compactMap { value in
            switch value {
            case let value as Int:
                return Double(value)
            case let value as Int8:
                return Double(value)
            case let value as Int16:
                return Double(value)
            case let value as Int32:
                return Double(value)
            case let value as Int64:
                return Double(value)
            case let value as UInt:
                return Double(value)
            case let value as UInt8:
                return Double(value)
            case let value as UInt16:
                return Double(value)
            case let value as UInt32:
                return Double(value)
            case let value as UInt64:
                return Double(value)
            case let value as Float:
                return Double(value)
            case let value as Double:
                return value
            case let value as NSNumber:
                return value.doubleValue
            case let value as String:
                return Double(value)
            default:
                return nil
            }
        }
    }

    private func booleanArg(_ args: [String: Any], key: String, defaultValue: Bool) -> Bool {
        switch args[key] {
        case let value as Bool:
            return value
        case let value as NSNumber:
            return value.boolValue
        case let value as String:
            switch value.trimmingCharacters(in: .whitespacesAndNewlines).lowercased() {
            case "true", "1":
                return true
            case "false", "0":
                return false
            default:
                return defaultValue
            }
        default:
            return defaultValue
        }
    }

    private func watermarkPositionArg(_ args: [String: Any], key: String) -> VideoWatermarkPosition {
        let rawValue = intArg(args, key: key, defaultValue: VideoWatermarkPosition.bottomRight.rawValue)
        return VideoWatermarkPosition(rawValue: rawValue) ?? .bottomRight
    }

    private func concatPositionArg(_ args: [String: Any], key: String) -> VideoConcatImagePosition {
        let rawValue = intArg(args, key: key, defaultValue: VideoConcatImagePosition.tail.rawValue)
        return VideoConcatImagePosition(rawValue: rawValue) ?? .tail
    }

    private func normalizeOptionalString(_ value: String?) -> String? {
        let trimmed = value?.trimmingCharacters(in: .whitespacesAndNewlines)
        return (trimmed?.isEmpty == false) ? trimmed : nil
    }

    private func dispatchToMain(_ block: @escaping () -> Void) {
        if Thread.isMainThread {
            block()
        } else {
            DispatchQueue.main.async(execute: block)
        }
    }
}
