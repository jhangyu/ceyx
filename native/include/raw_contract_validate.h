#ifndef RAW_CONTRACT_VALIDATE_H_
#define RAW_CONTRACT_VALIDATE_H_

/* Layout classification and RawGpuInput invariant validation (spec sections
 * 3, 4.1, 9). Pure: no allocation, no decoder dependency, no GPU dependency.
 * Must be callable before any host or device allocation happens. */

#include <stdio.h>

#include "raw_pipeline_contract.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum RawLayoutClass {
    kRawLayoutClassUnsupported = 0,
    kRawLayoutClassBayer2x2 = 1,
    kRawLayoutClassXTrans6x6 = 2,
    kRawLayoutClassMonochrome = 3,
    kRawLayoutClassLinearRgb = 4,
    kRawLayoutClassLinearYCbCr = 5,
    kRawLayoutClassOtherCfa = 6,
    kRawLayoutClassLayered = 7,
    kRawLayoutClassMultiFrame = 8
} RawLayoutClass;

/* Pure function of the descriptor. Never consults vendor or backend. */
RawLayoutClass raw_classify_layout(const RawLayoutDescriptor* layout);

/* 1 only for Bayer 2x2 and X-Trans 6x6 in Phase 17. */
int raw_layout_class_is_production(RawLayoutClass cls);

const char* raw_layout_class_name(RawLayoutClass cls);

/* Writes the red site's column/row parity. Convention matches
 * include/dng_cfa_phase.h: RGGB=(0,0) GRBG=(1,0) GBRG=(0,1) BGGR=(1,1).
 * Returns 0 and leaves the outputs untouched when the layout is not a valid
 * 2x2 Bayer pattern - there is deliberately no RGGB fallback here, because a
 * guessed phase silently mis-colours the image (spec section 3.3.5). */
int raw_bayer_phase_from_pattern(const RawLayoutDescriptor* layout,
                                 int32_t* out_red_x,
                                 int32_t* out_red_y);

/* kRawSuccess when every spec section 4.1 invariant holds. reason_out is
 * always NUL-terminated when reason_cap > 0. */
RawErrorCode raw_validate_gpu_input(const RawGpuInput* input,
                                    char* reason_out,
                                    size_t reason_cap);

/* One line, the spec section 11 "[Contract] ... -> PASS/FAIL" format. */
void raw_contract_print(const char* stage_name,
                        const RawGpuInput* input,
                        RawErrorCode status,
                        const char* reason,
                        FILE* out);

#ifdef __cplusplus
}
#endif

#endif  /* RAW_CONTRACT_VALIDATE_H_ */
