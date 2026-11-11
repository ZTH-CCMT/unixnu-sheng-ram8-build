// SPDX-License-Identifier: Apache-2.0
#ifndef NVT_PARTIAL_OBSERVATION_H
#define NVT_PARTIAL_OBSERVATION_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#include "NvtTouchCore.h"
#define NVT_PM_SIDE 9
#define NVT_PM_PATCH (NVT_PM_SIDE*NVT_PM_SIDE)
#define NVT_PM_MAX_SAMPLES 121

/* Explicit numeric profile. No handset calibration is implied. Reset disables
 * PM fitting until configure succeeds. The report experiment is sigma=2,
 * delta=6, RMS limit6, budget2/25node; callers must opt into it explicitly.
 * Q16 fields are ADC units; bounds are solver bounds, not detection thresholds. */
struct nvt_partial_config {
    uint32_t sigma_q16, huber_delta_q16, rms_limit_q16;
    uint32_t budget_num, budget_den;
    int32_t amplitude_min_q16, amplitude_max_q16;
    int32_t background_min_q16, background_max_q16;
    unsigned search_radius_q8, max_evaluations;
    unsigned allow_gaussian_learning;
};
/* Generic immutable K contract. K normalized0..1 in Q20; derivatives with
 * respect to translating its center, per node, Q20. Context must outlive this
 * generation. A callback failure is UNKNOWN. Gaussian learning is one optional
 * realization of K, not the report's uniquely specified template learner. */
typedef int (*nvt_partial_evaluator)(const void *context,int col_q8,int row_q8,
                                    int32_t *k_q20,int32_t *dx_q20,int32_t *dy_q20);
struct nvt_partial_template_binding {
    nvt_partial_evaluator evaluate;
    const void *context;
    int origin_col_q8, origin_row_q8;
};
/* Public math contract for independent formula/oracle checks. Residual and
 * position Jacobians are ADC/Q16 and ADC-per-node/Q16. K is normalizedQ20.
 * Uses full nuisance projection, actual 2x2 eigenvalue, explicit sigma.
 * Supported numeric domain: residual_q16 is signed32 (including INT32_MIN), |J_q16|<=2^30;
 * K_q20 in0..2^20. Out-of-domain values return numerical UNKNOWN before cast.
 * Eigenvalue fields describe unwhitened H=J_eff^T J_eff in Q16; sigma is
 * applied only to the explicit admission equation. No physical precision claim. */
struct nvt_partial_math_sample {int32_t residual_q16,k_q20,jx_q16,jy_q16;};
struct nvt_partial_math_result {
    int admitted, one_axis;
    uint64_t mse_q32, cost2_hi, cost2_lo;
    uint64_t lambda_min_q16, lambda_max_q16;
    int64_t schur_xx_q16,schur_xy_q16,schur_yy_q16;
    unsigned numerical_status; /*0=valid,1=input/range,2=degenerate,3=rounding-boundary UNKNOWN*/
};
int nvt_partial_math_evaluate(const struct nvt_partial_math_sample *samples,int count,
                              const struct nvt_partial_config *config,
                              struct nvt_partial_math_result *result);
enum nvt_partial_reason {
    NVT_PM_ACCEPTED, NVT_PM_NO_TEMPLATE, NVT_PM_BAD_PACKET,
    NVT_PM_STALE_PACKET, NVT_PM_NO_SIGNAL, NVT_PM_AMBIGUOUS,
    NVT_PM_SEARCH_BOUND, NVT_PM_BAD_FIT, NVT_PM_NO_INFORMATION,
    NVT_PM_NOT_CONFIGURED,NVT_PM_REASONS
};
struct nvt_partial_template {
    int valid,tracking_id,anchor_row,anchor_col;
    int origin_row_q8,origin_col_q8,seed_peak,support;
    int variance_x_q16,variance_y_q16,covariance_xy_q16;
    int seed_mse_q8; /* diagnostic MSE*256, NOT RMS; old naming retained */
    uint64_t generation,born_us,seed_us;
    int16_t values[NVT_PM_PATCH];
    nvt_partial_evaluator evaluate;
    const void *context;
};
struct nvt_partial_sample {int16_t row,col,delta;uint32_t weight;};
struct nvt_partial_state {
    struct nvt_partial_template finger[NVT_SLOTS];
    struct nvt_partial_sample sample[NVT_PM_MAX_SAMPLES];
    int32_t model[NVT_PM_MAX_SAMPLES],model_x[NVT_PM_MAX_SAMPLES],model_y[NVT_PM_MAX_SAMPLES];
    struct nvt_partial_math_sample math[NVT_PM_MAX_SAMPLES];
    struct nvt_partial_config config;
    unsigned config_valid,presence_ids,one_axis_ids;
    uint64_t last_packet_us;
    unsigned packet_clock_valid;
    unsigned long seeded,accepted,rejected[NVT_PM_REASONS];
    int last_reason,last_block;
    int last_rms_q8; /* diagnostic MSE*256, NOT RMS; admission uses exact SSE */
    int last_information;
};
void nvt_partial_reset(struct nvt_partial_state *state);
int nvt_partial_configure(struct nvt_partial_state *state,const struct nvt_partial_config *config);
int nvt_partial_bind_template(struct nvt_partial_state *state,const struct nvt_touch_core *core,
                               int slot,const struct nvt_partial_template_binding *binding,uint64_t now_us);
void nvt_partial_seed(struct nvt_partial_state *,const struct nvt_touch_core *,const int16_t *,uint64_t);
int nvt_partial_measure(struct nvt_partial_state *,const struct nvt_touch_core *,
                        const uint8_t *,size_t,uint64_t,struct nvt_contact *,int);
#ifdef __cplusplus
}
#endif
#endif
