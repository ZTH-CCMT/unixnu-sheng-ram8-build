// SPDX-License-Identifier: Apache-2.0
#include "NvtCoverage.h"
#include <string.h>

static uint64_t cover_ceil_div(uint64_t n,uint64_t d) {
    return n/d+(n%d!=0);
}
/* Exact ceil(Aq16*dt_us^2/(2*10^12)), avoiding the potentially overflowing
 * three-factor product. dt<100000 and A<=UINT32_MAX:
 * A*dt<2^49; q*dt<2^45; remainder numerator<2.2e12.
 * Only uint64 divide/multiply is needed; no __int128 runtime operations. */
static uint64_t cover_acceleration(uint32_t a,uint64_t dt) {
    const uint64_t first=(uint64_t)a*dt;
    const uint64_t q=first/UINT64_C(2000000),r=first%UINT64_C(2000000);
    const uint64_t qdt=q*dt;
    const uint64_t tail=(qdt%UINT64_C(1000000))*UINT64_C(2000000)+r*dt;
    return qdt/UINT64_C(1000000)+cover_ceil_div(tail,UINT64_C(2000000000000));
}
static unsigned cover_low(int64_t value,unsigned maximum) {
    if(value<=0)return 0;
    if(value>=(int64_t)maximum*65536)return maximum;
    return (unsigned)(value/65536);
}
static unsigned cover_high(int64_t value,unsigned maximum) {
    if(value<=0)return 0;
    if(value>=(int64_t)maximum*65536)return maximum;
    return (unsigned)cover_ceil_div((uint64_t)value,65536);
}
static int cover_finish(struct nvt_coverage_result *r,int decision,int reason) {
    r->decision=decision;r->reason=reason;return decision;
}
int nvt_coverage_negative(const struct nvt_coverage_policy *policy,
                          const struct nvt_coverage_contact *contact,
                          const struct nvt_coverage_observation *observation,
                          struct nvt_coverage_result *result) {
    if(!result)return NVT_COVERAGE_UNKNOWN;
    memset(result,0,sizeof(*result));
    if(!policy)return cover_finish(result,NVT_COVERAGE_UNKNOWN,NVT_COVERAGE_NO_POLICY);
    if(!contact || !observation || !observation->values_le || !observation->baseline ||
       !observation->row_count || observation->row0>=NVT_ROWS ||
       observation->row_count>NVT_ROWS-observation->row0 ||
       observation->value_bytes<(size_t)observation->row_count*NVT_COLUMNS*2u ||
       contact->row_q16<0 || contact->row_q16>(NVT_ROWS-1)*65536 ||
       contact->col_q16<0 || contact->col_q16>(NVT_COLUMNS-1)*65536)
        return cover_finish(result,NVT_COVERAGE_UNKNOWN,NVT_COVERAGE_INVALID_INPUT);
    result->sample_token=observation->sample_token;result->time_us=observation->time_us;
    if(!observation->fresh || !observation->epoch_trusted ||
       !observation->baseline_trusted || !observation->mode_trusted)
        return cover_finish(result,NVT_COVERAGE_UNKNOWN,NVT_COVERAGE_UNTRUSTED);
    if(contact->epoch!=observation->epoch)
        return cover_finish(result,NVT_COVERAGE_UNKNOWN,NVT_COVERAGE_EPOCH_MISMATCH);
    if(observation->time_us<contact->position_us)
        return cover_finish(result,NVT_COVERAGE_UNKNOWN,NVT_COVERAGE_STALE);
    const uint64_t dt=observation->time_us-contact->position_us;
    result->dt_us=dt;
    if(dt>=NVT_OBSERVATION_MAX_US)
        return cover_finish(result,NVT_COVERAGE_UNKNOWN,NVT_COVERAGE_DEADLINE);
    if(!policy->motion_bounds_verified || !contact->position_support_bounds_verified) {
        result->first_row=0;result->last_row=NVT_ROWS-1;
        result->first_col=0;result->last_col=NVT_COLUMNS-1;
    } else {
        const uint64_t velocity=cover_ceil_div((uint64_t)policy->vmax_q16_per_s*dt,UINT64_C(1000000));
        const uint64_t radius=(uint64_t)contact->uncertainty_radius_q16+
            contact->support_radius_q16+velocity+cover_acceleration(policy->amax_q16_per_s2,dt);
        result->radius_q16=radius;
        // Bounds stay within signed64 even for all explicit uint32 maxima.
        result->first_row=cover_low((int64_t)contact->row_q16-(int64_t)radius,NVT_ROWS-1);
        result->last_row=cover_high((int64_t)contact->row_q16+(int64_t)radius,NVT_ROWS-1);
        result->first_col=cover_low((int64_t)contact->col_q16-(int64_t)radius,NVT_COLUMNS-1);
        result->last_col=cover_high((int64_t)contact->col_q16+(int64_t)radius,NVT_COLUMNS-1);
    }
    const unsigned covered_last=observation->row0+observation->row_count-1;
    if(result->first_row<observation->row0 || result->last_row>covered_last)
        return cover_finish(result,NVT_COVERAGE_UNKNOWN,NVT_COVERAGE_INSUFFICIENT);
    int largest=-65536;
    for(unsigned row=result->first_row;row<=result->last_row;++row)
        for(unsigned col=result->first_col;col<=result->last_col;++col) {
            const size_t local=(size_t)(row-observation->row0)*NVT_COLUMNS+col,offset=local*2;
            const unsigned raw=(unsigned)observation->values_le[offset] |
                ((unsigned)observation->values_le[offset+1]<<8);
            const int value=(raw&0x8000u)?(int)raw-65536:(int)raw;
            const int delta=value-(int)observation->baseline[row*NVT_COLUMNS+col];
            if(delta>largest)largest=delta;
            // Existing detector noise floor: either sign can be interference
            // or an unreliable baseline. Neither proves quiet background.
            // This is not sufficient positive finger/contact evidence.
            if(delta>=NVT_NOISE_THRESHOLD || delta<=-NVT_NOISE_THRESHOLD) {
                result->largest_delta=largest;
                return cover_finish(result,NVT_COVERAGE_SIGNAL_PRESENT,NVT_COVERAGE_SIGNAL);
            }
        }
    result->largest_delta=largest;
    return cover_finish(result,NVT_COVERAGE_COVERED_NEGATIVE,NVT_COVERAGE_NEGATIVE);
}
