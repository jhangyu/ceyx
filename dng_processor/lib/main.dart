import 'dart:io';

import 'package:flutter/material.dart';
import 'package:file_picker/file_picker.dart';

import 'src/dng_decoder_service.dart';
import 'src/dng_image_widget.dart';

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
  String? _filePath;
  String? _error;
  bool _decoding = false;

  // Timing info
  double _decodeMs = 0;
  double _processMs = 0;

  @override
  void initState() {
    super.initState();
    try {
      _decoder.initialize();
    } catch (e) {
      _error = 'Failed to load native library: $e';
    }
  }

  Future<void> _pickAndDecode() async {
    // Pick a DNG file
    final result = await FilePicker.platform.pickFiles(
      type: FileType.custom,
      allowedExtensions: ['dng', 'DNG'],
      dialogTitle: 'Select a DNG file',
    );

    if (result == null || result.files.isEmpty) return;

    final path = result.files.single.path;
    if (path == null) return;

    setState(() {
      _decoding = true;
      _error = null;
      _image = null;
      _filePath = path;
    });

    try {
      final image = _decoder.decode(path);
      setState(() {
        _image = image;
        _decodeMs = image.decodeMs;
        _processMs = image.processMs;
        _decoding = false;
      });
    } on DngDecodeException catch (e) {
      setState(() {
        _error = e.toString();
        _decoding = false;
      });
    } catch (e) {
      setState(() {
        _error = 'Unexpected error: $e';
        _decoding = false;
      });
    }
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: const Text('DNG Processor'),
        centerTitle: true,
        actions: [
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
            _infoChip(Icons.timer_outlined,
                'Decode: ${_decodeMs.toStringAsFixed(0)}ms'),
            const SizedBox(width: 8),
            // Halide time
            _infoChip(Icons.bolt,
                'Halide: ${_processMs.toStringAsFixed(0)}ms'),
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
    if (_decoding) {
      return const Center(
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

    if (_error != null) {
      return Center(
        child: Padding(
          padding: const EdgeInsets.all(32),
          child: Column(
            mainAxisSize: MainAxisSize.min,
            children: [
              Icon(Icons.error_outline,
                  size: 48, color: Theme.of(context).colorScheme.error),
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
    }

    if (_image != null) {
      return InteractiveViewer(
        maxScale: 10.0,
        child: Center(
          child: DngImageWidget(
            dngImage: _image!,
            fit: BoxFit.contain,
          ),
        ),
      );
    }

    // Empty state
    return Center(
      child: Column(
        mainAxisSize: MainAxisSize.min,
        children: [
          Icon(Icons.camera,
              size: 64,
              color: Theme.of(context).colorScheme.onSurface.withAlpha(80)),
          const SizedBox(height: 16),
          Text(
            'Select a DNG file to decode',
            style: Theme.of(context).textTheme.titleMedium?.copyWith(
                  color:
                      Theme.of(context).colorScheme.onSurface.withAlpha(128),
                ),
          ),
        ],
      ),
    );
  }
}
