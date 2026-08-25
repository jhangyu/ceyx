import 'dart:async';
import 'dart:ui' as ui;

import 'package:flutter/material.dart';

import 'package:ceyx/ceyx.dart';

/// Converts a [DngImage] (RGBA buffer) to a Flutter [ui.Image].
Future<ui.Image> dngImageToUiImage(DngImage dngImage) {
  final completer = Completer<ui.Image>();
  ui.decodeImageFromPixels(
    dngImage.rgbaData,
    dngImage.width,
    dngImage.height,
    ui.PixelFormat.rgba8888,
    (ui.Image image) {
      completer.complete(image);
    },
  );
  return completer.future;
}

/// Widget that displays a [DngImage] by converting its RGBA buffer
/// to a Flutter [ui.Image] via [RawImage].
class DngImageWidget extends StatefulWidget {
  final DngImage dngImage;
  final BoxFit fit;

  const DngImageWidget({
    super.key,
    required this.dngImage,
    this.fit = BoxFit.contain,
  });

  @override
  State<DngImageWidget> createState() => _DngImageWidgetState();
}

class _DngImageWidgetState extends State<DngImageWidget> {
  ui.Image? _uiImage;
  bool _loading = true;

  @override
  void initState() {
    super.initState();
    _convertImage();
  }

  @override
  void didUpdateWidget(covariant DngImageWidget oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (oldWidget.dngImage != widget.dngImage) {
      _uiImage?.dispose();
      _uiImage = null;
      _loading = true;
      _convertImage();
    }
  }

  @override
  void dispose() {
    _uiImage?.dispose();
    super.dispose();
  }

  Future<void> _convertImage() async {
    final image = await dngImageToUiImage(widget.dngImage);
    if (mounted) {
      setState(() {
        _uiImage = image;
        _loading = false;
      });
    } else {
      image.dispose();
    }
  }

  @override
  Widget build(BuildContext context) {
    if (_loading || _uiImage == null) {
      return const Center(child: CircularProgressIndicator());
    }
    return RawImage(
      image: _uiImage,
      fit: widget.fit,
      filterQuality: FilterQuality.medium,
    );
  }
}
