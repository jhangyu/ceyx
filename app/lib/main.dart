import 'dart:io';
import 'dart:typed_data';
import 'dart:ui' as ui;

import 'package:flutter/material.dart';
import 'package:file_picker/file_picker.dart';
import 'package:path_provider/path_provider.dart';

import 'ceyx_example.dart';

/*
---
file_summary: "App 進入點與首頁 UI，處理檔案選擇、狀態顯示與影像呈現"
modules:
  - name: "App Entry"
    description: "MaterialApp 與主題配置"
    lines: "28-50"
  - name: "DngHomePage"
    description: "首頁狀態管理、檔案選擇邏輯與解碼服務呼叫；_saving/_decoding 分離"
    lines: "52-185"
  - name: "UI Widgets"
    description: "主要的 Scaffold、狀態列顯示與 ImageViewer"
    lines: "187-415"
---
*/

void main() {
  runApp(const DngProcessorApp());
}

class DngProcessorApp extends StatelessWidget {
  const DngProcessorApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'DNG Processor',
      debugShowCheckedModeBanner: false,
      theme: ThemeData(
        colorScheme: ColorScheme.fromSeed(
          seedColor: Colors.teal,
          brightness: Brightness.dark,
        ),
        useMaterial3: true,
      ),
      home: const DngHomePage(),
    );
  }
}

class DngHomePage extends StatefulWidget {
  const DngHomePage({super.key});

  @override
  State<DngHomePage> createState() => _DngHomePageState();
}

class _DngHomePageState extends State<DngHomePage> {
  final DngDecoderService _decoder = DngDecoderService();

  DngImage? _image;
  Uint8List? _previewBytes;
  bool _showingPreview = false;
  String? _filePath;
  String? _error;
  bool _decoding = false;
  // Separate flag for save-screenshot operation so it does not affect
  // the image-area overlay or the Open-file button (W2-3).
  bool _saving = false;

  // Timing info
  double _decodeMs = 0;
  double _processMs = 0;

  @override
  void initState() {
    super.initState();
    try {
      _decoder.initialize();
      WidgetsBinding.instance.addPostFrameCallback((_) {
        _startupWarmup();
      });
    } catch (e) {
      _error = 'Failed to load native library: $e';
    }
  }

  /// R3-3: startup warmup with VkPipelineCache persistence (Android only).
  /// The cache path MUST be set before warmup so the warmup's pipeline
  /// compilation hits the persisted cache from the second launch onward
  /// (and its results are saved for the next launch).
  Future<void> _startupWarmup() async {
    if (Platform.isAndroid) {
      try {
        final cacheDir = await getApplicationCacheDirectory();
        final rc = _decoder.setPipelineCachePath(
          '${cacheDir.path}/dng_vk_pipeline.cache',
        );
        debugPrint(
          'DNG pipeline cache path set rc=$rc '
          'status=${_decoder.pipelineCacheStatus}',
        );
      } catch (e) {
        // Cache setup failure must never block warmup/decoding.
        debugPrint('DNG pipeline cache setup failed (non-fatal): $e');
      }
    }
    try {
      await _decoder.warmupForSize();
      if (Platform.isAndroid) {
        // Evidence hook: bit 4 = cache file was loaded this launch (hit).
        debugPrint(
          'DNG warmup done, pipeline cache status='
          '${_decoder.pipelineCacheStatus}',
        );
      }
    } catch (e) {
      debugPrint('DNG warmup failed: $e');
    }
  }

  Future<void> _pickAndDecode() async {
    // Pick a RAW or DNG file. The accepted list is derived from
    // kSupportedDecodeExtensions (plugin/lib/src/raw_route.dart)
    // so the picker can never drift from the decoder's routing table.
    final result = await FilePicker.platform.pickFiles(
      type: FileType.custom,
      allowedExtensions: <String>[
        for (final ext in kSupportedDecodeExtensions) ...<String>[
          ext,
          ext.toUpperCase(),
        ],
      ],
      dialogTitle: 'Select a RAW or DNG file',
    );

    if (result == null || result.files.isEmpty) return;

    final path = result.files.single.path;
    if (path == null) return;

    setState(() {
      _decoding = true;
      _error = null;
      _image = null;
      _previewBytes = null;
      _showingPreview = false;
      _filePath = path;
    });

    // Attempt the fast preview JPEG first — runs on a worker isolate so the
    // UI can render the spinner frame before the FFI call.
    //
    // DNG only: dng_extract_preview_jpeg is a DNG SDK entry with no
    // generic-RAW equivalent in Phase 18, so for a RAW file it would spend a
    // worker isolate just to return null.
    if (decodeRouteForPath(path) == DecodeRoute.dng) {
      try {
        final preview = await _decoder.getPreviewJpegOnWorker(path);
        if (preview != null) {
          setState(() {
            _previewBytes = preview;
            _showingPreview = true;
          });
        }
      } catch (e) {
        debugPrint('Failed to extract preview: $e');
      }
    }

    // Process the full RAW image
    try {
      // Use Future.microtask or Future.delayed to allow the UI to render the preview first
      await Future.delayed(const Duration(milliseconds: 16));

      final image = await _decoder.decodeOnWorker(path);
      if (!mounted) return;
      setState(() {
        _image = image;
        _decodeMs = image.decodeMs;
        _processMs = image.processMs;
        _decoding = false;
        _showingPreview = false;
      });
    } on DngDecodeException catch (e) {
      if (!mounted) return;
      setState(() {
        _error = e.toString();
        _decoding = false;
        _showingPreview = false;
      });
    } catch (e) {
      if (!mounted) return;
      setState(() {
        _error = 'Unexpected error: $e';
        _decoding = false;
        _showingPreview = false;
      });
    }
  }

  Future<void> _saveScreenshot() async {
    if (_image == null) return;

    // Use _saving (not _decoding) so the full-image display stays visible
    // and the Open-file button is not disabled during the save operation.
    setState(() => _saving = true);
    try {
      final uiImage = await dngImageToUiImage(_image!);
      final byteData = await uiImage.toByteData(format: ui.ImageByteFormat.png);
      if (byteData != null) {
        final bytes = byteData.buffer.asUint8List();
        final dir = await getApplicationDocumentsDirectory();
        final timestamp = DateTime.now().toIso8601String().replaceAll(':', '-');
        final file = File('${dir.path}/dng_screenshot_$timestamp.png');
        await file.writeAsBytes(bytes);

        if (mounted) {
          ScaffoldMessenger.of(context).showSnackBar(
            SnackBar(content: Text('Screenshot saved to ${file.path}')),
          );
        }
      }
    } catch (e) {
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(content: Text('Failed to save screenshot: $e')),
        );
      }
    } finally {
      if (mounted) setState(() => _saving = false);
    }
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: const Text('DNG Processor'),
        centerTitle: true,
        actions: [
          if (_image != null)
            IconButton(
              icon: const Icon(Icons.save_alt),
              tooltip: 'Save Screenshot',
              // Disable while decoding (image about to change) or while
              // a save is already in progress.
              onPressed: (_decoding || _saving) ? null : _saveScreenshot,
            ),
          IconButton(
            icon: const Icon(Icons.folder_open),
            tooltip: 'Open DNG file',
            onPressed: _decoding ? null : _pickAndDecode,
          ),
        ],
      ),
      body: Column(
        children: [
          // Status bar
          _buildStatusBar(),
          // Image display
          Expanded(child: _buildImageArea()),
        ],
      ),
      floatingActionButton: FloatingActionButton.extended(
        onPressed: _decoding ? null : _pickAndDecode,
        icon: const Icon(Icons.image),
        label: const Text('Select DNG'),
      ),
    );
  }

  Widget _buildStatusBar() {
    return Container(
      width: double.infinity,
      padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 8),
      color: Theme.of(context).colorScheme.surfaceContainerHighest,
      child: Row(
        children: [
          // File name
          Expanded(
            child: Text(
              _filePath != null
                  ? File(_filePath!).uri.pathSegments.last
                  : 'No file selected',
              style: Theme.of(context).textTheme.bodyMedium,
              overflow: TextOverflow.ellipsis,
            ),
          ),
          if (_image != null) ...[
            const SizedBox(width: 16),
            // Image dimensions
            _infoChip(
              Icons.photo_size_select_actual,
              '${_image!.width}×${_image!.height}',
            ),
            const SizedBox(width: 8),
            // Decode time
            _infoChip(
              Icons.timer_outlined,
              'Decode: ${_decodeMs.toStringAsFixed(0)}ms',
            ),
            const SizedBox(width: 8),
            // Halide time
            _infoChip(Icons.bolt, 'Halide: ${_processMs.toStringAsFixed(0)}ms'),
            const SizedBox(width: 8),
            // Total time
            _infoChip(
              Icons.schedule,
              'Total: ${_image!.totalMs.toStringAsFixed(0)}ms',
            ),
          ],
        ],
      ),
    );
  }

  Widget _infoChip(IconData icon, String label) {
    return Row(
      mainAxisSize: MainAxisSize.min,
      children: [
        Icon(icon, size: 14, color: Theme.of(context).colorScheme.primary),
        const SizedBox(width: 4),
        Text(label, style: Theme.of(context).textTheme.bodySmall),
      ],
    );
  }

  Widget _buildImageArea() {
    Widget content;
    if (_decoding) {
      if (_showingPreview && _previewBytes != null) {
        content = Stack(
          key: const ValueKey('preview'),
          fit: StackFit.expand,
          children: [
            InteractiveViewer(
              maxScale: 10.0,
              child: Center(
                child: Image.memory(
                  _previewBytes!,
                  fit: BoxFit.contain,
                  gaplessPlayback: true,
                ),
              ),
            ),
            Positioned(
              bottom: 32,
              left: 0,
              right: 0,
              child: Center(
                child: Container(
                  padding: const EdgeInsets.symmetric(
                    horizontal: 24,
                    vertical: 12,
                  ),
                  decoration: BoxDecoration(
                    color: Colors.black.withAlpha(200),
                    borderRadius: BorderRadius.circular(32),
                    boxShadow: [
                      BoxShadow(
                        color: Colors.black.withAlpha(50),
                        blurRadius: 8,
                        offset: const Offset(0, 4),
                      ),
                    ],
                  ),
                  child: Row(
                    mainAxisSize: MainAxisSize.min,
                    children: [
                      const SizedBox(
                        width: 20,
                        height: 20,
                        child: CircularProgressIndicator(strokeWidth: 2),
                      ),
                      const SizedBox(width: 16),
                      Text(
                        'Loading full RAW...',
                        style: Theme.of(context).textTheme.bodyMedium?.copyWith(
                          color: Colors.white,
                          fontWeight: FontWeight.w500,
                        ),
                      ),
                    ],
                  ),
                ),
              ),
            ),
          ],
        );
      } else {
        content = const Center(
          key: ValueKey('decoding'),
          child: Column(
            mainAxisSize: MainAxisSize.min,
            children: [
              CircularProgressIndicator(),
              SizedBox(height: 16),
              Text('Decoding DNG...'),
            ],
          ),
        );
      }
    } else if (_error != null) {
      content = Center(
        key: const ValueKey('error'),
        child: Padding(
          padding: const EdgeInsets.all(32),
          child: Column(
            mainAxisSize: MainAxisSize.min,
            children: [
              Icon(
                Icons.error_outline,
                size: 48,
                color: Theme.of(context).colorScheme.error,
              ),
              const SizedBox(height: 16),
              Text(
                _error!,
                style: TextStyle(color: Theme.of(context).colorScheme.error),
                textAlign: TextAlign.center,
              ),
            ],
          ),
        ),
      );
    } else if (_image != null) {
      content = InteractiveViewer(
        key: const ValueKey('full_image'),
        maxScale: 10.0,
        child: Center(
          child: DngImageWidget(dngImage: _image!, fit: BoxFit.contain),
        ),
      );
    } else {
      content = Center(
        key: const ValueKey('empty'),
        child: Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            Icon(
              Icons.camera,
              size: 64,
              color: Theme.of(context).colorScheme.onSurface.withAlpha(80),
            ),
            const SizedBox(height: 16),
            Text(
              'Select a DNG file to decode',
              style: Theme.of(context).textTheme.titleMedium?.copyWith(
                color: Theme.of(context).colorScheme.onSurface.withAlpha(128),
              ),
            ),
          ],
        ),
      );
    }

    return AnimatedSwitcher(
      duration: const Duration(milliseconds: 300),
      switchInCurve: Curves.easeOut,
      switchOutCurve: Curves.easeIn,
      child: content,
    );
  }
}
