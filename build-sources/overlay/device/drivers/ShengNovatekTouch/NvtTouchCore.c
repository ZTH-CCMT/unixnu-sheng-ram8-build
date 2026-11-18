// SPDX-License-Identifier: Apache-2.0
// See NvtTouchCore.h and NOTICE. Finger tracking with qualified regional observations.
#include "NvtTouchCore.h"
#include "NvtShapeMetricsInline.h"
#include <string.h>

static const int kRowMap[NVT_ROWS] = {
    26, 78, 130, 182, 234, 286, 338, 390, 442, 494,
    546, 598, 650, 702, 754, 806, 856, 906, 956, 1006,
    1056, 1106, 1156, 1206, 1256, 1306, 1356, 1406, 1456, 1506,
    1556, 1606, 1656, 1706, 1756, 1806, 1856, 1906, 1956, 2006,
};
static const int kColumnMap[NVT_COLUMNS] = {
    25, 75, 125, 175, 225, 275, 325, 375, 425, 476,
    528, 580, 632, 684, 736, 788, 840, 892, 944, 996,
    1048, 1099, 1149, 1199, 1249, 1299, 1349, 1399, 1449, 1499,
    1549, 1599, 1649, 1699, 1749, 1799, 1849, 1899, 1949, 2000,
    2052, 2104, 2156, 2208, 2260, 2312, 2364, 2416, 2468, 2520,
    2572, 2623, 2673, 2723, 2773, 2823, 2873, 2923, 2973, 3023,
};
#define AXIS_BASE 2700
#define AXIS_EDGE 600
#define AXIS_GAIN 4350
#define MAX_PEAKS NVT_REGION_MAX_CANDIDATES
#define TRACK_RADIUS (NVT_SUPER_RESOLUTION*50*4)
#define REJOIN_RADIUS (4*NVT_FUZZ)
#define SCAN_INTERVAL_US (UINT64_C(1000000) / NVT_FRAME_HZ)
#define MOTION_CUTOFF_MHZ 34000
#define MOTION_RESPONSE_FADE_US UINT64_C(125000)
#define MOTION_RUN_SCALE 256
#define MOTION_RUN_FULL (12 * MOTION_RUN_SCALE) // 1.2 px of consistent travel
#define FILTER_ONE INT64_C(65536)
#define LEGACY_FILTER_QUANTUM (FILTER_ONE / 16)

int nvt_touch_row_center(int row) {return row>=0 && row<NVT_ROWS ? kRowMap[row] : 0;}
int nvt_touch_column_center(int column) {return column>=0 && column<NVT_COLUMNS ? kColumnMap[column] : 0;}

int nvt_touch_map_edge_axis(int value, int maximum, int low_edge_bias) {
    if (value < AXIS_EDGE) {
        int mapped = AXIS_GAIN * (AXIS_EDGE - value) / AXIS_BASE;
        if (mapped > AXIS_EDGE - 1) mapped = AXIS_EDGE - 1;
        return AXIS_EDGE - mapped - (low_edge_bias ? 1 : 0);
    }
    int edge_start = maximum - AXIS_EDGE;
    if (value > edge_start) {
        int mapped = AXIS_GAIN * (value - edge_start) / AXIS_BASE;
        if (mapped > AXIS_EDGE - 1) mapped = AXIS_EDGE - 1;
        return edge_start + mapped;
    }
    return value;
}

void nvt_touch_reset(struct nvt_touch_core *core) {
    memset(core, 0, sizeof(*core));
}

// The lowest identifier no active slot holds, 1..NVT_SLOTS. The identifier is the contact's path index upstream, and backboardd drops
// every path whose index reaches its path limit (int-267: after about 33 strokes no touch reached SpringBoard), so identifiers are
// reused once their finger is up instead of counting up for the whole boot.
static int free_tracking_id(const struct nvt_touch_core *core) {
    for (int id=1;id<=NVT_SLOTS;++id) {
        int taken=0;
        for (int s=0;s<NVT_SLOTS && !taken;++s) taken=core->slots[s].active && core->slots[s].tracking_id==id;
        if (core->timed_enabled && ((core->retired_ids | core->previous_retired_ids) & (1u << id))) taken=1;
        if (!taken) return id;
    }
    return core->timed_enabled ? 0 : NVT_SLOTS;
}

// Backward Euler low-pass gain in Q16, using the observation interval.
static int64_t filter_alpha(int64_t cutoff_mhz, uint64_t dt_us) {
    uint64_t w;
    if (dt_us) {
        if ((uint64_t)cutoff_mhz > UINT64_MAX / dt_us) return 65535;
        const uint64_t product=(uint64_t)cutoff_mhz*dt_us;
        w=(product/UINT64_C(1000000000))*411775+
          ((product%UINT64_C(1000000000))*411775)/UINT64_C(1000000000);
    } else w=(uint64_t)(cutoff_mhz*411775/(1000*NVT_FRAME_HZ));
    // Divide the complement to avoid w*65536 overflowing at long intervals.
    const uint64_t denominator=65536+w,numerator=UINT64_C(1)<<32;
    return (int64_t)(65536-(numerator/denominator+(numerator%denominator!=0)));
}

static int64_t response_weight(int value, int start, int width) {
    if (value <= start) return 0;
    if (value >= start + width) return FILTER_ONE;
    return (int64_t)(value - start) * FILTER_ONE / width;
}

static int64_t position_weight(int step, uint64_t dt_us, int *run) {
    const uint64_t quiet_us = !step ? dt_us : dt_us > SCAN_INTERVAL_US ? dt_us - SCAN_INTERVAL_US : 0;
    const int decay = quiet_us >= MOTION_RESPONSE_FADE_US ? MOTION_RUN_FULL :
        (int)(MOTION_RUN_FULL * quiet_us / MOTION_RESPONSE_FADE_US);
    int prior = *run < 0 ? -*run : *run;
    prior = prior > decay ? prior - decay : 0;
    if (!step) {
        *run = *run < 0 ? -prior : prior;
        return 0;
    }
    const int distance = step < 0 ? -step : step;
    const int same_direction = prior && ((*run > 0) == (step > 0));
    int travel = distance * MOTION_RUN_SCALE + (same_direction ? prior : 0);
    if (travel > MOTION_RUN_FULL) travel = MOTION_RUN_FULL;
    *run = step < 0 ? -travel : travel;

    const int64_t single = response_weight(distance, 4 * NVT_FUZZ, 8 * NVT_SUPER_RESOLUTION);
    const int64_t sustained = same_direction ?
        response_weight(travel, 3 * MOTION_RUN_SCALE, 9 * MOTION_RUN_SCALE) : 0;
    return sustained > single ? sustained : single;
}

static int64_t follow_axis(int64_t position, int64_t target, int step, int64_t *velocity,
                           int *run, uint32_t *weight, uint64_t dt_us,
                           int64_t speed_alpha, int64_t fast_alpha) {
    const int64_t interval = dt_us > (uint64_t)INT64_MAX ? INT64_MAX : (int64_t)dt_us;
    const int64_t speed = (int64_t)step * 1000000 / interval;
    *velocity += (speed - *velocity) * speed_alpha / FILTER_ONE;
    const int64_t magnitude = *velocity < 0 ? -*velocity : *velocity;
    int64_t alpha = filter_alpha(NVT_MIN_CUTOFF_MHZ + magnitude * NVT_CUTOFF_PER_SPEED / 2, dt_us);
    *weight = (uint32_t)position_weight(step, dt_us, run);
    if (fast_alpha > alpha) alpha += (fast_alpha - alpha) * *weight / FILTER_ONE;

    const int64_t residual = target - position;
    // Hold until following a turned sample would move in its measured direction.
    if ((step < 0 && residual > 0) || (step > 0 && residual < 0)) return position;
    return position + residual * alpha / FILTER_ONE;
}

static void seed_filter(struct nvt_slot *slot, int x, int y) {
    slot->rx=slot->filter_raw_x=x;
    slot->ry=slot->filter_raw_y=y;
    slot->fx=x*FILTER_ONE;
    slot->fy=y*FILTER_ONE;
    slot->vx=slot->vy=0;
    slot->motion_run_x=slot->motion_run_y=0;
    slot->motion_weight_x=slot->motion_weight_y=0;
}

// Timed measurements follow each axis independently at full precision.
static int follow(struct nvt_slot *slot, int x, int y, int weak, uint64_t dt_us) {
    if (weak) {
        slot->motion_run_x=slot->motion_run_y=0;
        slot->motion_weight_x=slot->motion_weight_y=0;
        return 0;
    }
    const int64_t target_x=x*FILTER_ONE,target_y=y*FILTER_ONE;
    const int64_t speed_alpha=filter_alpha(NVT_SPEED_CUTOFF_MHZ,dt_us);
    if (dt_us) {
        const int64_t fast_alpha=filter_alpha(MOTION_CUTOFF_MHZ,dt_us);
        slot->fx=follow_axis(slot->fx,target_x,x-slot->filter_raw_x,&slot->vx,
                             &slot->motion_run_x,&slot->motion_weight_x,dt_us,speed_alpha,fast_alpha);
        slot->fy=follow_axis(slot->fy,target_y,y-slot->filter_raw_y,&slot->vy,
                             &slot->motion_run_y,&slot->motion_weight_y,dt_us,speed_alpha,fast_alpha);
    } else {
        // The untimed API retains the original shared cutoff and Q4 rounding.
        const int64_t sx=(target_x-slot->fx)*NVT_FRAME_HZ/FILTER_ONE;
        const int64_t sy=(target_y-slot->fy)*NVT_FRAME_HZ/FILTER_ONE;
        slot->vx+=(sx-slot->vx)*speed_alpha/FILTER_ONE;
        slot->vy+=(sy-slot->vy)*speed_alpha/FILTER_ONE;
        const int64_t speed=(slot->vx<0 ? -slot->vx : slot->vx)+(slot->vy<0 ? -slot->vy : slot->vy);
        const int64_t alpha=filter_alpha(NVT_MIN_CUTOFF_MHZ+speed*NVT_CUTOFF_PER_SPEED/2,0);
        slot->fx+=((target_x-slot->fx)/LEGACY_FILTER_QUANTUM)*alpha/FILTER_ONE*LEGACY_FILTER_QUANTUM;
        slot->fy+=((target_y-slot->fy)/LEGACY_FILTER_QUANTUM)*alpha/FILTER_ONE*LEGACY_FILTER_QUANTUM;
        slot->motion_run_x=slot->motion_run_y=0;
        slot->motion_weight_x=slot->motion_weight_y=0;
    }
    slot->rx=(int)((slot->fx+FILTER_ONE/2)/FILTER_ONE);
    slot->ry=(int)((slot->fy+FILTER_ONE/2)/FILTER_ONE);
    slot->filter_raw_x=x;slot->filter_raw_y=y;
    return 1;
}

struct track_projection {
    int x, y, raw_x, raw_y;
    int radius, rejoin_radius, timed;
};

enum {
    track_radius_squared = TRACK_RADIUS * TRACK_RADIUS,
    shape_cost_limit = track_radius_squared / 4,
    // Losing a confirmed match costs more than all feasible pairs combined.
    confirmed_miss_cost = (NVT_SLOTS + 1) * (track_radius_squared + 2 * shape_cost_limit),
    rejected_match_cost = confirmed_miss_cost + 1
};

static uint64_t floor_sqrt_u64(uint64_t value) {
    uint64_t result=0,bit=UINT64_C(1)<<62;
    while(bit>value)bit>>=2;
    while(bit) {
        if(value>=result+bit) {value-=result+bit;result=(result>>1)+bit;}
        else result>>=1;
        bit>>=2;
    }
    return result;
}
static int64_t point_cost(int x, int y, int px, int py) {
    const int64_t dx = (int64_t)x - px, dy = (int64_t)y - py;
    return dx * dx + dy * dy;
}
static void project_tracks(const struct nvt_touch_core *core, uint64_t now_us,
                           struct track_projection *projections) {
    unsigned legacy_active = 0;
    if (!core->timed_enabled) {
        for (int s = 0; s < NVT_SLOTS; ++s) legacy_active += core->slots[s].active != 0;
    }
    for (int s = 0; s < NVT_SLOTS; ++s) {
        const struct nvt_slot *slot = &core->slots[s];
        if (!slot->active) continue;
        struct track_projection *p = &projections[s];
        *p = (struct track_projection){
            slot->x, slot->y, slot->x, slot->y,
            slot->missed ? REJOIN_RADIUS : TRACK_RADIUS, REJOIN_RADIUS, core->timed_enabled != 0
        };
        if (!p->timed) {
            const int steps = legacy_active <= 3 ? slot->missed + 1 : 0;
            p->x += slot->dx * steps;
            p->y += slot->dy * steps;
            continue;
        }
        const uint64_t gap = now_us > slot->last_observed_us ? now_us - slot->last_observed_us : 0;
        const uint64_t horizon = gap < NVT_DROPOUT_MAX_US ? gap : NVT_DROPOUT_MAX_US;
        // One nominal scan keeps the old 24 px rejoin radius. Missing scans
        // widen it in proportion to the elapsed time, up to the dropout budget.
        if (horizon > SCAN_INTERVAL_US)
            p->rejoin_radius = (int)(REJOIN_RADIUS * horizon / SCAN_INTERVAL_US);
        if (slot->missed) p->radius = p->rejoin_radius;
        if (slot->age < 2 || !horizon || gap >= NVT_OBSERVATION_MAX_US ||
            !slot->last_interval_us || slot->last_interval_us > NVT_DROPOUT_MAX_US ||
            point_cost(slot->dx, slot->dy, 0, 0) < NVT_FUZZ * NVT_FUZZ) continue;

        // A very short host interval is a poor estimate of the controller rate.
        const uint64_t interval = slot->last_interval_us < SCAN_INTERVAL_US / 2 ?
            SCAN_INTERVAL_US / 2 : slot->last_interval_us;
        int64_t dx = (int64_t)slot->dx * (int64_t)horizon / (int64_t)interval;
        int64_t dy = (int64_t)slot->dy * (int64_t)horizon / (int64_t)interval;
        const uint64_t distance_squared = (uint64_t)(dx * dx) + (uint64_t)(dy * dy);
        if (distance_squared > track_radius_squared) {
            uint64_t length = floor_sqrt_u64(distance_squared);
            if (length * length < distance_squared) ++length;
            dx = dx * TRACK_RADIUS / (int64_t)length;
            dy = dy * TRACK_RADIUS / (int64_t)length;
        }
        if (gap > horizon) {
            const int64_t remaining = (int64_t)(NVT_OBSERVATION_MAX_US - gap);
            const int64_t fade = (int64_t)(NVT_OBSERVATION_MAX_US - NVT_DROPOUT_MAX_US);
            dx = dx * remaining / fade;
            dy = dy * remaining / fade;
        }
        p->x += (int)dx;
        p->y += (int)dy;
    }
}

static int64_t track_distance(const struct track_projection *p, int x, int y) {
    const int64_t predicted = point_cost(x, y, p->x, p->y);
    const int64_t measured = point_cost(x, y, p->raw_x, p->raw_y);
    return p->timed && measured < predicted ? measured : predicted;
}

struct electrode_span {int first, last;};
struct peak_bounds {uint8_t first_row, last_row, first_col, last_col;};

static int electrode_position(int index, int short_axis) {
    const int center = short_axis ? kRowMap[index] : kColumnMap[index];
    const int maximum = short_axis ? NVT_SHORT_AXIS_MAX : NVT_LONG_AXIS_MAX;
    return nvt_touch_map_edge_axis(center * NVT_SUPER_RESOLUTION, maximum, short_axis);
}

static struct electrode_span contact_span(int measured, int predicted, int short_axis) {
    const int count = short_axis ? NVT_ROWS : NVT_COLUMNS;
    const int low = measured < predicted ? measured : predicted;
    const int high = measured > predicted ? measured : predicted;
    struct electrode_span span = {count - 1, count - 1};
    int previous = electrode_position(0, short_axis);
    for (int i = 1; i < count; ++i) {
        const int position = electrode_position(i, short_axis);
        const int midpoint_twice = previous + position;
        if (span.first == count - 1 && 2 * low <= midpoint_twice) span.first = i - 1;
        if (2 * high < midpoint_twice) {span.last = i - 1;break;}
        previous = position;
    }
    return span;
}

static int matrix_contact_quiet(const struct nvt_touch_core *core, const struct track_projection *projection,
                                const struct peak_bounds *bounds, uint32_t survivor_labels) {
    const struct electrode_span rows = contact_span(projection->raw_y, projection->y, 1);
    const struct electrode_span cols = contact_span(projection->raw_x, projection->x, 0);
    const int top = rows.first >= NVT_MERGE_SPAN ? rows.first - NVT_MERGE_SPAN : 0;
    const int bottom = rows.last + NVT_MERGE_SPAN < NVT_ROWS ? rows.last + NVT_MERGE_SPAN : NVT_ROWS - 1;
    const int left = cols.first >= NVT_MERGE_SPAN ? cols.first - NVT_MERGE_SPAN : 0;
    const int right = cols.last + NVT_MERGE_SPAN < NVT_COLUMNS ? cols.last + NVT_MERGE_SPAN : NVT_COLUMNS - 1;
    for (int r = top; r <= bottom; ++r) {
        for (int c = left; c <= right; ++c) {
            const int node = r * NVT_COLUMNS + c;
            const int delta = core->delta[node];
            if (delta <= -NVT_NOISE_THRESHOLD) return 0;
            if (delta < NVT_NOISE_THRESHOLD) continue;
            // The measured-to-predicted footprint itself must be quiet.
            if (r >= rows.first && r <= rows.last && c >= cols.first && c <= cols.last) return 0;
            // A matched neighbor can account for its tail only if its whole peak group is outside.
            const int label = core->label[node];
            if (label < 0 || !(survivor_labels & (UINT32_C(1) << label))) return 0;
            const struct peak_bounds *group = &bounds[label];
            if (group->first_row <= bottom && group->last_row >= top &&
                group->first_col <= right && group->last_col >= left) return 0;
        }
    }
    return 1;
}

// Use the same motion hypotheses for a nearby weak return and its recovery.
static int miss_reason(const struct track_projection *projection, const struct nvt_contact *found,
                       const int *used, int nfound, const int (*palms)[2], int npalms, const int *quality) {
    int64_t best = -1;
    int reason = NVT_MISS_NONE;
    for (int c = 0; c < nfound; ++c) {
        const int weak = found[c].peak < NVT_UP_THRESHOLD ||
            (quality && found[c].peak < NVT_DOWN_THRESHOLD && !quality[c]);
        const int64_t distance = weak && projection->timed ?
            track_distance(projection, found[c].x, found[c].y) :
            point_cost(found[c].x, found[c].y, projection->raw_x, projection->raw_y);
        if (best >= 0 && distance >= best) continue;
        best = distance;
        reason = used[c] ? NVT_MISS_TAKEN : weak ? NVT_MISS_WEAK : NVT_MISS_FAR;
    }
    for (int p = 0; p < npalms; ++p) {
        const int64_t distance = point_cost(palms[p][0], palms[p][1], projection->raw_x, projection->raw_y);
        if (best >= 0 && distance >= best) continue;
        best = distance;
        reason = NVT_MISS_PALM;
    }
    if (best > track_radius_squared) return NVT_MISS_FAR;
    if (reason == NVT_MISS_WEAK && best > projection->rejoin_radius * projection->rejoin_radius)
        return NVT_MISS_FAR;
    return reason;
}

static int shape_cost(int previous, int current) {
    if (previous <= 0 || current <= 0) return 0;
    const int larger = previous > current ? previous : current;
    const int smaller = previous < current ? previous : current;
    return (int)((int64_t)(larger - smaller) * shape_cost_limit / larger);
}

// Hungarian assignment for one group. Each row has a choice of an eligible
// candidate or a dummy column. Score each pair once for the augmenting paths.
static void assign_group(struct nvt_touch_core *core, const struct nvt_contact *found, int nfound,
                         const int *quality, const struct track_projection *projections,
                         const int *rows, int n, uint32_t available, int *match) {
    if (!n || !available) return;
    enum { COLUMNS = MAX_PEAKS + NVT_SLOTS };
    int columns[MAX_PEAKS], count = 0;
    for (int c = 0; c < nfound; ++c) {
        if (available & (UINT32_C(1) << c)) columns[count++] = c;
    }
    const int confirmed = core->timed_enabled && core->slots[rows[0]].age >= 2;
    const int unmatched = confirmed ? confirmed_miss_cost : track_radius_squared;
    int (*costs)[MAX_PEAKS] = core->assignment_costs;
    for (int i = 0; i < n; ++i) {
        const struct nvt_slot *slot = &core->slots[rows[i]];
        const struct track_projection *projection = &projections[rows[i]];
        const int compare_shape = confirmed && quality && n > 1 && count > 1 &&
            slot->shape_peak && core->now_us >= slot->shape_us &&
            core->now_us - slot->shape_us <= NVT_OBSERVATION_MAX_US;
        for (int j = 0; j < count; ++j) {
            const int c = columns[j];
            const struct nvt_contact *contact = &found[c];
            costs[i][j] = rejected_match_cost;
            if (contact->peak < NVT_UP_THRESHOLD ||
                (projection->timed && quality && contact->peak < NVT_DOWN_THRESHOLD && !quality[c])) continue;
            const int64_t distance = track_distance(projection, contact->x, contact->y);
            if (distance > projection->radius * projection->radius) continue;
            int cost = (int)distance;
            if (compare_shape && contact->peak >= NVT_DOWN_THRESHOLD && contact->area > 0)
                cost += shape_cost(slot->shape_peak, contact->peak) + shape_cost(slot->shape_area, contact->area);
            costs[i][j] = cost;
        }
    }
    const int m=count+n;
    int64_t u[NVT_SLOTS+1]={0},v[COLUMNS+1]={0},minv[COLUMNS+1];
    int p[COLUMNS+1]={0},way[COLUMNS+1]={0};char used[COLUMNS+1];
    for (int i=1;i<=n;++i) {
        p[0]=i;int j0=0;
        for (int j=0;j<=m;++j) {minv[j]=INT64_MAX;used[j]=0;}
        do {
            used[j0]=1;
            const int i0=p[j0];int64_t delta=INT64_MAX;int j1=0;
            for (int j=1;j<=m;++j) {
                if (used[j]) continue;
                const int64_t cur=(j<=count ? costs[i0-1][j-1] : unmatched)-u[i0]-v[j];
                if (cur<minv[j]) {minv[j]=cur;way[j]=j0;}
                if (minv[j]<delta) {delta=minv[j];j1=j;}
            }
            for (int j=0;j<=m;++j) {
                if (used[j]) {u[p[j]]+=delta;v[j]-=delta;}
                else minv[j]-=delta;
            }
            j0=j1;
        } while (p[j0]);
        do {const int j1=way[j0];p[j0]=p[j1];j0=j1;} while (j0);
    }
    for (int j = 1; j <= count; ++j) {
        if (p[j] && costs[p[j] - 1][j - 1] <= unmatched) match[rows[p[j] - 1]] = columns[j - 1];
    }
}

static void assign_tracks(struct nvt_touch_core *core, const struct nvt_contact *found, int nfound,
                          int *match, const int *quality, const struct track_projection *projections) {
    _Static_assert(MAX_PEAKS < 32, "Candidate indices must fit in the availability mask");
    int confirmed[NVT_SLOTS], pending[NVT_SLOTS], nconfirmed = 0, npending = 0;
    for (int s = 0; s < NVT_SLOTS; ++s) {
        match[s] = -1;
        if (!core->slots[s].active) continue;
        if (!core->timed_enabled || core->slots[s].age >= 2) confirmed[nconfirmed++] = s;
        else if (quality) pending[npending++] = s;
    }
    uint32_t available = (UINT32_C(1) << nfound) - 1;
    assign_group(core, found, nfound, quality, projections, confirmed, nconfirmed, available, match);
    // Pending contacts compete only for points the visible contacts left free.
    if (npending) {
        for (int i = 0; i < nconfirmed; ++i) {
            const int c = match[confirmed[i]];
            if (c >= 0) available &= ~(UINT32_C(1) << c);
        }
        assign_group(core, found, nfound, quality, projections, pending, npending, available, match);
    }
}

static void sort_samples(int *values, unsigned count) {
    for (unsigned i = 1; i < count; ++i) {
        const int value = values[i];
        unsigned j = i;
        while (j > 0 && values[j - 1] > value) {
            values[j] = values[j - 1];
            --j;
        }
        values[j] = value;
    }
}

static void form_reference(struct nvt_touch_core *core) {
    int column[NVT_STARTUP_FRAMES];
    const unsigned quartile = NVT_STARTUP_FRAMES / 4;
    // A finger raises the recorded signal. The lower quartile can recover the
    // baseline from fewer than half of the startup scans at rest.
    for (unsigned node=0;node<NVT_NODES;++node) {
        for (unsigned s=0;s<NVT_STARTUP_FRAMES;++s) column[s]=core->startup[s][node];
        sort_samples(column, NVT_STARTUP_FRAMES);
        core->reference[node] = (int16_t)((column[quartile - 1] + column[quartile]) / 2);
    }
    core->have_reference=1;core->startup_count=0;
}

static int delta_at(const int16_t *m, const int16_t *ref, int index) {return (int)m[index]-(int)ref[index];}

static int reference_step(int delta) {
    const int step = delta / NVT_REFERENCE_RATE;
    return step ? step : (delta > 0) - (delta < 0);
}

static int common_reference_delta(const struct nvt_touch_core *core, const int16_t *matrix) {
    enum { grid = 5 };
    int samples[grid * grid];
    for (int r = 0; r < grid; ++r) {
        const int row = (2 * r + 1) * NVT_ROWS / (2 * grid);
        for (int c = 0; c < grid; ++c) {
            const int col = (2 * c + 1) * NVT_COLUMNS / (2 * grid);
            samples[r * grid + c] = delta_at(matrix, core->reference, row * NVT_COLUMNS + col);
        }
    }
    sort_samples(samples, grid * grid);
    return samples[grid * grid / 2];
}

static void measure_and_update_reference(struct nvt_touch_core *core, const int16_t *matrix) {
    _Static_assert(NVT_COLUMNS <= 64, "A reference row must fit in one bit mask");
    const int common = common_reference_delta(core, matrix);
    const int guard_threshold = common + NVT_NOISE_THRESHOLD / 2;
    uint64_t guard_rows[NVT_ROWS];
    for (int r = 0; r < NVT_ROWS; ++r) {
        uint64_t support = 0;
        for (int c = 0; c < NVT_COLUMNS; ++c) {
            const int node = r * NVT_COLUMNS + c;
            const int delta = delta_at(matrix, core->reference, node);
            core->delta[node] = delta;
            support |= (uint64_t)(delta >= guard_threshold) << c;
        }
        guard_rows[r] = support;
    }
    // Two neighboring electrodes above the local guard can be the start of a
    // finger. Hold their neighborhood before it reaches the detection threshold.
    uint64_t previous = 0;
    for (int r = 0; r < NVT_ROWS; ++r) {
        const uint64_t row = guard_rows[r];
        const uint64_t adjacent = previous | (r + 1 < NVT_ROWS ? guard_rows[r + 1] : 0);
        const uint64_t nearby = row | adjacent;
        const uint64_t foreground = row & (adjacent | (nearby << 1) | (nearby >> 1));
        uint64_t guard = foreground;
        for (int offset = 1; offset <= NVT_MERGE_SPAN; ++offset)
            guard |= (foreground << offset) | (foreground >> offset);
        guard_rows[r] = guard;
        previous = row;
    }
    int moved = 0;
    for (int r = 0; r < NVT_ROWS; ++r) {
        uint64_t guard = 0;
        for (int offset = -NVT_MERGE_SPAN; offset <= NVT_MERGE_SPAN; ++offset)
            if (r + offset >= 0 && r + offset < NVT_ROWS) guard |= guard_rows[r + offset];
        for (int c = 0; c < NVT_COLUMNS; ++c, guard >>= 1) {
            if (guard & 1) continue;
            const int node = r * NVT_COLUMNS + c;
            const int step = reference_step(core->delta[node]);
            core->reference[node] = (int16_t)(core->reference[node] + step);
            moved |= step != 0;
        }
    }
    core->reference_frames += moved;
}

struct peak {int index, value;};
struct blob {
    int64_t sum_w, sum_row, sum_col;
    int peak, floor, area, weighted_area;
};

static int peaks_share_neighborhood(int first, int second) {
    const int dr = first / NVT_COLUMNS - second / NVT_COLUMNS;
    const int dc = first % NVT_COLUMNS - second % NVT_COLUMNS;
    return dr >= -NVT_MERGE_SPAN && dr <= NVT_MERGE_SPAN && dc >= -NVT_MERGE_SPAN && dc <= NVT_MERGE_SPAN;
}

// Cell edges use doubled coordinates so adjacent-electrode midpoints stay exact.
struct peak_cell {int left, right, top, bottom;};

static struct peak_cell peak_cell_at(int node) {
    const int row = node / NVT_COLUMNS, col = node % NVT_COLUMNS;
    const int x = electrode_position(col, 0), y = electrode_position(row, 1);
    return (struct peak_cell){
        col > 0 ? x + electrode_position(col - 1, 0) : 0,
        col + 1 < NVT_COLUMNS ? x + electrode_position(col + 1, 0) : 2 * NVT_LONG_AXIS_MAX,
        row > 0 ? y + electrode_position(row - 1, 1) : 0,
        row + 1 < NVT_ROWS ? y + electrode_position(row + 1, 1) : 2 * NVT_SHORT_AXIS_MAX
    };
}

static int spacing_fits_cells(const struct peak_cell *first, const struct peak_cell *second, int dx, int dy) {
    return 2 * dx >= second->left - first->right && 2 * dx <= second->right - first->left &&
           2 * dy >= second->top - first->bottom && 2 * dy <= second->bottom - first->top;
}

static int cell_contains_position(const struct peak_cell *cell, int x, int y) {
    return 2 * x >= cell->left && 2 * x <= cell->right && 2 * y >= cell->top && 2 * y <= cell->bottom;
}

static int ridge_fits_neighbor(int first_node, int second_node, const struct track_projection *track,
                               const struct track_projection *neighbor) {
    const struct peak_cell first = peak_cell_at(first_node), second = peak_cell_at(second_node);
    // A shared displacement can put both tracks in their observed cells even when both predictions lag.
    if (spacing_fits_cells(&first, &second, neighbor->raw_x - track->raw_x, neighbor->raw_y - track->raw_y) ||
        spacing_fits_cells(&first, &second, neighbor->x - track->x, neighbor->y - track->y)) return 0;

    const struct peak_cell group = {
        first.left < second.left ? first.left : second.left,
        first.right > second.right ? first.right : second.right,
        first.top < second.top ? first.top : second.top,
        first.bottom > second.bottom ? first.bottom : second.bottom
    };
    return cell_contains_position(&group, neighbor->raw_x, neighbor->raw_y) ||
           cell_contains_position(&group, neighbor->x, neighbor->y);
}

static int has_adjacent_signal(const int *delta, int node) {
    const int row = node / NVT_COLUMNS, col = node % NVT_COLUMNS;
    for (int dr = -1; dr <= 1; ++dr) {
        if (row + dr < 0 || row + dr >= NVT_ROWS) continue;
        for (int dc = -1; dc <= 1; ++dc) {
            if ((!dr && !dc) || col + dc < 0 || col + dc >= NVT_COLUMNS) continue;
            if (delta[(row + dr) * NVT_COLUMNS + col + dc] >= NVT_NOISE_THRESHOLD) return 1;
        }
    }
    return 0;
}

static int fast_start_candidate(const struct nvt_touch_core *core,
                                const struct nvt_contact *found, int nfound,
                                int blob_label, int npeaks, int npalms,
                                int overflow, int had_active) {
    if (!core->timed_enabled || had_active || nfound != 1 || npeaks != 1 ||
        npalms || overflow || blob_label < 0 ||
        found[0].peak < 2 * NVT_DOWN_THRESHOLD) return 0;
    int low_row=NVT_ROWS, high_row=-1, low_col=NVT_COLUMNS, high_col=-1, support=0;
    for (int node=0;node<NVT_NODES;++node) {
        if (core->label[node] != blob_label || core->delta[node] < NVT_UP_THRESHOLD) continue;
        const int row=node/NVT_COLUMNS, col=node%NVT_COLUMNS;
        if (row<low_row)low_row=row;if(row>high_row)high_row=row;
        if (col<low_col)low_col=col;if(col>high_col)high_col=col;
        ++support;
    }
    const int rows=high_row-low_row+1, cols=high_col-low_col+1;
    const int span=2*NVT_MERGE_SPAN+1;
    // At least three strong nodes spanning both axes, bounded by the existing
    // merge-neighbourhood span, with >=50% support occupancy in their rectangle.
    // These are experiment eligibility choices, not learned hardware constants.
    return support>=3 && rows>=2 && cols>=2 && rows<=span && cols<=span &&
           support*2>=rows*cols;
}

struct candidate_workspace {
    struct nvt_contact found[MAX_PEAKS];
    int quality[MAX_PEAKS],palms[MAX_PEAKS][2];
    int blob_label[MAX_PEAKS];
    struct peak_bounds peak_bounds[MAX_PEAKS]; // Indexed by blob label, not candidate index.
    int count,npalms,npeaks,single_blob_label,overflow,had_active,clipped_count;
    int peak_active;
};
// Shared steepest-ascent / merge / centroid body. Full M passes the
// complete panel and has no clipping. Regional callers use only actual rows.
static void extract_candidates(struct nvt_touch_core *core,int first_row,int row_count,
                               const struct track_projection *projections,
                               struct candidate_workspace *result) {
    int *delta=core->delta;
    // Steepest-ascent labelling: every node above the noise floor flows to the local maximum above it.
    int *label=core->label;
    struct peak peaks[MAX_PEAKS];int npeaks=0,overflow=0;
    int replaced[MAX_PEAKS];
    for (int i=0;i<NVT_NODES;++i) label[i]=-1;
    for (int i=0;i<NVT_NODES;++i) {
        if (delta[i] < NVT_NOISE_THRESHOLD) continue;
        int cur=i;
        for (;;) {
            int best=cur,bv=delta[cur],r=cur/NVT_COLUMNS,c=cur%NVT_COLUMNS;
            for (int dr=-1;dr<=1;++dr) for (int dc=-1;dc<=1;++dc) {
                if (!dr && !dc) continue;
                int rr=r+dr,cc=c+dc;
                if (rr<0||rr>=NVT_ROWS||cc<0||cc>=NVT_COLUMNS) continue;
                int n=rr*NVT_COLUMNS+cc;
                if (delta[n] > bv || (delta[n]==bv && n>best && delta[n]>delta[cur])) {bv=delta[n];best=n;}
            }
            if (best==cur) break;
            cur=best;
        }
        // cur is a local maximum; register it as a peak the first time we see it. A full table drops its weakest maximum for a
        // stronger one (the nodes already labelled with it are cleared below), so a finger is never lost to a crowd of faint maxima.
        int found=-1;
        for (int p=0;p<npeaks;++p) if (peaks[p].index==cur) {found=p;break;}
        if (found<0) {
            // Reject an isolated new-contact peak before it can merge with
            // another spike. A node that climbed here already supplies support.
            if (cur == i && delta[cur] >= NVT_DOWN_THRESHOLD && !has_adjacent_signal(delta, cur)) continue;
            if (npeaks==MAX_PEAKS) {
                overflow=1;
                int weakest=0;
                for (int p=1;p<npeaks;++p) if (peaks[p].value<peaks[weakest].value) weakest=p;
                if (peaks[weakest].value>=delta[cur]) continue;
                peaks[weakest].index=cur;peaks[weakest].value=delta[cur];++replaced[weakest];found=weakest;
            } else {
                replaced[npeaks]=0;peaks[npeaks].index=cur;peaks[npeaks].value=delta[cur];found=npeaks++;
            }
        }
        label[i]=found | replaced[found]<<8;
    }
    if (overflow) ++core->peak_overflows;
    if ((unsigned long)npeaks>core->peaks_max) core->peaks_max=(unsigned long)npeaks;
    // A label names its peak and the peak's generation: nodes that flowed to a dropped maximum belong to nothing.
    for (int i=0;i<NVT_NODES;++i) if (label[i]>=0) label[i]=(label[i]>>8)==replaced[label[i]&0xff] ? (label[i]&0xff) : -1;
    // Outside coverage is unknown. Workspace zeros are never a negative
    // observation. Any component entering the two-node background/merge ring
    // is clipped and cannot use this complete-region centroid path.
    int clipped[MAX_PEAKS]={0},clipped_count=0;
    if(first_row>0 || first_row+row_count<NVT_ROWS) {
        for(int node=0;node<NVT_NODES;++node)if(label[node]>=0) {
            const int row=node/NVT_COLUMNS;
            if((first_row>0 && row<first_row+NVT_MERGE_SPAN) ||
               (first_row+row_count<NVT_ROWS && row>=first_row+row_count-NVT_MERGE_SPAN))
                clipped[label[node]]=1;
        }
    }
    // One finger can show two maxima a node or two apart (a ridge across it). Left as two blobs, the finger's centroid jumps by half a
    // node whenever the ridge moves; a peak with a stronger one within NVT_MERGE_SPAN nodes is folded into it.
    struct nvt_contact found[MAX_PEAKS];
    int match[NVT_SLOTS], reserved[MAX_PEAKS] = {0};
    int established = 0, had_active = 0;
    for (int s = 0; s < NVT_SLOTS; ++s) {
        if (!core->slots[s].active) continue;
        had_active=1;
        if (core->slots[s].age>=2) ++established;
    }
    if (established >= 2 && npeaks >= 2) {
        // Give established fingers a chance to keep separate maxima before merging
        // a ridge. The final assignment below uses the weighted blob centroids.
        for (int p = 0; p < npeaks; ++p) {
            int node = peaks[p].index;
            found[p].x = electrode_position(node % NVT_COLUMNS, 0);
            found[p].y = electrode_position(node / NVT_COLUMNS, 1);
            found[p].peak = peaks[p].value;
        }
        assign_tracks(core, found, npeaks, match, NULL, projections);
        int owner[MAX_PEAKS] = {0};
        for (int s = 0; s < NVT_SLOTS; ++s) {
            if (match[s] >= 0 && core->slots[s].age >= 2) owner[match[s]] = s + 1;
        }
        // A regional stripe supplies positive positions without the full-field context for lift.
        const int check_ridges = core->timed_enabled && first_row == 0 && row_count == NVT_ROWS;
        for (int p = 0; p < npeaks; ++p) {
            if (!owner[p]) continue;
            reserved[p] = 1;
            if (!check_ridges) continue;
            const struct track_projection *track = &projections[owner[p] - 1];
            const int64_t distance = track_distance(track, found[p].x, found[p].y);
            for (int q = 0; q < npeaks; ++q) {
                if (p == q || !owner[q] || !peaks_share_neighborhood(peaks[p].index, peaks[q].index)) continue;
                const struct track_projection *neighbor = &projections[owner[q] - 1];
                // A broad rejoin match must not split a ridge when both maxima fit the neighbor better.
                const int64_t neighbor_distance = track_distance(neighbor, found[p].x, found[p].y);
                if (neighbor_distance < distance && neighbor_distance <= neighbor->radius * neighbor->radius &&
                    track_distance(neighbor, found[q].x, found[q].y) <= track_distance(track, found[q].x, found[q].y) &&
                    ridge_fits_neighbor(peaks[p].index, peaks[q].index, track, neighbor)) {
                    reserved[p] = 0;
                    break;
                }
            }
        }
    }
    int into[MAX_PEAKS];
    for (int p=0;p<npeaks;++p) {
        into[p]=p;
        if (reserved[p]) continue;
        for (int q=0;q<npeaks;++q) {
            if (q==p || !peaks_share_neighborhood(peaks[p].index, peaks[q].index)) continue;
            const int best = into[p];
            if (reserved[q] != reserved[best]) {
                if (reserved[q]) into[p] = q;
            } else if (peaks[q].value > peaks[best].value ||
                       (peaks[q].value == peaks[best].value && q < best)) {
                into[p] = q;
            }
        }
    }
    for (int p=0;p<npeaks;++p) {
        int r=p;
        while (into[r]!=r) r=into[r];   // reserved peaks are roots, other links increase in rank
        if (r!=p) ++core->merges;
        into[p]=r;
    }
    for (int i=0;i<NVT_NODES;++i) if (label[i]>=0) label[i]=into[label[i]];
    for(int p=0;p<npeaks;++p)if(clipped[p])clipped[into[p]]=1;
    struct blob blobs[MAX_PEAKS];
    memset(blobs, 0, (size_t)npeaks * sizeof(blobs[0]));
    memset(result,0,sizeof(*result));
    // A peak reserved by an existing slot owns the group. Its amplitude can
    // be lower than the strongest signal which merged into that group.
    for (int p = 0; p < npeaks; ++p) {
        const int root = into[p];
        struct blob *b = &blobs[root];
        struct peak_bounds *bounds = &result->peak_bounds[root];
        const uint8_t row = (uint8_t)(peaks[p].index / NVT_COLUMNS);
        const uint8_t col = (uint8_t)(peaks[p].index % NVT_COLUMNS);
        if (!b->peak) *bounds = (struct peak_bounds){row, row, col, col};
        else {
            if (row < bounds->first_row) bounds->first_row = row;
            if (row > bounds->last_row) bounds->last_row = row;
            if (col < bounds->first_col) bounds->first_col = col;
            if (col > bounds->last_col) bounds->last_col = col;
        }
        if (peaks[p].value > b->peak) b->peak = peaks[p].value;
    }
    for (int p = 0; p < npeaks; ++p) {
        const int floor = blobs[p].peak * NVT_CENTROID_FLOOR / 100;
        blobs[p].floor = floor > NVT_COORDINATE_REDUCE ? floor : NVT_COORDINATE_REDUCE;
    }
    for (int i=0;i<NVT_NODES;++i) {
        if (label[i]<0) continue;
        struct blob *b=&blobs[label[i]];
        ++b->area;
        const int w = delta[i] - b->floor;
        if (w <= 0) continue;
        b->sum_w += w;
        b->sum_row += (int64_t)w * kRowMap[i / NVT_COLUMNS] * NVT_SUPER_RESOLUTION;
        b->sum_col += (int64_t)w * kColumnMap[i % NVT_COLUMNS] * NVT_SUPER_RESOLUTION;
        ++b->weighted_area;
    }
    int nfound=0,single_blob_label=-1,peak_active=0;
    int spatial_weak[MAX_PEAKS]={0};
    int palms[MAX_PEAKS][2];int npalms=0;
    for (int p=0;p<npeaks;++p) {
        struct blob *b=&blobs[p];
        if (!b->sum_w) continue;
        if(clipped[p]){++clipped_count;continue;}
        peak_active |= b->peak >= NVT_PEAK_THRESHOLD;
        // Starting a contact needs the down threshold, keeping one only the up threshold.
        int x=(int)((b->sum_col + b->sum_w/2) / b->sum_w);
        int y=(int)((b->sum_row + b->sum_w/2) / b->sum_w);
        if (x<0) x=0; if (x>NVT_LONG_AXIS_MAX-1) x=NVT_LONG_AXIS_MAX-1;
        if (y<0) y=0; if (y>NVT_SHORT_AXIS_MAX-1) y=NVT_SHORT_AXIS_MAX-1;
        x=nvt_touch_map_edge_axis(x,NVT_LONG_AXIS_MAX,0);
        y=nvt_touch_map_edge_axis(y,NVT_SHORT_AXIS_MAX,1);
        if (b->weighted_area>=NVT_PALM_MIN_NODES) {palms[npalms][0]=x;palms[npalms][1]=y;++npalms;continue;}
        found[nfound].slot=-1;found[nfound].tracking_id=0;found[nfound].x=x;found[nfound].y=y;
        found[nfound].peak=b->peak;found[nfound].area=b->area;found[nfound].weight=(int)b->sum_w;
        result->blob_label[nfound]=p;
        single_blob_label=p;
        if(!overflow && b->peak>=NVT_UP_THRESHOLD && b->peak<NVT_DOWN_THRESHOLD) {
            struct nvt_shape_metrics shape;
            // Match steepest-ascent support: labels admit delta>=NOISE. Using
            // floor=NOISE-1 retains those exact boundary nodes. Rank is exact
            // integer affine rank, not rounded covariance or probability.
            if(nvt_shape_measure(delta,label,p,NVT_NOISE_THRESHOLD-1,&shape)==1)
                spatial_weak[nfound]=shape.support_nodes>=3 && shape.spatial_rank==2;
        }
        ++nfound;
    }

    result->count=nfound;result->npalms=npalms;result->npeaks=npeaks;
    result->single_blob_label=single_blob_label;result->overflow=overflow;
    result->had_active=had_active;result->clipped_count=clipped_count;
    result->peak_active=peak_active;
    memcpy(result->found,found,(size_t)nfound*sizeof(found[0]));
    memcpy(result->quality,spatial_weak,(size_t)nfound*sizeof(spatial_weak[0]));
    memcpy(result->palms,palms,(size_t)npalms*sizeof(palms[0]));
}

static void remember_shape(struct nvt_slot *slot, const struct nvt_contact *contact, uint64_t now_us) {
    if (contact->peak < NVT_DOWN_THRESHOLD || contact->area <= 0) return;
    slot->shape_peak = contact->peak;
    slot->shape_area = contact->area;
    slot->shape_us = now_us;
}

// Apply accepted positions from full M, complete regions, and fitted P.
static void accept_position(struct nvt_touch_core *core,struct nvt_slot *slot,
                            int s,struct nvt_contact *contact,int complete) {
        const int dx=(*contact).x-slot->x,dy=(*contact).y-slot->y,ax=dx<0?-dx:dx,ay=dy<0?-dy:dy;
        const int px=slot->rx,py=slot->ry;
        const int weak=!core->timed_enabled && (*contact).peak<NVT_DOWN_THRESHOLD;
        const uint64_t filter_dt_us=!core->timed_enabled ? 0 :
            core->now_us>slot->last_filter_us ? core->now_us-slot->last_filter_us : 1;
        const int filter_applied=follow(slot,(*contact).x,(*contact).y,weak,filter_dt_us);
        uint64_t dt_us=0;
        if(core->timed_enabled) {
            dt_us=core->now_us>=slot->last_observed_us ? core->now_us-slot->last_observed_us : 1;
            if(dt_us<1)dt_us=1;
            if(dt_us>NVT_OBSERVATION_MAX_US)dt_us=NVT_OBSERVATION_MAX_US;
            slot->last_interval_us=dt_us;slot->last_observed_us=core->now_us;
            slot->last_presence_us=core->now_us;
            if(filter_applied)slot->last_filter_us=core->now_us;
            if(complete)remember_shape(slot,contact,core->now_us);
        }
        slot->dx=core->timed_enabled ? dx : dx/(slot->missed+1);
        slot->dy=core->timed_enabled ? dy : dy/(slot->missed+1);
        if (slot->missed) {++core->holds_bridged;slot->missed=0;}
        (*contact).slot=s;(*contact).tracking_id=slot->tracking_id;
        slot->x=(*contact).x;slot->y=(*contact).y;if(slot->age<__INT_MAX__)++slot->age;
        // Timed reporting keeps origin separate; the report timeline owns the
        // historical DOWN. Never overwrite the newest measured output here.
        if (!core->timed_enabled && slot->age == 2) {
            slot->rx = px;
            slot->ry = py;
        }
        (*contact).x=slot->rx;(*contact).y=slot->ry;
        if (complete && ax<4*NVT_FUZZ && ay<4*NVT_FUZZ) {
            const int ox=slot->rx>px?slot->rx-px:px-slot->rx,oy=slot->ry>py?slot->ry-py:py-slot->ry;
            ++core->jitter_frames;core->jitter_sum+=(unsigned long)(ax+ay);core->smoothed_sum+=(unsigned long)(ox+oy);
            if (ax>core->jitter_max) core->jitter_max=ax;
            if (ay>core->jitter_max) core->jitter_max=ay;
            if (ox>core->smoothed_max) core->smoothed_max=ox;
            if (oy>core->smoothed_max) core->smoothed_max=oy;
        }

}

static int process_matrix(struct nvt_touch_core *core, const int16_t *matrix, unsigned frame_type,
                          struct nvt_contact *out, int max) {
    if (frame_type != 2 && frame_type != 4) return 0;
    if (frame_type == 2 && !core->have_reference) {
        memcpy(core->reference, matrix, sizeof(core->reference));
        core->have_reference=1;core->startup_count=0;return -1;
    }
    if (!core->have_reference) {
        memcpy(core->startup[core->startup_count++], matrix, sizeof(core->reference));
        if (core->startup_count < NVT_STARTUP_FRAMES) return -1;
        form_reference(core);
        return -1;
    }
    ++core->frames_by_type[frame_type==4];
    measure_and_update_reference(core, matrix);

    struct track_projection projections[NVT_SLOTS];
    project_tracks(core, core->now_us, projections);
    struct candidate_workspace candidates;
    extract_candidates(core, 0, NVT_ROWS, projections, &candidates);
    struct nvt_contact *found=candidates.found;
    const int peak_active = candidates.peak_active;
    const int nfound=candidates.count,npalms=candidates.npalms,npeaks=candidates.npeaks;
    const int single_blob_label=candidates.single_blob_label;
    const int overflow=candidates.overflow,had_active=candidates.had_active;
    const int *spatial_weak=candidates.quality;
    const int (*palms)[2]=(const int (*)[2])candidates.palms;
    int match[NVT_SLOTS];
    // Assign eligible blobs as a group so neighboring fingers cannot take the
    // same candidate. Confirmed timed tracks choose before pending contacts.
    const int fast_start=fast_start_candidate(core,found,nfound,single_blob_label,npeaks,npalms,overflow,had_active);
    int used_contact[MAX_PEAKS]={0};
    uint32_t survivor_labels=0;
    assign_tracks(core, found, nfound, match, spatial_weak, projections);
    for (int s=0;s<NVT_SLOTS;++s) {
        const int matched=match[s];
        if (matched<0) continue;
        used_contact[matched]=1;
        if (!overflow && core->slots[s].age>=2 && found[matched].peak>=NVT_DOWN_THRESHOLD)
            survivor_labels |= UINT32_C(1) << candidates.blob_label[matched];
    }
    for (int s=0;s<NVT_SLOTS;++s) {
        struct nvt_slot *slot=&core->slots[s];
        if (!slot->active) continue;
        const int best=match[s];
        if (best<0) {
            const int visible = core->timed_enabled && slot->age >= 2;
            const int covered_negative = visible &&
                matrix_contact_quiet(core, &projections[s], candidates.peak_bounds, survivor_labels);
            int reason = NVT_MISS_NONE;
            if (!covered_negative) {
                reason = miss_reason(&projections[s], found, used_contact, nfound, palms, npalms,
                                     core->timed_enabled ? spatial_weak : NULL);
                // A local residual may have no publishable blob, for example
                // if the extractor discarded a one-electrode peak.
                if (visible && reason == NVT_MISS_NONE) reason = NVT_MISS_WEAK;
            }
            if (!slot->missed) slot->miss_reason = reason;
            // Keep a partial dropout, including a fading signal at the finger.
            // An otherwise quiet matrix with only distant noise is a release.
            ++slot->missed;
            const int legacy_hold=(peak_active || reason == NVT_MISS_WEAK) && slot->age >= 2 && slot->missed <= NVT_HOLD_FRAMES;
            // Aggressive policy never keeps an unmatched old point alongside a
            // distant/reassigned/palm peak. Only a weak nearby dropout is bridged.
            const int timed_hold=reason==NVT_MISS_WEAK && slot->age>=2 &&
                core->now_us>=slot->last_observed_us && core->now_us-slot->last_observed_us<NVT_DROPOUT_MAX_US;
            if(core->timed_enabled ? timed_hold : legacy_hold)continue;
            if(core->timed_enabled) {
                if(slot->age>=2) {
                    const unsigned bit=1u<<slot->tracking_id;
                    core->retired_ids|=bit;
                    if(covered_negative)
                        slot->last_covered_negative_us=core->now_us;
                    else
                        core->cancelled_ids|=bit;
                }
                if(reason==NVT_MISS_FAR || reason==NVT_MISS_TAKEN)++core->far_retired;
            }
            ++core->ends[covered_negative ? NVT_MISS_NONE : slot->miss_reason];slot->active=0;continue;
        }
        accept_position(core,slot,s,&found[best],1);
    }
    for (int c=0;c<nfound;++c) {
        if (used_contact[c] || found[c].peak<NVT_DOWN_THRESHOLD) continue;
        for (int s=0;s<NVT_SLOTS;++s) {
            if (core->slots[s].active) continue;
            // Preserve the legacy allocation order exactly for the old API.
            if(!core->timed_enabled)core->slots[s].active=1;
            const int id=free_tracking_id(core);
            if(!id){++core->id_quarantine_drops;continue;}
            core->slots[s].active=1;core->slots[s].tracking_id=id;core->slots[s].age=fast_start ? 2 : 1;core->slots[s].missed=0;
            if(core->timed_enabled) {
                core->slots[s].born_us=core->now_us;core->slots[s].last_observed_us=core->now_us;
                core->slots[s].last_presence_us=core->now_us;
                core->slots[s].last_covered_negative_us=0;
                core->slots[s].last_interval_us=SCAN_INTERVAL_US;
                core->slots[s].last_filter_us=core->now_us; // Initial measured filter seed.
                core->slots[s].shape_peak=0;
                remember_shape(&core->slots[s], &found[c], core->now_us);
            }
            core->slots[s].x=found[c].x;core->slots[s].y=found[c].y;
            core->slots[s].origin_x=found[c].x;core->slots[s].origin_y=found[c].y;
            core->slots[s].generation=++core->next_generation;
            seed_filter(&core->slots[s], found[c].x, found[c].y);
            core->slots[s].dx=core->slots[s].dy=0;
            if(!core->timed_enabled)core->slots[s].last_filter_us=0;
            found[c].slot=s;found[c].tracking_id=core->slots[s].tracking_id;used_contact[c]=1;break;
        }
    }
    int count=0;
    for (int c=0;c<nfound && count<max;++c) {
        if (found[c].slot<0 || core->slots[found[c].slot].age<2) continue;   // report from the second frame on
        out[count++]=found[c];
    }
    for (int s=0;s<NVT_SLOTS && count<max;++s) {   // held fingers, where they were last reported
        const struct nvt_slot *slot=&core->slots[s];
        if (!slot->active || !slot->missed || slot->age<2) continue;
        out[count].slot=s;out[count].tracking_id=slot->tracking_id;out[count].x=slot->rx;out[count].y=slot->ry;
        out[count].peak=out[count].area=out[count].weight=0;++count;
    }
    return count;
}


static int visible_set(const struct nvt_touch_core *core,struct nvt_contact *out,int max) {
    int n=0;
    for(int s=0;s<NVT_SLOTS && n<max;++s) {
        const struct nvt_slot *slot=&core->slots[s];
        if(!slot->active || slot->age<2)continue;
        out[n].slot=s;out[n].tracking_id=slot->tracking_id;out[n].x=slot->rx;out[n].y=slot->ry;
        out[n].peak=out[n].area=out[n].weight=0;++n;
    }
    return n;
}

int nvt_touch_extract_region_at(struct nvt_touch_core *core, const int16_t *rows,
                               int first_row, int row_count, uint64_t now_us,
                               struct nvt_region_candidates *out) {
    if(!core || !rows || !out || first_row<0 || row_count<1 ||
       first_row>NVT_ROWS-row_count || !core->have_reference)return -1;
    // This scratch raster carries no lifetime or negative-evidence semantics.
    // Only actual covered values enter subtraction; outside is masked UNKNOWN.
    memset(core->delta,0,sizeof(core->delta));
    for(int r=0;r<row_count;++r)for(int col=0;col<NVT_COLUMNS;++col) {
        const int node=(first_row+r)*NVT_COLUMNS+col;
        core->delta[node]=(int)rows[r*NVT_COLUMNS+col]-core->reference[node];
    }
    struct track_projection projections[NVT_SLOTS];
    project_tracks(core, now_us, projections);
    struct candidate_workspace w;
    extract_candidates(core, first_row, row_count, projections, &w);
    memset(out,0,sizeof(*out));
    out->count=w.count;out->clipped_count=w.clipped_count;out->overflow=w.overflow;
    out->first_row=first_row;out->row_count=row_count;
    memcpy(out->contact,w.found,(size_t)w.count*sizeof(w.found[0]));
    for(int c=0;c<w.count;++c)if(w.quality[c])out->quality_mask|=1u<<c;
    return w.count;
}

int nvt_touch_extract_region(struct nvt_touch_core *core, const int16_t *rows,
                             int first_row, int row_count, struct nvt_region_candidates *out) {
    return nvt_touch_extract_region_at(core, rows, first_row, row_count, core ? core->now_us : 0, out);
}

int nvt_touch_process_region(struct nvt_touch_core *core,
                             const struct nvt_region_candidates *observation,
                             uint64_t now_us,struct nvt_contact *out,int max) {
    if(!core || !observation || !out || max<0 ||
       observation->count<0 || observation->count>NVT_REGION_MAX_CANDIDATES ||
       observation->first_row<0 || observation->row_count<1 ||
       observation->first_row>NVT_ROWS-observation->row_count)return -1;
    if((core->clock_valid && now_us<core->now_us) ||
       (core->region_clock_valid && now_us<=core->last_region_us))return -1;
    const int nfound=observation->count;
    struct nvt_contact found[MAX_PEAKS];
    int quality[MAX_PEAKS]={0},match[NVT_SLOTS],used[MAX_PEAKS]={0};
    for(int c=0;c<nfound;++c) {
        found[c]=observation->contact[c];
        if(found[c].x<0 || found[c].x>=NVT_LONG_AXIS_MAX ||
           found[c].y<0 || found[c].y>=NVT_SHORT_AXIS_MAX || found[c].peak<0)return -1;
        quality[c]=(observation->quality_mask&(1u<<c))!=0;
    }
    int changed=nvt_touch_begin_observation(core,now_us,out,max)>=0;
    core->region_clock_valid=1;core->last_region_us=now_us;
    struct track_projection projections[NVT_SLOTS];
    project_tracks(core, now_us, projections);
    assign_tracks(core, found, nfound, match, quality, projections);
    for(int s=0;s<NVT_SLOTS;++s)if(match[s]>=0)used[match[s]]=1;
    for(int s=0;s<NVT_SLOTS;++s) {
        struct nvt_slot *slot=&core->slots[s];
        if(!slot->active || match[s]<0)continue;
        if(now_us<=slot->last_observed_us)continue;
        accept_position(core,slot,s,&found[match[s]],1);++changed;
    }
    for(int c=0;c<nfound;++c) {
        if(used[c] || found[c].peak<NVT_DOWN_THRESHOLD)continue;
        // A strip cannot prove the global single-peak condition needed by the
        // existing high-confidence fast path. Use ordinary independent samples.
        for(int s=0;s<NVT_SLOTS;++s)if(!core->slots[s].active) {
            const int id=free_tracking_id(core);if(!id){++core->id_quarantine_drops;break;}
            struct nvt_slot *slot=&core->slots[s];
            memset(slot,0,sizeof(*slot));slot->active=1;slot->tracking_id=id;slot->age=1;
            slot->x=slot->origin_x=found[c].x;
            slot->y=slot->origin_y=found[c].y;
            seed_filter(slot, found[c].x, found[c].y);
            slot->born_us=slot->last_observed_us=slot->last_filter_us=slot->last_presence_us=now_us;
            slot->generation=++core->next_generation;slot->last_interval_us=SCAN_INTERVAL_US;
            remember_shape(slot, &found[c], now_us);
            ++changed;break;
        }
    }
    // No match in this region is UNKNOWN. Never increment missed, infer UP,
    // renew an unobserved ID, update the baseline or run the full-M negative path.
    return changed ? visible_set(core,out,max) : -1;
}

int nvt_touch_process_partial(struct nvt_touch_core *core,
                             const struct nvt_contact *measured,int count,
                             uint64_t now_us,struct nvt_contact *out,int max) {
    if(!core || !out || max<0 || count<0 || count>NVT_SLOTS || (count && !measured))return -1;
    if(core->clock_valid && now_us<core->now_us)return -1;
    unsigned seen=0;
    for(int j=0;j<count;++j) {
        const int id=measured[j].tracking_id;
        if(id<1 || id>NVT_SLOTS || (seen&(1u<<id)) ||
           measured[j].x<0 || measured[j].x>=NVT_LONG_AXIS_MAX ||
           measured[j].y<0 || measured[j].y>=NVT_SHORT_AXIS_MAX ||
           measured[j].peak<NVT_UP_THRESHOLD)return -1;
        seen|=1u<<id;
    }
    int changed=nvt_touch_begin_observation(core,now_us,out,max)>=0;
    struct track_projection projection[NVT_SLOTS];
    project_tracks(core, now_us, projection);
    for(int j=0;j<count;++j)for(int s=0;s<NVT_SLOTS;++s) {
        struct nvt_slot *slot=&core->slots[s];
        if(!slot->active || slot->age<2 || slot->tracking_id!=measured[j].tracking_id)continue;
        if(now_us<=slot->last_observed_us)break;
        const struct track_projection *p = &projection[s];
        if (track_distance(p, measured[j].x, measured[j].y) > p->radius * p->radius) break;
        struct nvt_contact position=measured[j];
        accept_position(core,slot,s,&position,0);
        ++changed;break;
    }
    // Unmeasured slots keep their actual position timestamp. No fake full
    // raster, negative evidence, baseline update or hidden contact creation.
    return changed ? visible_set(core,out,max) : -1;
}

int nvt_touch_note_presence(struct nvt_touch_core *core,int tracking_id,
                           uint64_t generation,uint64_t now_us) {
    if(!core || !generation || (core->clock_valid && now_us<core->now_us))return 0;
    for(int s=0;s<NVT_SLOTS;++s) {
        struct nvt_slot *slot=&core->slots[s];
        if(!slot->active || slot->age<2 || slot->tracking_id!=tracking_id || slot->generation!=generation)continue;
        if(now_us<slot->last_presence_us || now_us<slot->last_observed_us ||
           now_us-slot->last_observed_us>=NVT_OBSERVATION_MAX_US)return 0;
        slot->last_presence_us=now_us;return 1;
    }
    return 0;
}

int nvt_touch_covered_negative(struct nvt_touch_core *core,int tracking_id,
                              uint64_t generation,uint64_t now_us,
                              struct nvt_contact *out,int max) {
    if(!core || !out || max<0 || !generation ||
       (core->clock_valid && now_us<core->now_us))return -1;
    for(int s=0;s<NVT_SLOTS;++s) {
        struct nvt_slot *slot=&core->slots[s];
        if(!slot->active || slot->age<2 || slot->tracking_id!=tracking_id || slot->generation!=generation)continue;
        if(now_us<slot->last_observed_us)return -1;
        core->timed_enabled=1;core->clock_valid=1;core->now_us=now_us;
        slot->last_covered_negative_us=now_us;
        core->retired_ids|=1u<<tracking_id;slot->active=0;
        ++core->ends[NVT_MISS_NONE];
        return visible_set(core,out,max);
    }
    return -1;
}

int nvt_touch_quiet_stripe_for_ids(struct nvt_touch_core *core, const int16_t *rows,
                                 int first_row, int row_count, uint64_t previous_us,
                                 uint64_t now_us, uint32_t ids, struct nvt_contact *out, int max) {
    if(!core || !core->timed_enabled || !core->have_reference || !rows || !out || max<0 ||
       first_row<0 || row_count<1 || first_row>NVT_ROWS-row_count ||
       !ids || now_us<=previous_us || (core->clock_valid && now_us<core->now_us))return -1;
    const int last_row=first_row+row_count-1;
    // Check the entire width of the observed stripe for a moving contact.
    for(int i=0;i<row_count*NVT_COLUMNS;++i) {
        const int delta=(int)rows[i]-core->reference[first_row*NVT_COLUMNS+i];
        if(delta<=-NVT_NOISE_THRESHOLD || delta>=NVT_NOISE_THRESHOLD)return -1;
    }
    const int top=nvt_touch_map_edge_axis(kRowMap[first_row]*NVT_SUPER_RESOLUTION,NVT_SHORT_AXIS_MAX,1);
    const int bottom=nvt_touch_map_edge_axis(kRowMap[last_row]*NVT_SUPER_RESOLUTION,NVT_SHORT_AXIS_MAX,1);
    struct track_projection projections[NVT_SLOTS];
    project_tracks(core, now_us, projections);
    int changed=0;
    for(int s=0;s<NVT_SLOTS;++s) {
        const struct nvt_slot *slot=&core->slots[s];
        if (!slot->active || slot->age < 2 || !(ids & (UINT32_C(1) << slot->tracking_id)) ||
            slot->last_observed_us >= now_us || slot->last_presence_us >= now_us) continue;
        const uint64_t gap = now_us - slot->last_observed_us;
        if (gap > NVT_DROPOUT_MAX_US) continue;
        const struct track_projection *p=&projections[s];
        // Allow acceleration during an unobserved interval. The default
        // estimate reaches 200 px in two nominal scans and grows with gap^2.
        const uint64_t two_scans = 2 * SCAN_INTERVAL_US;
        const uint64_t span_squared = two_scans * two_scans;
        const int reach = (int)((TRACK_RADIUS * gap * gap + span_squared - 1) / span_squared);
        const int radius = reach > TRACK_RADIUS ? reach : TRACK_RADIUS;
        const int low=(p->y<p->raw_y ? p->y : p->raw_y)-radius;
        const int high=(p->y>p->raw_y ? p->y : p->raw_y)+radius;
        if((first_row!=0 && low<top) || (last_row!=NVT_ROWS-1 && high>bottom))continue;
        if(nvt_touch_covered_negative(core,slot->tracking_id,slot->generation,now_us,out,max)>=0)changed=1;
    }
    return changed ? visible_set(core,out,max) : -1;
}

int nvt_touch_quiet_stripe(struct nvt_touch_core *core, const int16_t *rows,
                          int first_row, int row_count, uint64_t previous_us, uint64_t now_us,
                          struct nvt_contact *out, int max) {
    return nvt_touch_quiet_stripe_for_ids(core, rows, first_row, row_count, previous_us, now_us,
                                         UINT32_MAX, out, max);
}

static int expire_contacts(struct nvt_touch_core *core, uint64_t now_us, int before_observation,
                           struct nvt_contact *out, int max) {
    if(!core || !out || max<0)return -1;
    core->timed_enabled=1;
    const int rollback=core->clock_valid && now_us<core->now_us;
    if (before_observation && (!core->clock_valid || now_us != core->now_us)) {
        core->previous_retired_ids=core->retired_ids;
        core->retired_ids=0;
    }
    if(rollback) {
        ++core->clock_rebases;
        core->region_clock_valid=0;core->last_region_us=0;
    }
    core->clock_valid=1;core->now_us=now_us;
    int visible_changed=0;
    for(int s=0;s<NVT_SLOTS;++s) {
        struct nvt_slot *slot=&core->slots[s];
        if(!slot->active)continue;
        const int pending=slot->age<2;
        const uint64_t since=pending ? slot->born_us : slot->last_observed_us;
        const uint64_t elapsed=now_us>=since ? now_us-since : 0;
        const uint64_t deadline=pending ? NVT_PENDING_MAX_US :
            slot->missed ? NVT_DROPOUT_MAX_US : NVT_OBSERVATION_MAX_US;
        // An observation stamped exactly at a visible deadline gets a chance
        // to match before the timer expires the remaining slots.
        const int overdue=elapsed>deadline || (elapsed==deadline && (pending || !before_observation));
        if(!rollback && !overdue)continue;
        if(pending)++core->pending_expired;
        else {
            const unsigned bit=1u<<slot->tracking_id;
            core->retired_ids|=bit;core->cancelled_ids|=bit;visible_changed=1;
            if(slot->missed && overdue)++core->dropout_expired;
            else ++core->observation_cancelled;
        }
        slot->active=0;
    }
    return visible_changed ? visible_set(core,out,max) : -1;
}

int nvt_touch_begin_observation(struct nvt_touch_core *core, uint64_t now_us,
                                struct nvt_contact *out, int max) {
    return expire_contacts(core,now_us,1,out,max);
}

int nvt_touch_expire(struct nvt_touch_core *core,uint64_t now_us,struct nvt_contact *out,int max) {
    return expire_contacts(core,now_us,0,out,max);
}

uint64_t nvt_touch_wait_us(const struct nvt_touch_core *core,uint64_t now_us) {
    if(!core)return 0;
    uint64_t wait=0;
    for(int s=0;s<NVT_SLOTS;++s) {
        const struct nvt_slot *slot=&core->slots[s];
        if(!slot->active)continue;
        const uint64_t base=slot->age<2 ? slot->born_us : slot->last_observed_us;
        const uint64_t limit=slot->age<2 ? NVT_PENDING_MAX_US : slot->missed ? NVT_DROPOUT_MAX_US : NVT_OBSERVATION_MAX_US;
        const uint64_t elapsed=now_us>=base ? now_us-base : 0;
        const uint64_t remaining=elapsed>=limit ? 1 : limit-elapsed;
        if(!wait || remaining<wait)wait=remaining;
    }
    return wait;
}

int nvt_touch_process_timed(struct nvt_touch_core *core,const int16_t *matrix,unsigned frame_type,
                           uint64_t now_us,struct nvt_contact *out,int max) {
    if(!core || !matrix || !out || max<0 || (frame_type!=2 && frame_type!=4))return -1;
    const int expired=nvt_touch_begin_observation(core,now_us,out,max);
    core->previous_matrix_us=now_us;core->matrix_clock_valid=1;
    const int observed=process_matrix(core,matrix,frame_type,out,max);
    return observed>=0 ? observed : expired;
}

int nvt_touch_process(struct nvt_touch_core *core,const int16_t *matrix,unsigned frame_type,
                      struct nvt_contact *out,int max) {
    core->timed_enabled=0;
    return process_matrix(core,matrix,frame_type,out,max);
}
