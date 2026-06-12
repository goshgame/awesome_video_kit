import 'dart:async';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:awesome_video_kit/awesome_video_kit.dart';

void main() {
  runApp(const MyApp());
}

class MyApp extends StatelessWidget {
  const MyApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      debugShowCheckedModeBanner: false,
      theme: ThemeData(
        colorScheme: ColorScheme.fromSeed(seedColor: const Color(0xFF0F766E)),
        useMaterial3: true,
      ),
      home: const PluginDemoPage(),
    );
  }
}

class PluginDemoPage extends StatefulWidget {
  const PluginDemoPage({super.key});

  @override
  State<PluginDemoPage> createState() => _PluginDemoPageState();
}

class _PluginDemoPageState extends State<PluginDemoPage> {
  final _downloadUrlController = TextEditingController();
  final _downloadOutputPathController = TextEditingController();
  final _transcodeInputPathController = TextEditingController();
  final _transcodeOutputPathController = TextEditingController();
  final _watermarkPathController = TextEditingController();
  final _crfController = TextEditingController(text: '23');
  final _videoWidthController = TextEditingController(text: '720');
  final _videoHeightController = TextEditingController(text: '0');
  final _audioBitrateController = TextEditingController(text: '128000');
  final _presetController = TextEditingController(text: 'fast');
  final _profileController = TextEditingController(text: 'high');
  final _levelController = TextEditingController(text: '3.1');

  StreamSubscription<int>? _downloadProgressSubscription;
  StreamSubscription<int>? _transcodeProgressSubscription;

  bool _downloadOverwrite = true;
  bool _transcodeOverwrite = true;
  bool _useBundledWatermark = true;
  bool _faststart = true;

  int _downloadProgress = 0;
  int _transcodeProgress = 0;
  bool _isDownloading = false;
  bool _isTranscoding = false;
  String _platformVersion = '加载中...';
  String? _preparedWatermarkPath;
  VideoWatermarkPosition _watermarkPosition =
      VideoWatermarkPosition.bottomRight;
  final List<String> _logs = <String>[];

  bool get _transcodeSupported => true;

  @override
  void initState() {
    super.initState();
    _downloadProgressSubscription = AwesomeVideoKit.downloadProgressStream
        .listen(
          (progress) {
            if (!mounted) return;
            setState(() {
              _downloadProgress = progress;
            });
          },
          onError: (Object error, StackTrace stackTrace) {
            _appendLog('下载进度流异常：$error');
          },
        );
    _transcodeProgressSubscription = AwesomeVideoKit.transcodeProgressStream
        .listen(
          (progress) {
            if (!mounted) return;
            setState(() {
              _transcodeProgress = progress;
            });
          },
          onError: (Object error, StackTrace stackTrace) {
            _appendLog('转码进度流异常：$error');
          },
        );
    unawaited(_bootstrap());
  }

  @override
  void dispose() {
    _downloadProgressSubscription?.cancel();
    _transcodeProgressSubscription?.cancel();
    _downloadUrlController.dispose();
    _downloadOutputPathController.dispose();
    _transcodeInputPathController.dispose();
    _transcodeOutputPathController.dispose();
    _watermarkPathController.dispose();
    _crfController.dispose();
    _videoWidthController.dispose();
    _videoHeightController.dispose();
    _audioBitrateController.dispose();
    _presetController.dispose();
    _profileController.dispose();
    _levelController.dispose();
    super.dispose();
  }

  Future<void> _bootstrap() async {
    await _refreshPlatformVersion();
    await _refreshNativeState();
  }

  Future<void> _refreshPlatformVersion() async {
    try {
      final version = await AwesomeVideoKit.getPlatformVersion();
      if (!mounted) return;
      setState(() {
        _platformVersion = version ?? '未知';
      });
    } catch (error) {
      _appendLog('获取平台版本失败：$error');
    }
  }

  Future<void> _refreshNativeState() async {
    try {
      final downloading = await AwesomeVideoKit.isDownloading();
      final transcoding = await AwesomeVideoKit.isTranscoding();
      if (!mounted) return;
      setState(() {
        _isDownloading = downloading;
        _isTranscoding = transcoding;
      });
    } catch (error) {
      _appendLog('刷新原生状态失败：$error');
    }
  }

  Future<String> _prepareBundledWatermark() async {
    final cachedPath = _preparedWatermarkPath;
    if (cachedPath != null && await File(cachedPath).exists()) {
      return cachedPath;
    }

    final data = await rootBundle.load('assets/watermark.png');
    final tempDir = await Directory.systemTemp.createTemp(
      'awesome_video_kit_example_',
    );
    final file = File('${tempDir.path}/watermark.png');
    await file.writeAsBytes(
      data.buffer.asUint8List(data.offsetInBytes, data.lengthInBytes),
      flush: true,
    );
    _preparedWatermarkPath = file.path;
    _appendLog('已准备内置水印文件：${file.path}');
    return file.path;
  }

  Future<String?> _resolveWatermarkPath({required bool forceWatermark}) async {
    final manualPath = _watermarkPathController.text.trim();
    if (manualPath.isNotEmpty) return manualPath;
    if (!forceWatermark) return null;
    if (_useBundledWatermark) {
      return _prepareBundledWatermark();
    }
    return null;
  }

  Future<void> _downloadVideo({required bool withWatermark}) async {
    final url = _downloadUrlController.text.trim();
    if (url.isEmpty) {
      _appendLog('下载已中止：缺少 m3u8 地址');
      return;
    }

    final watermarkPath = await _resolveWatermarkPath(
      forceWatermark: withWatermark,
    );
    if (withWatermark && (watermarkPath == null || watermarkPath.isEmpty)) {
      _appendLog('下载水印测试已中止：缺少水印路径');
      return;
    }

    setState(() {
      _downloadProgress = 0;
      _isDownloading = true;
    });

    try {
      final output = await AwesomeVideoKit.downloadVideo(
        url: url,
        outputPath: _normalizedOrNull(_downloadOutputPathController.text),
        overwrite: _downloadOverwrite,
        watermarkImagePath: watermarkPath,
        watermarkPosition: _watermarkPosition,
      );
      if (!mounted) return;
      setState(() {
        _downloadProgress = 100;
        _isDownloading = false;
        _transcodeInputPathController.text = output;
      });
      _appendLog('下载成功：$output');
      await _refreshNativeState();
    } on PlatformException catch (error) {
      if (!mounted) return;
      setState(() {
        _isDownloading = false;
      });
      _appendLog('下载失败：${error.code} ${error.message}');
      await _refreshNativeState();
    } catch (error) {
      if (!mounted) return;
      setState(() {
        _isDownloading = false;
      });
      _appendLog('下载失败：$error');
      await _refreshNativeState();
    }
  }

  Future<void> _transcodeVideo({required bool withWatermark}) async {
    if (!_transcodeSupported) {
      _appendLog('当前打包的 iOS 原生库未暴露转码能力。');
      return;
    }

    final inputPath = _transcodeInputPathController.text.trim();
    if (inputPath.isEmpty) {
      _appendLog('转码已中止：缺少输入视频路径');
      return;
    }

    final watermarkPath = await _resolveWatermarkPath(
      forceWatermark: withWatermark,
    );
    if (withWatermark && (watermarkPath == null || watermarkPath.isEmpty)) {
      _appendLog('转码水印测试已中止：缺少水印路径');
      return;
    }

    setState(() {
      _transcodeProgress = 0;
      _isTranscoding = true;
    });

    try {
      final output = await AwesomeVideoKit.transcodeVideo(
        inputPath: inputPath,
        outputPath: _normalizedOrNull(_transcodeOutputPathController.text),
        overwrite: _transcodeOverwrite,
        options: VideoTranscodeOptions(
          crf: _parseInt(_crfController.text, 23),
          preset: _normalizedOrNull(_presetController.text),
          profile: _normalizedOrNull(_profileController.text),
          level: _normalizedOrNull(_levelController.text),
          videoWidth: _parseInt(_videoWidthController.text, 720),
          videoHeight: _parseInt(_videoHeightController.text, 0),
          audioBitrate: _parseInt(_audioBitrateController.text, 128000),
          faststart: _faststart,
          watermarkImagePath: watermarkPath,
          watermarkPosition: _watermarkPosition,
        ),
      );
      if (!mounted) return;
      setState(() {
        _transcodeProgress = 100;
        _isTranscoding = false;
      });
      _appendLog('转码成功：$output');
      await _refreshNativeState();
    } on PlatformException catch (error) {
      if (!mounted) return;
      setState(() {
        _isTranscoding = false;
      });
      _appendLog('转码失败：${error.code} ${error.message}');
      await _refreshNativeState();
    } catch (error) {
      if (!mounted) return;
      setState(() {
        _isTranscoding = false;
      });
      _appendLog('转码失败：$error');
      await _refreshNativeState();
    }
  }

  Future<void> _cancelDownload() async {
    try {
      await AwesomeVideoKit.cancelDownload();
      _appendLog('已请求取消下载');
    } catch (error) {
      _appendLog('取消下载失败：$error');
    } finally {
      await _refreshNativeState();
    }
  }

  Future<void> _cancelTranscode() async {
    try {
      await AwesomeVideoKit.cancelTranscode();
      _appendLog('已请求取消转码');
    } catch (error) {
      _appendLog('取消转码失败：$error');
    } finally {
      await _refreshNativeState();
    }
  }

  void _appendLog(String message) {
    final timestamp = DateTime.now().toIso8601String().substring(11, 19);
    if (!mounted) {
      _logs.insert(0, '[$timestamp] $message');
      return;
    }
    setState(() {
      _logs.insert(0, '[$timestamp] $message');
      if (_logs.length > 30) {
        _logs.removeLast();
      }
    });
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: const Text('FFmpeg 工具示例')),
      body: SafeArea(
        child: ListView(
          padding: const EdgeInsets.all(16),
          children: [
            _buildHeaderCard(),
            const SizedBox(height: 16),
            _buildDownloadSection(),
            const SizedBox(height: 16),
            _buildTranscodeSection(),
            const SizedBox(height: 16),
            _buildLogSection(),
          ],
        ),
      ),
    );
  }

  Widget _buildHeaderCard() {
    return Card(
      child: Padding(
        padding: const EdgeInsets.all(16),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Text('原生状态', style: Theme.of(context).textTheme.titleMedium),
            const SizedBox(height: 8),
            Text('平台版本：$_platformVersion'),
            const SizedBox(height: 8),
            Wrap(
              spacing: 8,
              runSpacing: 8,
              children: [
                Chip(
                  label: Text(
                    _isDownloading ? '下载中 $_downloadProgress%' : '下载空闲',
                  ),
                ),
                Chip(
                  label: Text(
                    _isTranscoding ? '转码中 $_transcodeProgress%' : '转码空闲',
                  ),
                ),
                if (!_transcodeSupported)
                  const Chip(label: Text('iOS 包暂不支持转码')),
              ],
            ),
            const SizedBox(height: 12),
            Wrap(
              spacing: 12,
              runSpacing: 12,
              children: [
                FilledButton.tonal(
                  onPressed: _refreshPlatformVersion,
                  child: const Text('刷新平台版本'),
                ),
                FilledButton.tonal(
                  onPressed: _refreshNativeState,
                  child: const Text('刷新原生状态'),
                ),
                FilledButton.tonal(
                  onPressed: () async {
                    final path = await _prepareBundledWatermark();
                    if (!mounted) return;
                    _watermarkPathController.text = path;
                  },
                  child: const Text('准备内置水印'),
                ),
              ],
            ),
          ],
        ),
      ),
    );
  }

  Widget _buildDownloadSection() {
    return Card(
      child: Padding(
        padding: const EdgeInsets.all(16),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Text('下载测试', style: Theme.of(context).textTheme.titleMedium),
            const SizedBox(height: 12),
            TextField(
              controller: _downloadUrlController,
              decoration: const InputDecoration(
                labelText: 'm3u8 地址',
                hintText: '输入可访问的 m3u8 地址',
                border: OutlineInputBorder(),
              ),
            ),
            const SizedBox(height: 12),
            TextField(
              controller: _downloadOutputPathController,
              decoration: const InputDecoration(
                labelText: '下载输出路径',
                hintText: '留空则由原生层自动生成 mp4 路径',
                border: OutlineInputBorder(),
              ),
            ),
            const SizedBox(height: 12),
            _buildWatermarkControls(),
            SwitchListTile.adaptive(
              contentPadding: EdgeInsets.zero,
              title: const Text('覆盖已有输出文件'),
              value: _downloadOverwrite,
              onChanged: (value) {
                setState(() {
                  _downloadOverwrite = value;
                });
              },
            ),
            const SizedBox(height: 8),
            LinearProgressIndicator(value: _downloadProgress / 100),
            const SizedBox(height: 8),
            Text('下载进度：$_downloadProgress%'),
            const SizedBox(height: 12),
            Wrap(
              spacing: 12,
              runSpacing: 12,
              children: [
                FilledButton(
                  onPressed: () => _downloadVideo(withWatermark: false),
                  child: const Text('开始下载'),
                ),
                FilledButton.tonal(
                  onPressed: () => _downloadVideo(withWatermark: true),
                  child: const Text('下载并添加水印'),
                ),
                OutlinedButton(
                  onPressed: _cancelDownload,
                  child: const Text('取消下载'),
                ),
              ],
            ),
          ],
        ),
      ),
    );
  }

  Widget _buildTranscodeSection() {
    return Card(
      child: Padding(
        padding: const EdgeInsets.all(16),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Text('转码测试', style: Theme.of(context).textTheme.titleMedium),
            const SizedBox(height: 12),
            TextField(
              controller: _transcodeInputPathController,
              decoration: const InputDecoration(
                labelText: '输入视频路径',
                hintText: '可直接使用上面下载成功后的输出路径',
                border: OutlineInputBorder(),
              ),
            ),
            const SizedBox(height: 12),
            TextField(
              controller: _transcodeOutputPathController,
              decoration: const InputDecoration(
                labelText: '转码输出路径',
                hintText: '留空则由原生层自动生成 mp4 路径',
                border: OutlineInputBorder(),
              ),
            ),
            const SizedBox(height: 12),
            _buildWatermarkControls(),
            const SizedBox(height: 12),
            Row(
              children: [
                Expanded(
                  child: TextField(
                    controller: _crfController,
                    keyboardType: TextInputType.number,
                    decoration: const InputDecoration(
                      labelText: 'CRF 值',
                      border: OutlineInputBorder(),
                    ),
                  ),
                ),
                const SizedBox(width: 12),
                Expanded(
                  child: TextField(
                    controller: _audioBitrateController,
                    keyboardType: TextInputType.number,
                    decoration: const InputDecoration(
                      labelText: '音频码率',
                      border: OutlineInputBorder(),
                    ),
                  ),
                ),
              ],
            ),
            const SizedBox(height: 12),
            Row(
              children: [
                Expanded(
                  child: TextField(
                    controller: _videoWidthController,
                    keyboardType: TextInputType.number,
                    decoration: const InputDecoration(
                      labelText: '视频宽度',
                      border: OutlineInputBorder(),
                    ),
                  ),
                ),
                const SizedBox(width: 12),
                Expanded(
                  child: TextField(
                    controller: _videoHeightController,
                    keyboardType: TextInputType.number,
                    decoration: const InputDecoration(
                      labelText: '视频高度',
                      border: OutlineInputBorder(),
                    ),
                  ),
                ),
              ],
            ),
            const SizedBox(height: 12),
            Row(
              children: [
                Expanded(
                  child: TextField(
                    controller: _presetController,
                    decoration: const InputDecoration(
                      labelText: '编码预设',
                      border: OutlineInputBorder(),
                    ),
                  ),
                ),
                const SizedBox(width: 12),
                Expanded(
                  child: TextField(
                    controller: _profileController,
                    decoration: const InputDecoration(
                      labelText: '编码档位',
                      border: OutlineInputBorder(),
                    ),
                  ),
                ),
                const SizedBox(width: 12),
                Expanded(
                  child: TextField(
                    controller: _levelController,
                    decoration: const InputDecoration(
                      labelText: '编码级别',
                      border: OutlineInputBorder(),
                    ),
                  ),
                ),
              ],
            ),
            SwitchListTile.adaptive(
              contentPadding: EdgeInsets.zero,
              title: const Text('覆盖已有输出文件'),
              value: _transcodeOverwrite,
              onChanged: (value) {
                setState(() {
                  _transcodeOverwrite = value;
                });
              },
            ),
            SwitchListTile.adaptive(
              contentPadding: EdgeInsets.zero,
              title: const Text('启用快速起播'),
              value: _faststart,
              onChanged: (value) {
                setState(() {
                  _faststart = value;
                });
              },
            ),
            const SizedBox(height: 8),
            LinearProgressIndicator(value: _transcodeProgress / 100),
            const SizedBox(height: 8),
            Text('转码进度：$_transcodeProgress%'),
            const SizedBox(height: 12),
            Wrap(
              spacing: 12,
              runSpacing: 12,
              children: [
                FilledButton(
                  onPressed: () => _transcodeVideo(withWatermark: false),
                  child: const Text('开始转码'),
                ),
                FilledButton.tonal(
                  onPressed: () => _transcodeVideo(withWatermark: true),
                  child: const Text('转码并添加水印'),
                ),
                OutlinedButton(
                  onPressed: _cancelTranscode,
                  child: const Text('取消转码'),
                ),
              ],
            ),
          ],
        ),
      ),
    );
  }

  Widget _buildWatermarkControls() {
    return Column(
      children: [
        TextField(
          controller: _watermarkPathController,
          decoration: const InputDecoration(
            labelText: '水印图片路径',
            hintText: '留空时可使用内置 assets/watermark.png',
            border: OutlineInputBorder(),
          ),
        ),
        SwitchListTile.adaptive(
          contentPadding: EdgeInsets.zero,
          title: const Text('路径为空时使用内置水印'),
          subtitle: Text(_preparedWatermarkPath ?? '尚未准备内置水印文件'),
          value: _useBundledWatermark,
          onChanged: (value) {
            setState(() {
              _useBundledWatermark = value;
            });
          },
        ),
        DropdownButtonFormField<VideoWatermarkPosition>(
          value: _watermarkPosition,
          decoration: const InputDecoration(
            labelText: '水印位置',
            border: OutlineInputBorder(),
          ),
          items: VideoWatermarkPosition.values
              .map(
                (position) => DropdownMenuItem<VideoWatermarkPosition>(
                  value: position,
                  child: Text(_watermarkLabel(position)),
                ),
              )
              .toList(),
          onChanged: (value) {
            if (value == null) return;
            setState(() {
              _watermarkPosition = value;
            });
          },
        ),
      ],
    );
  }

  Widget _buildLogSection() {
    return Card(
      child: Padding(
        padding: const EdgeInsets.all(16),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Text('运行日志', style: Theme.of(context).textTheme.titleMedium),
            const SizedBox(height: 12),
            Container(
              width: double.infinity,
              constraints: const BoxConstraints(minHeight: 160),
              padding: const EdgeInsets.all(12),
              decoration: BoxDecoration(
                color: const Color.fromRGBO(0, 0, 0, 0.04),
                borderRadius: BorderRadius.circular(12),
              ),
              child: SelectableText(
                _logs.isEmpty ? '暂无日志' : _logs.join('\n'),
                style: const TextStyle(fontFamily: 'monospace'),
              ),
            ),
          ],
        ),
      ),
    );
  }

  String _watermarkLabel(VideoWatermarkPosition position) {
    switch (position) {
      case VideoWatermarkPosition.topLeft:
        return '左上';
      case VideoWatermarkPosition.topRight:
        return '右上';
      case VideoWatermarkPosition.bottomLeft:
        return '左下';
      case VideoWatermarkPosition.bottomRight:
        return '右下';
      case VideoWatermarkPosition.center:
        return '居中';
      case VideoWatermarkPosition.alternatingTopLeftBottomRight:
        return '左上/右下交替';
    }
  }

  String? _normalizedOrNull(String value) {
    final trimmed = value.trim();
    if (trimmed.isEmpty) return null;
    return trimmed;
  }

  int _parseInt(String value, int fallback) {
    return int.tryParse(value.trim()) ?? fallback;
  }
}
