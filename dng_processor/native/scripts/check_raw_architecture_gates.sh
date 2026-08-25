#!/usr/bin/env bash
# Mechanises the spec section 13.3 code-review grep gate.
#
# Scope: production project source only. The vendor subtree is excluded on
# purpose - LibRaw's own RawSpeed glue is exactly where rawspeed3_* SHOULD
# appear; the rule is that PROJECT source must not.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NATIVE_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
SCOPE=("${NATIVE_DIR}/src" "${NATIVE_DIR}/include" "${NATIVE_DIR}/tests")
FAILURES=0

# grep -r over the scope, excluding third_party. Prints matches so a failure is
# diagnosable without a second command.
#
# NOTE: this intentionally does NOT strip comment lines. Step 2's red-proof
# mutation ("// rawspeed3_init") is itself a comment and must still trip
# rule1 - the spec's "zero matches" requirement for the forbidden-call rules
# is literal, including mentions in comments. Per-rule false positives from
# legitimate documentation (round-8, finding F4 recurrence) are fixed by
# tightening each rule's own pattern below, not by weakening this shared scan.
scan() {
    grep -rnE "$1" "${SCOPE[@]}" \
        --include='*.cpp' --include='*.h' --include='*.hpp' \
        2>/dev/null | grep -v '/third_party/'
}

forbid() {
    local rule="$1" pattern="$2" description="$3"
    local hits
    hits="$(scan "${pattern}")"
    local count
    count="$(printf '%s' "${hits}" | grep -c . || true)"
    if [ "${count}" -ne 0 ]; then
        echo "[ArchGate] FAIL ${rule} (${description}): ${count} occurrence(s)"
        printf '%s\n' "${hits}" | sed 's/^/    /'
        FAILURES=$((FAILURES + 1))
    else
        echo "[ArchGate] ${rule} -> PASS (${description}: 0 occurrences)"
    fi
}

require_count() {
    local rule="$1" pattern="$2" want="$3" description="$4"
    local count
    count="$(scan "${pattern}" | grep -c . || true)"
    if [ "${count}" -lt "${want}" ]; then
        echo "[ArchGate] FAIL ${rule} (${description}): found ${count}, need >= ${want}"
        FAILURES=$((FAILURES + 1))
    else
        echo "[ArchGate] ${rule} -> PASS (${description}: ${count})"
    fi
}

# --- forbidden architecture (spec section 13.3) ---------------------------
forbid rule1 'rawspeed3_init|rawspeed3_decodefile|rawspeed3_release|rawspeed3_close' \
       'no direct RawSpeed3 C API calls'
forbid rule2 '#include *[<"]rawspeed|rawspeed3_capi\.h' \
       'no RawSpeed headers in project source'
# Deviation from the plan's literal pattern (round-8, finding F4 recurrence):
# bare-name matching trips on documentation that CITES the forbidden API as a
# file:line reference (e.g. "raw2image.cpp:144-148") to explain why the
# frontend must not call it - not an actual call. Requiring call syntax
# (name immediately followed by '(') still catches a real invocation while
# leaving such citations alone.
forbid rule3 '\b(raw2image|dcraw_process|dcraw_ppm_tiff_writer|dcraw_make_mem_image)\s*\(' \
       'no LibRaw CPU render API'
forbid rule5 'RawSpeedFrontend|RawSpeedAdapter|decoder_registry|DecoderFactory|decoder_plugin' \
       'no parallel frontend, registry or plugin framework'

# rule4 is scoped to the shared GPU API surface only.
GPU_API=(
    "${NATIVE_DIR}/include/raw_pipeline_contract.h"
    "${NATIVE_DIR}/include/raw_contract_validate.h"
    "${NATIVE_DIR}/include/raw_gpu_pipeline.h"
    "${NATIVE_DIR}/include/dng_render_params.h"
)
rule4_hits=0
for header in "${GPU_API[@]}"; do
    if [ ! -f "${header}" ]; then
        # P3 (round-8 parking lot): a missing GPU-API header is a coverage
        # regression, not a skip - silently continuing would let rule4's
        # surface shrink without anyone noticing. Fail loudly with the path.
        echo "[ArchGate] FAIL rule4 (missing GPU API header): ${header}"
        rule4_hits=$((rule4_hits + 1))
        continue
    fi
    # Deviation from the plan's literal pattern (round-8, finding F4 recurrence):
    # 'LibRaw[^F]|libraw_data_t|rawspeed' substring-matched the contract's own
    # backend-identity vocabulary (enum names like kRawFrontendLibRaw, fields
    # like rawspeed_flags, string literals like "rawspeed3") which this header
    # is REQUIRED to carry for rule8 observability - not an actual LibRaw/
    # RawSpeed decoder type leaking in. Tightened to whole-word matches plus
    # comment-line exclusion so only real type/API references trip the gate.
    # N2 (round-8 nit): the lowercase LibRaw typedef family
    # (libraw_data_t, libraw_output_params_t, libraw_processed_image_t, ...)
    # is matched generically instead of enumerating each struct name.
    # N1 (round-8 nit): the earlier third pipeline stage
    # ('grep -v RawForcedBackend') is removed - the comment-line exclusion
    # above already removes all three real hits in the current tree, so the
    # extra stage was dead and would have silently hidden a genuine future
    # leak that happened to share a line with RawForcedBackend.
    if grep -nE '\bLibRaw\b|\blibraw_[a-z_]+_t\b|\brawspeed\b' "${header}" \
        | grep -vE '^[0-9]+:[[:space:]]*(//|\*)'; then
        rule4_hits=$((rule4_hits + 1))
    fi
done
if [ "${rule4_hits}" -ne 0 ]; then
    echo "[ArchGate] FAIL rule4 (no decoder type in the shared GPU API surface)"
    FAILURES=$((FAILURES + 1))
else
    echo "[ArchGate] rule4 -> PASS (no decoder type in the shared GPU API surface)"
fi

# --- required implementation (spec section 13.3, second block) ------------
require_count rule6 'class LibRawFrontendContext' 1 'one LibRaw-owned generic frontend'
require_count rule7 'class LibRawGpuInputAdapter' 1 'one LibRaw rawdata/metadata -> RawGpuInput adapter'
require_count rule8 'LIBRAW_WARN_RAWSPEED3_PROCESSED' 1 'RawSpeed3/native fallback observability'
require_count rule9 'bool runRenderStage4HalideAot' 2 'one shared Stage4 core (plain + device forms)'

if [ "${FAILURES}" -ne 0 ]; then
    echo "[ArchGate] FAIL (${FAILURES} rules)"
    exit 1
fi
echo "[ArchGate] ALL PASS"
exit 0
