#ifndef RAW_FILE_ROUTER_H_
#define RAW_FILE_ROUTER_H_

/* Magic-byte probe: DNG vs non-DNG, and nothing else (spec section 6.1).
 *
 * The probe must NOT judge camera vendor, CFA layout, or RawSpeed support
 * status - those belong to the decoder. It must not consult the file
 * extension either: content decides. */

#include <stddef.h>
#include <stdint.h>

#include "raw_pipeline_contract.h"

#ifdef __cplusplus
extern "C" {
#endif

#define kRawProbeHeaderBytes 512

typedef enum RawRoute {
    kRawRouteUnknown = 0,
    kRawRouteDng = 1,
    kRawRouteGeneric = 2
} RawRoute;

const char* raw_route_name(RawRoute route);

/* Stateless. Every offset derived from the header is bounds-checked against
 * header_size before it is dereferenced (untrusted input). */
RawErrorCode raw_probe_bytes(const uint8_t* header, size_t header_size,
                             RawRoute* out_route);

/* Reads at most kRawProbeHeaderBytes and delegates to raw_probe_bytes. */
RawErrorCode raw_probe_file(const char* file_path, RawRoute* out_route);

#ifdef __cplusplus
}
#endif

#endif  /* RAW_FILE_ROUTER_H_ */
