/* Functional capability probe for libheif's codec plugin registry.
 *
 * Spec: docs/logs/2026-08-30/Spec_build_rewrite.md §5.6. This file is the
 * real-file replacement for the probe that used to live inline in a bash
 * heredoc (build_heif_dist_windows.sh(round6):555-584, no longer present in
 * that script -- it moves here as a compilable, reviewable, no-shell-quoting
 * source file per Plan D4).
 *
 * It backs four assertions from spec §5.3:
 *   A-CAP-HEVC-ENC, A-CAP-AV1-ENC, A-CAP-HEVC-DEC, A-CAP-AV1-DEC
 *
 * Exit code convention: bit 0 = HEVC encoder, bit 1 = AV1 encoder,
 * bit 2 = HEVC decoder, bit 3 = AV1 decoder. Exit 0 means all four
 * capabilities are present (all bits clear because a present capability
 * clears its bit, an absent one sets it -- see below); each assertion reads
 * exactly the bit it measures rather than treating the whole probe as a
 * single pass/fail unit, matching spec §5.3's requirement that
 * A-CAP-AV1-ENC and A-CAP-AV1-DEC are independently checked capabilities
 * even though this is a single probe binary. The docstring on
 * assertions.py's module explains why this file has NOT been compiled or
 * run as part of this round's deliverable (spec [H]: never described as
 * proven until a CI round shows it green).
 *
 * Round-2 status: WRITTEN, NOT YET BUILT OR RUN ANYWHERE (spec §5.6 [H]).
 * A4.6 (compiles + runs against the existing committed macOS dist, returns
 * 1/1/1/1) is explicit outstanding work, tracked separately -- not silently
 * skipped.
 */

#include <stdio.h>
#include <libheif/heif.h>

int main(void) {
  int hevc_enc = heif_have_encoder_for_format(heif_compression_HEVC);
  int av1_enc = heif_have_encoder_for_format(heif_compression_AV1);
  int hevc_dec = heif_have_decoder_for_format(heif_compression_HEVC);
  int av1_dec = heif_have_decoder_for_format(heif_compression_AV1);

  printf("HEVC-ENC=%d AV1-ENC=%d HEVC-DEC=%d AV1-DEC=%d\n",
         hevc_enc, av1_enc, hevc_dec, av1_dec);

  int exit_code = 0;
  if (!hevc_enc) exit_code |= 1;
  if (!av1_enc) exit_code |= 2;
  if (!hevc_dec) exit_code |= 4;
  if (!av1_dec) exit_code |= 8;

  return exit_code;
}
