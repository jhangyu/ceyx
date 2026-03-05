#!/usr/bin/env python3
"""
Visual Regression Test: Compare DNG SDK rendered output with Adobe reference JPEG.
Computes PSNR and SSIM. Standard: PSNR > 35dB = no visible color shift.

Usage: python3 compare_psnr.py <rendered.rgb> <reference.jpg>
"""
import sys
import struct
import math
import os

def load_rgb_file(path):
    """Load raw RGB image with 8-byte header (width:u32, height:u32)"""
    with open(path, 'rb') as f:
        w = struct.unpack('<I', f.read(4))[0]
        h = struct.unpack('<I', f.read(4))[0]
        data = f.read()
    expected = w * h * 3
    if len(data) != expected:
        print(f"ERROR: RGB file size mismatch: {len(data)} vs expected {expected}")
        sys.exit(1)
    return w, h, data

def load_jpeg(path):
    """Load JPEG using PIL"""
    try:
        from PIL import Image
        img = Image.open(path).convert('RGB')
        w, h = img.size
        data = img.tobytes()
        return w, h, data
    except ImportError:
        print("ERROR: Pillow not installed. Run: pip3 install Pillow")
        sys.exit(1)

def compute_psnr(data1, data2, w, h):
    """Compute PSNR between two image byte arrays"""
    assert len(data1) == len(data2), f"Size mismatch: {len(data1)} vs {len(data2)}"
    mse = 0.0
    n = len(data1)
    for i in range(n):
        diff = data1[i] - data2[i]
        mse += diff * diff
    mse /= n
    if mse == 0:
        return float('inf')
    return 10.0 * math.log10(255.0 * 255.0 / mse)

def compute_psnr_numpy(data1, data2):
    """Compute PSNR using numpy for speed"""
    import numpy as np
    a1 = np.frombuffer(data1, dtype=np.uint8).astype(np.float64)
    a2 = np.frombuffer(data2, dtype=np.uint8).astype(np.float64)
    mse = np.mean((a1 - a2) ** 2)
    if mse == 0:
        return float('inf')
    return 10.0 * math.log10(255.0 * 255.0 / mse)

def compute_ssim_numpy(data1, data2, w, h):
    """Compute mean SSIM per-channel"""
    import numpy as np
    a1 = np.frombuffer(data1, dtype=np.uint8).reshape(h, w, 3).astype(np.float64)
    a2 = np.frombuffer(data2, dtype=np.uint8).reshape(h, w, 3).astype(np.float64)

    C1 = (0.01 * 255) ** 2
    C2 = (0.03 * 255) ** 2

    # Block-based SSIM with 8x8 blocks
    block = 8
    ssim_sum = 0.0
    count = 0
    for y in range(0, h - block + 1, block):
        for x in range(0, w - block + 1, block):
            for c in range(3):
                b1 = a1[y:y+block, x:x+block, c]
                b2 = a2[y:y+block, x:x+block, c]
                mu1 = np.mean(b1)
                mu2 = np.mean(b2)
                s1 = np.var(b1)
                s2 = np.var(b2)
                s12 = np.mean((b1 - mu1) * (b2 - mu2))
                num = (2*mu1*mu2 + C1) * (2*s12 + C2)
                den = (mu1**2 + mu2**2 + C1) * (s1 + s2 + C2)
                ssim_sum += num / den
                count += 1
    return ssim_sum / count if count > 0 else 0.0

def main():
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <rendered.rgb> <reference.jpg>")
        sys.exit(1)

    rgb_path = sys.argv[1]
    jpg_path = sys.argv[2]

    print("=" * 60)
    print("  Visual Regression Test — PSNR / SSIM")
    print("=" * 60)

    # Load rendered image
    rw, rh, rdata = load_rgb_file(rgb_path)
    print(f"\n  Rendered: {rw}x{rh} ({len(rdata)} bytes)")

    # Load reference JPEG
    jw, jh, jdata = load_jpeg(jpg_path)
    print(f"  Reference: {jw}x{jh} ({len(jdata)} bytes)")

    # Check if dimensions match — if not, resize reference to match
    if rw != jw or rh != jh:
        print(f"\n  ⚠ Dimension mismatch! Resizing reference {jw}x{jh} → {rw}x{rh}")
        from PIL import Image
        img = Image.open(jpg_path).convert('RGB').resize((rw, rh), Image.LANCZOS)
        jdata = img.tobytes()
        jw, jh = rw, rh

    # Compute PSNR
    try:
        import numpy as np
        psnr = compute_psnr_numpy(rdata, jdata)
        ssim = compute_ssim_numpy(rdata, jdata, rw, rh)
        has_ssim = True
    except ImportError:
        psnr = compute_psnr(rdata, jdata, rw, rh)
        ssim = None
        has_ssim = False

    print(f"\n{'=' * 60}")
    print(f"  PSNR: {psnr:.2f} dB")
    if has_ssim:
        print(f"  SSIM: {ssim:.4f}")

    # Judgment
    threshold = 35.0
    if psnr > threshold:
        print(f"\n  ✅ [PASS] PSNR {psnr:.2f} dB > {threshold} dB — 無可見色偏")
    else:
        print(f"\n  ❌ [FAIL] PSNR {psnr:.2f} dB <= {threshold} dB — 存在色偏")

    if has_ssim:
        if ssim > 0.90:
            print(f"  ✅ [PASS] SSIM {ssim:.4f} > 0.90 — 結構相似度良好")
        else:
            print(f"  ❌ [FAIL] SSIM {ssim:.4f} <= 0.90 — 結構差異顯著")

    print(f"{'=' * 60}")
    return 0 if psnr > threshold else 1

if __name__ == '__main__':
    sys.exit(main())
