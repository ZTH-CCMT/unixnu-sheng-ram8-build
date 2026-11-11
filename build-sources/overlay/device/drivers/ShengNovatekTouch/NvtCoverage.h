// SPDX-License-Identifier: Apache-2.0
#ifndef NVT_COVERAGE_H
#define NVT_COVERAGE_H
#include <stddef.h>
#include <stdint.h>
#include "NvtTouchCore.h"
#ifdef __cplusplus
extern "C" {
#endif
#define NVT_COVERAGE_Q16 UINT32_C(65536)
struct nvt_coverage_policy {
    /* Explicit measured/verified motion bounds; no default human speed.
     * If false, the possible position set is the entire panel. */
    unsigned motion_bounds_verified;
    uint32_t vmax_q16_per_s,amax_q16_per_s2;
};
struct nvt_coverage_contact {
    uint64_t epoch,position_us;
    int32_t row_q16,col_q16;
    /* Caller-supplied bounds enclosing E and the contact's signal support S.
     * Scalar radii conservatively enclose arbitrary initial/support sets. */
    uint32_t uncertainty_radius_q16,support_radius_q16;
    /* A statistical 3sigma estimate alone is not a verified physical bound. */
    unsigned position_support_bounds_verified;
};
struct nvt_coverage_observation {
    uint64_t epoch,sample_token,time_us;
    unsigned row0,row_count;
    /* Actual current row_count*60 little-endian signed int16 values.
     * No assembled raster or stale rows. baseline is the trusted full panel. */
    const uint8_t *values_le;
    size_t value_bytes;
    const int16_t *baseline;
    /* These provenance facts are owned by the packet/epoch/baseline producer. */
    unsigned fresh,epoch_trusted,baseline_trusted,mode_trusted;
};
enum nvt_coverage_decision {
    NVT_COVERAGE_UNKNOWN=0,
    NVT_COVERAGE_COVERED_NEGATIVE=1,
    NVT_COVERAGE_SIGNAL_PRESENT=2
};
enum nvt_coverage_reason {
    NVT_COVERAGE_NO_POLICY=0,NVT_COVERAGE_INVALID_INPUT,
    NVT_COVERAGE_UNTRUSTED,NVT_COVERAGE_EPOCH_MISMATCH,
    NVT_COVERAGE_STALE,NVT_COVERAGE_DEADLINE,
    NVT_COVERAGE_INSUFFICIENT,NVT_COVERAGE_SIGNAL,
    NVT_COVERAGE_NEGATIVE
};
struct nvt_coverage_result {
    int decision,reason,largest_delta;
    unsigned first_row,last_row,first_col,last_col;
    uint64_t dt_us,radius_q16,sample_token,time_us;
};
/* NULL policy is explicitly UNKNOWN. With an explicit unverified-bounds policy,
 * U is the whole panel; a PM stripe cannot prove absence. With verified bounds:
 * U = E (+) Ball(Vmax*dt + 0.5*Amax*dt^2), then inflate by support S.
 * Ceil Q16 arithmetic encloses, never shrinks, the continuous set.
 * dt>=100ms belongs to Core observation cancellation, not physical-UP proof.
 * A result never mutates a track or sends UP. The owner checks ID/generation
 * before consuming it, and never refreshes position time from negative data.
 * Freshness flags/epoch are trusted caller evidence, not inferred counter rules.
 */
int nvt_coverage_negative(const struct nvt_coverage_policy *,
                          const struct nvt_coverage_contact *,
                          const struct nvt_coverage_observation *,
                          struct nvt_coverage_result *);
#ifdef __cplusplus
}
#endif
#endif
