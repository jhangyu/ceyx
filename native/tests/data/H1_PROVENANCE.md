# H1 known-answer fixtures

| File | What it is |
|---|---|
| `h1_sample.heic` | An 8-bit HEIC encoded by macOS ImageIO (`sips -s format heic`) from a downscaled photographic JPEG |
| `h1_reference.rgba` | The **independent** answer: ImageIO's own decode of `h1_sample.heic` to PNG, converted to a raw RGBA8 sidecar by `png_to_rgba.py` |

Regenerate with `make_h1_fixtures.sh <source.jpg>` on a macOS host. Paste the
two SHA-256 values it prints into the indented block below (they are what makes
"the fixture changed" a detectable event rather than a silent one):

    9ddb727dcc09af4bea69ff0527e27fc6f3b9fe537bb956cd151a533c40f9f0ad  h1_sample.heic
    42f115fc3d5ea0677efa36455edd3b83e976b41ca0f0741b5382e9d9c982e3a4  h1_reference.rgba

Extent: 512x415 RGBA8.

## Source image

The repository carries no photographic JPEG, so the source was derived in-tree
from `docs/images/demo_decoded_dng.png` (a full-frame decoded RAW rendering,
i.e. real photographic colour content, not a synthetic flat-colour target):

    sips -s format jpeg -s formatOptions 95 docs/images/demo_decoded_dng.png \
         --out <work>/h1_source.jpg
    bash native/tests/data/make_h1_fixtures.sh <work>/h1_source.jpg

That intermediate JPEG is scratch and is not committed; the two fixtures above
are, so the gate needs neither network nor phone.

## Why the reference is not produced by our decoder

A gate that compares a decoder against its own previous output detects
*regressions* and nothing else: a wrong YUV matrix, a full-vs-limited range
mistake, or swapped chroma planes would be baked into the reference and pass
forever. ImageIO is a separate implementation of the same standard, so a
systematic colour error in our path shows up as a large MAE immediately.

## Pre-registered judgement rule (fixed before the first number existed)

- Expected MAE between libheif and ImageIO on the same coded bitstream is
  dominated by chroma-upsampling and rounding differences: **0.0–1.5** (units
  of 1/255).
- **1.5–2.0** passes but is reported as marginal.
- **Above 2.0** is a FAILURE TO REPORT, not a threshold to raise. The only
  permitted response is diagnosing the range/matrix handling.
- **Exactly 0.0** is suspicious and must be checked: it usually means the
  reference was regenerated from our own decoder.
- A dimension mismatch is an immediate failure, never a resize-and-compare. It
  is also the signal that the `irot`/`imir` transform contract in `heif_api.h`
  is wrong, because ImageIO applies the container transform too.
