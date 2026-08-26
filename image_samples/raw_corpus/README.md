# Generic RAW test corpus

Binaries here are **untracked** (size and licensing vary per file). The tracked
record is `native/tests/raw_corpus_manifest.json`, which carries
each sample's SHA-256, expected layout, expected unpack backend and expected
error code.

To populate: place the RAW files named exactly as the manifest's `path` field,
then run

    python3 native/tests/verify_raw_corpus.py

A missing optional sample is reported as SKIP. A missing *required category*
(three Bayer vendors, two X-Trans generations, one natural LibRaw-native
fallback, one unsupported layout, three malformed fixtures) is a FAIL — silent
coverage loss is the failure mode this file exists to prevent.

Malformed fixtures are generated, not collected:

    python3 native/tests/verify_raw_corpus.py --generate-malformed
