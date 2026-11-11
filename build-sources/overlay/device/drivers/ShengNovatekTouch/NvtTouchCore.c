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
// Across an unobserved frame only rejoin within the measured jitter range of
// the predicted position. A nearby new keystroke can be inside TRACK_RADIUS.
#define REJOIN_RADIUS (4*NVT_FUZZ)

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

// Smoothing factor (1/65536) of a first-order low-pass filter with this cutoff, sampled once a frame: w / (1 + w), w = 2 pi fc / rate.
static long filter_alpha(long cutoff_mhz, uint64_t dt_us) {
    uint64_t w;
    if (dt_us) {
        // A weak matched stream can keep the contact alive without calling the
        // position filter for much longer than the100ms observation budget.
        // Saturation here is exact at16-bit alpha precision, not a new timeout.
        if ((uint64_t)cutoff_mhz > UINT64_MAX / dt_us) return 65535;
        const uint64_t product=(uint64_t)cutoff_mhz*dt_us;
        w=(product/UINT64_C(1000000000))*411775+
          ((product%UINT64_C(1000000000))*411775)/UINT64_C(1000000000);
    } else w=(uint64_t)(cutoff_mhz*411775/(1000L*NVT_FRAME_HZ));
    // floor(65536*w/(65536+w)) ==65536-ceil(2^32/(65536+w)).
    // The equivalent ratio avoids w*65536 overflow for long filter intervals;
    // ordinary and legacy arithmetic retain exactly the old integer alpha.
    const uint64_t denominator=65536+w,numerator=UINT64_C(1)<<32;
    return (long)(65536-(numerator/denominator+(numerator%denominator!=0)));
}

// The reported position of a tracked finger for its raw one: an adaptive low-pass filter whose cutoff follows the finger's smoothed
// speed (NvtTouchCore.h). It is continuous: no movement is held back and released in one step.
// Measurement acceptance is owned by the tracker. Every accepted timed strong
// or weak position reaches this function; source/rank cannot silently freeze q.
// The legacy untimed API retains its old weak-follow behavior for comparison.
static int follow(struct nvt_slot *slot, int x, int y, int weak, uint64_t dt_us) {
    if (weak) return 0;
    // Legacy calls keep their exact fixed-rate arithmetic. Timed calls use the
    // actual interval since this slot's position/filter state was updated.
    // The bounded coordinate residual is smaller than INT64_MAX; for a larger
    // elapsed value its integer derivative is0. Avoid unsigned-to-signed wrap.
    const uint64_t velocity_dt=dt_us>(uint64_t)INT64_MAX ? (uint64_t)INT64_MAX : dt_us;
    const long sx=dt_us ? (long)(((int64_t)x-slot->filter_raw_x)*1000000/(int64_t)velocity_dt) :
        ((long)x*16-slot->fx)*NVT_FRAME_HZ/16;
    const long sy=dt_us ? (long)(((int64_t)y-slot->filter_raw_y)*1000000/(int64_t)velocity_dt) :
        ((long)y*16-slot->fy)*NVT_FRAME_HZ/16;
    // Do not extrapolate stale velocity memory across an observation gap. The
    // new endpoint difference still uses its actual elapsed measurement time.
    if(dt_us>NVT_DROPOUT_MAX_US)slot->vx=slot->vy=0;
    const long speed_alpha=filter_alpha(NVT_SPEED_CUTOFF_MHZ,dt_us);
    slot->vx+=(sx-slot->vx)*speed_alpha/65536;slot->vy+=(sy-slot->vy)*speed_alpha/65536;
    const long speed=(slot->vx<0?-slot->vx:slot->vx)+(slot->vy<0?-slot->vy:slot->vy);
    const long alpha=filter_alpha(NVT_MIN_CUTOFF_MHZ+speed*NVT_CUTOFF_PER_SPEED/2,dt_us);
    slot->fx+=((long)x*16-slot->fx)*alpha/65536;slot->fy+=((long)y*16-slot->fy)*alpha/65536;
    slot->rx=(int)((slot->fx+8)/16);slot->ry=(int)((slot->fy+8)/16);
    slot->filter_raw_x=x;slot->filter_raw_y=y;
    return 1;
}

// The reason a tracked finger at (x, y) matched none of this frame's blobs, judged by the nearest blob of any kind.
static int miss_reason(const struct nvt_slot *slot, const struct nvt_contact *found, const int *used, int nfound,
                       const int (*palms)[2], int npalms, const int *quality) {
    long best=-1;int reason=NVT_MISS_NONE;
    for (int c=0;c<nfound;++c) {
        const long dx=found[c].x-slot->x,dy=found[c].y-slot->y,d=dx*dx+dy*dy;
        if (best>=0 && d>=best) continue;
        best=d;reason=used[c] ? NVT_MISS_TAKEN :
            (found[c].peak<NVT_UP_THRESHOLD ||
             (quality && found[c].peak<NVT_DOWN_THRESHOLD && !quality[c])) ? NVT_MISS_WEAK : NVT_MISS_FAR;
    }
    for (int p=0;p<npalms;++p) {
        const long dx=palms[p][0]-slot->x,dy=palms[p][1]-slot->y,d=dx*dx+dy*dy;
        if (best<0 || d<best) {best=d;reason=NVT_MISS_PALM;}
    }
    if (best>(long)TRACK_RADIUS*TRACK_RADIUS && reason!=NVT_MISS_NONE) reason=NVT_MISS_FAR;
    if (reason == NVT_MISS_WEAK && best > (long)REJOIN_RADIUS * REJOIN_RADIUS) reason = NVT_MISS_FAR;
    return reason;
}

// One shared measured-position / bounded-motion hypothesis for timed strong
// and weak measurements. Legacy callers retain the original prediction.
struct track_projection {long x, y, raw_x, raw_y;int timed;};

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
static long point_cost(int x,int y,long px,long py) {
    const long dx=(long)x-px,dy=(long)y-py;return dx*dx+dy*dy;
}
static void project_tracks(const struct nvt_touch_core *core,int predict,
                           struct track_projection *projection) {
    const uint64_t r2=(uint64_t)TRACK_RADIUS*TRACK_RADIUS;
    for(int s=0;s<NVT_SLOTS;++s) {
        const struct nvt_slot *slot=&core->slots[s];
        if(!slot->active)continue;
        struct track_projection *q=&projection[s];q->timed=core->timed_enabled!=0;
        q->raw_x=slot->x;q->raw_y=slot->y;
        long sx=0,sy=0;
        if(!core->timed_enabled) {
            const int steps=predict ? slot->missed+1 : 0;
            sx=(long)slot->dx*steps;sy=(long)slot->dy*steps;
        } else {
            const uint64_t gap=core->now_us>=slot->last_observed_us ? core->now_us-slot->last_observed_us : UINT64_MAX;
            if(predict && slot->age>=2 && !slot->missed && gap<=NVT_DROPOUT_MAX_US) {
                // Limited displacement, not velocity*time extrapolation. At a
                // stop/reversal the measured h0 remains an equally valid gate.
                sx=(long)slot->dx*2/3;sy=(long)slot->dy*2/3;
                const uint64_t norm2=(uint64_t)(sx*sx)+(uint64_t)(sy*sy);
                if(norm2>r2) {
                    uint64_t norm=floor_sqrt_u64(norm2);if(norm*norm<norm2)++norm;
                    sx=(long)((int64_t)sx*TRACK_RADIUS/(int64_t)norm);
                    sy=(long)((int64_t)sy*TRACK_RADIUS/(int64_t)norm);
                }
            }
        }
        q->x=(long)slot->x+sx;q->y=(long)slot->y+sy;
    }
}
static long track_cost(const struct nvt_slot *slot,const struct nvt_contact *found,
                       int nfound,int j,const struct track_projection *projection,
                       const int *quality) {
    const long r2=(long)TRACK_RADIUS*TRACK_RADIUS;
    if(j>=nfound)return r2;
    if(found[j].peak<NVT_UP_THRESHOLD ||
       (projection->timed && quality && found[j].peak<NVT_DOWN_THRESHOLD && !quality[j]))return 4*r2;
    long d=point_cost(found[j].x,found[j].y,projection->x,projection->y);
    if(projection->timed) {
        const long raw=point_cost(found[j].x,found[j].y,projection->raw_x,projection->raw_y);
        if(raw<d)d=raw;
    }
    const long limit=slot->missed ? (long)REJOIN_RADIUS*REJOIN_RADIUS : r2;
    return d<=limit ? d : 4*r2;
}

// Least-cost assignment of the active fingers (rows) to the blobs plus one "no blob" column per finger (Hungarian method with
// potentials, O(rows^2 * columns)); match[slot] gets the blob or -1.
static void assign_tracks(const struct nvt_touch_core *core, const struct nvt_contact *found, int nfound, int *match, const int *quality) {
    enum {COLUMNS=MAX_PEAKS+NVT_SLOTS};
    int rows[NVT_SLOTS+1],n=0;
    for (int s=0;s<NVT_SLOTS;++s) {match[s]=-1;if (core->slots[s].active) rows[++n]=s;}
    if (!n) return;
    // During a four/five-finger pinch the fingers can stop together. Extrapolating
    // all their velocities through the stop can swap neighbours at the centre.
    const int predict = n <= 3;
    struct track_projection projection[NVT_SLOTS];
    project_tracks(core,predict,projection);
    const int m=nfound+n;
    long u[NVT_SLOTS+1]={0},v[COLUMNS+1]={0},minv[COLUMNS+1];
    int p[COLUMNS+1]={0},way[COLUMNS+1]={0};char used[COLUMNS+1];
    for (int i=1;i<=n;++i) {
        p[0]=i;int j0=0;
        for (int j=0;j<=m;++j) {minv[j]=__LONG_MAX__;used[j]=0;}
        do {
            used[j0]=1;
            const int i0=p[j0];long delta=__LONG_MAX__;int j1=0;
            for (int j=1;j<=m;++j) {
                if (used[j]) continue;
                const long cur=track_cost(&core->slots[rows[i0]],found,nfound,j-1,&projection[rows[i0]],quality)-u[i0]-v[j];
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
    const long r2=(long)TRACK_RADIUS*TRACK_RADIUS;
    for (int j=1;j<=nfound;++j) {
        if (!p[j]) continue;
        const int s=rows[p[j]];
        if (track_cost(&core->slots[s],found,nfound,j-1,&projection[s],quality)<=r2) match[s]=j-1;
    }
}

static void sort_i16(int16_t *v, unsigned n) {
    for (unsigned i=1;i<n;++i) {int16_t x=v[i];unsigned j=i;while(j>0 && v[j-1]>x){v[j]=v[j-1];--j;}v[j]=x;}
}

static void form_reference(struct nvt_touch_core *core) {
    int16_t column[NVT_STARTUP_FRAMES];
    for (unsigned node=0;node<NVT_NODES;++node) {
        for (unsigned s=0;s<NVT_STARTUP_FRAMES;++s) column[s]=core->startup[s][node];
        sort_i16(column,NVT_STARTUP_FRAMES);
        core->reference[node]=(int16_t)(((int)column[NVT_STARTUP_FRAMES/2-1]+(int)column[NVT_STARTUP_FRAMES/2])/2);
    }
    core->have_reference=1;core->startup_count=0;
}

struct peak {int index, value;};
struct blob {long sum_w, sum_row, sum_col; int peak, area, label;};

static int delta_at(const int16_t *m, const int16_t *ref, int index) {return (int)m[index]-(int)ref[index];}

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
    int count,npalms,npeaks,single_blob_label,overflow,had_active,clipped_count;
};
// Shared literal steepest-ascent / merge / centroid body. Full M passes the
// complete panel and has no clipping. Regional callers use only actual rows.
static void extract_candidates(struct nvt_touch_core *core,int first_row,int row_count,
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
            found[p].x = nvt_touch_map_edge_axis(kColumnMap[node % NVT_COLUMNS] * NVT_SUPER_RESOLUTION, NVT_LONG_AXIS_MAX, 0);
            found[p].y = nvt_touch_map_edge_axis(kRowMap[node / NVT_COLUMNS] * NVT_SUPER_RESOLUTION, NVT_SHORT_AXIS_MAX, 1);
            found[p].peak = peaks[p].value;
        }
        assign_tracks(core, found, npeaks, match, NULL);
        for (int s = 0; s < NVT_SLOTS; ++s)
            if (match[s] >= 0 && core->slots[s].age >= 2) reserved[match[s]] = 1;
    }
    int into[MAX_PEAKS];
    for (int p=0;p<npeaks;++p) {
        into[p]=p;
        if (reserved[p]) continue;
        int pr=peaks[p].index/NVT_COLUMNS,pc=peaks[p].index%NVT_COLUMNS;
        for (int q=0;q<npeaks;++q) {
            if (q==p) continue;
            int dr=peaks[q].index/NVT_COLUMNS-pr,dc=peaks[q].index%NVT_COLUMNS-pc;
            if (dr<-NVT_MERGE_SPAN||dr>NVT_MERGE_SPAN||dc<-NVT_MERGE_SPAN||dc>NVT_MERGE_SPAN) continue;
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
    for (int p=0;p<npeaks;++p) memset(&blobs[p],0,sizeof(blobs[p])),blobs[p].peak=peaks[p].value,blobs[p].label=p;
    for (int i=0;i<NVT_NODES;++i) {
        if (label[i]<0) continue;
        struct blob *b=&blobs[label[i]];
        int floor=peaks[label[i]].value*NVT_CENTROID_FLOOR/100;if (floor<NVT_COORDINATE_REDUCE) floor=NVT_COORDINATE_REDUCE;
        int w=delta[i]-floor;if (w<0) w=0;
        b->sum_w+=w;b->sum_row+=(long)w*kRowMap[i/NVT_COLUMNS]*NVT_SUPER_RESOLUTION;
        b->sum_col+=(long)w*kColumnMap[i%NVT_COLUMNS]*NVT_SUPER_RESOLUTION;++b->area;
    }
    int nfound=0,single_blob_label=-1;
    int spatial_weak[MAX_PEAKS]={0};
    int palms[MAX_PEAKS][2];int npalms=0;
    for (int p=0;p<npeaks;++p) {
        struct blob *b=&blobs[p];
        if (!b->sum_w) continue;
        if(clipped[p]){++clipped_count;continue;}
        // Starting a contact needs the down threshold, keeping one only the up threshold.
        int x=(int)((b->sum_col + b->sum_w/2) / b->sum_w);
        int y=(int)((b->sum_row + b->sum_w/2) / b->sum_w);
        if (x<0) x=0; if (x>NVT_LONG_AXIS_MAX-1) x=NVT_LONG_AXIS_MAX-1;
        if (y<0) y=0; if (y>NVT_SHORT_AXIS_MAX-1) y=NVT_SHORT_AXIS_MAX-1;
        x=nvt_touch_map_edge_axis(x,NVT_LONG_AXIS_MAX,0);
        y=nvt_touch_map_edge_axis(y,NVT_SHORT_AXIS_MAX,1);
        if (b->area>=NVT_PALM_MIN_NODES) {palms[npalms][0]=x;palms[npalms][1]=y;++npalms;continue;}   // palm-sized, not reported
        found[nfound].slot=-1;found[nfound].tracking_id=0;found[nfound].x=x;found[nfound].y=y;
        found[nfound].peak=b->peak;found[nfound].area=b->area;found[nfound].weight=(int)b->sum_w;
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

    memset(result,0,sizeof(*result));
    result->count=nfound;result->npalms=npalms;result->npeaks=npeaks;
    result->single_blob_label=single_blob_label;result->overflow=overflow;
    result->had_active=had_active;result->clipped_count=clipped_count;
    memcpy(result->found,found,(size_t)nfound*sizeof(found[0]));
    memcpy(result->quality,spatial_weak,(size_t)nfound*sizeof(spatial_weak[0]));
    memcpy(result->palms,palms,(size_t)npalms*sizeof(palms[0]));
}

// One accepted measurement path for full M and complete PM regions.
static void accept_position(struct nvt_touch_core *core,struct nvt_slot *slot,
                            int s,struct nvt_contact *contact) {
        const int dx=(*contact).x-slot->x,dy=(*contact).y-slot->y,ax=dx<0?-dx:dx,ay=dy<0?-dy:dy;
        uint64_t dt_us=0;
        if(core->timed_enabled) {
            dt_us=core->now_us>=slot->last_observed_us ? core->now_us-slot->last_observed_us : 1;
            if(dt_us<1)dt_us=1;
            if(dt_us>NVT_OBSERVATION_MAX_US)dt_us=NVT_OBSERVATION_MAX_US;
            slot->last_interval_us=dt_us;slot->last_observed_us=core->now_us;
            slot->last_presence_us=core->now_us;
        }
        slot->dx=core->timed_enabled ? dx : dx/(slot->missed+1);
        slot->dy=core->timed_enabled ? dy : dy/(slot->missed+1);
        if (slot->missed) {++core->holds_bridged;slot->missed=0;}
        (*contact).slot=s;(*contact).tracking_id=slot->tracking_id;
        const int px=slot->rx,py=slot->ry;
        slot->x=(*contact).x;slot->y=(*contact).y;if(slot->age<__INT_MAX__)++slot->age;
        const int weak=!core->timed_enabled && (*contact).peak<NVT_DOWN_THRESHOLD;
        uint64_t filter_dt_us=0;
        if(core->timed_enabled) {
            filter_dt_us=core->now_us>=slot->last_filter_us ? core->now_us-slot->last_filter_us : 1;
            if(filter_dt_us<1)filter_dt_us=1;
        }
        const int filter_applied=follow(slot,(*contact).x,(*contact).y,weak,filter_dt_us);
        if(core->timed_enabled && filter_applied)slot->last_filter_us=core->now_us;
        // Timed reporting keeps origin separate; the report timeline owns the
        // historical DOWN. Never overwrite the newest measured output here.
        if (!core->timed_enabled && slot->age == 2) {
            slot->rx = px;
            slot->ry = py;
        }
        (*contact).x=slot->rx;(*contact).y=slot->ry;
        if (ax<4*NVT_FUZZ && ay<4*NVT_FUZZ) {
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
    int *delta=core->delta;
    for (int i=0;i<NVT_NODES;++i) delta[i]=delta_at(matrix,core->reference,i);
    // Slow reference drift: in a frame without a finger the quiet nodes move the reference a step towards their value (NvtTouchCore.h).
    int peak_active=0;
    for (int i=0;i<NVT_NODES && !peak_active;++i) peak_active=delta[i]>=NVT_PEAK_THRESHOLD;
    if (!peak_active) {
        for (int i=0;i<NVT_NODES;++i) {
            const int step=delta[i]/NVT_REFERENCE_RATE;
            core->reference[i]=(int16_t)(core->reference[i]+(step ? step : delta[i]>0 ? 1 : delta[i]<0 ? -1 : 0));
        }
        ++core->reference_frames;
    }

    struct candidate_workspace candidates;
    extract_candidates(core,0,NVT_ROWS,&candidates);
    struct nvt_contact *found=candidates.found;
    const int nfound=candidates.count,npalms=candidates.npalms,npeaks=candidates.npeaks;
    const int single_blob_label=candidates.single_blob_label;
    const int overflow=candidates.overflow,had_active=candidates.had_active;
    const int *spatial_weak=candidates.quality;
    const int (*palms)[2]=(const int (*)[2])candidates.palms;
    int match[NVT_SLOTS];
    // Track: the assignment of blobs (above the up threshold, within TRACK_RADIUS of each finger's predicted position) to fingers with
    // the least total squared distance; a finger may also take no blob, at the cost of the radius. Matching slot by slot let a finger
    // take its neighbour's blob (int-275: 5 strokes ended "taken"), and so does taking the closest pair first when two fingers move
    // together; measuring from the last position instead of the predicted one ended fast swipes (13 of 125 strokes ended "far").
    const int fast_start=fast_start_candidate(core,found,nfound,single_blob_label,npeaks,npalms,overflow,had_active);
    int used_contact[MAX_PEAKS]={0};
    assign_tracks(core,found,nfound,match,spatial_weak);
    for (int s=0;s<NVT_SLOTS;++s) if (match[s]>=0) used_contact[match[s]]=1;
    for (int s=0;s<NVT_SLOTS;++s) {
        struct nvt_slot *slot=&core->slots[s];
        if (!slot->active) continue;
        const int best=match[s];
        if (best<0) {
            const int reason = miss_reason(slot, found, used_contact, nfound, (const int (*)[2])palms, npalms, core->timed_enabled ? spatial_weak : NULL);
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
                if(slot->age>=2)core->retired_ids|=1u<<slot->tracking_id;
                if(reason==NVT_MISS_FAR || reason==NVT_MISS_TAKEN)++core->far_retired;
            }
            if(core->timed_enabled && !peak_active && reason==NVT_MISS_NONE)
                slot->last_covered_negative_us=core->now_us;
            ++core->ends[slot->miss_reason];slot->active=0;continue;
        }
        accept_position(core,slot,s,&found[best]);
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
                core->slots[s].last_interval_us=8333;
                core->slots[s].last_filter_us=core->now_us; // Initial measured filter seed.
            }
            core->slots[s].x=found[c].x;core->slots[s].y=found[c].y;
            core->slots[s].origin_x=found[c].x;core->slots[s].origin_y=found[c].y;
            core->slots[s].filter_raw_x=found[c].x;core->slots[s].filter_raw_y=found[c].y;
            core->slots[s].generation=++core->next_generation;
            core->slots[s].rx=found[c].x;core->slots[s].ry=found[c].y;
            core->slots[s].fx=(long)found[c].x*16;core->slots[s].fy=(long)found[c].y*16;core->slots[s].vx=core->slots[s].vy=0;
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

int nvt_touch_extract_region(struct nvt_touch_core *core,const int16_t *rows,
                             int first_row,int row_count,struct nvt_region_candidates *out) {
    if(!core || !rows || !out || first_row<0 || row_count<1 ||
       first_row>NVT_ROWS-row_count || !core->have_reference)return -1;
    // This scratch raster carries no lifetime or negative-evidence semantics.
    // Only actual covered values enter subtraction; outside is masked UNKNOWN.
    memset(core->delta,0,sizeof(core->delta));
    for(int r=0;r<row_count;++r)for(int col=0;col<NVT_COLUMNS;++col) {
        const int node=(first_row+r)*NVT_COLUMNS+col;
        core->delta[node]=(int)rows[r*NVT_COLUMNS+col]-core->reference[node];
    }
    struct candidate_workspace w;
    extract_candidates(core,first_row,row_count,&w);
    memset(out,0,sizeof(*out));
    out->count=w.count;out->clipped_count=w.clipped_count;out->overflow=w.overflow;
    out->first_row=first_row;out->row_count=row_count;
    memcpy(out->contact,w.found,(size_t)w.count*sizeof(w.found[0]));
    for(int c=0;c<w.count;++c)if(w.quality[c])out->quality_mask|=1u<<c;
    return w.count;
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
    core->timed_enabled=1;core->clock_valid=1;core->now_us=now_us;
    core->region_clock_valid=1;core->last_region_us=now_us;
    core->previous_retired_ids=core->retired_ids;core->retired_ids=0;
    assign_tracks(core,found,nfound,match,quality);
    for(int s=0;s<NVT_SLOTS;++s)if(match[s]>=0)used[match[s]]=1;
    int changed=0;
    for(int s=0;s<NVT_SLOTS;++s) {
        struct nvt_slot *slot=&core->slots[s];
        if(!slot->active || match[s]<0)continue;
        if(now_us<=slot->last_observed_us)continue;
        const uint64_t gap=now_us-slot->last_observed_us;
        if((slot->age<2 && now_us-slot->born_us>=NVT_PENDING_MAX_US) ||
           gap>=NVT_OBSERVATION_MAX_US || (slot->missed && gap>=NVT_DROPOUT_MAX_US))continue;
        accept_position(core,slot,s,&found[match[s]]);++changed;
    }
    for(int c=0;c<nfound;++c) {
        if(used[c] || found[c].peak<NVT_DOWN_THRESHOLD)continue;
        // A strip cannot prove the global single-peak condition needed by the
        // existing high-confidence fast path. Use ordinary independent samples.
        for(int s=0;s<NVT_SLOTS;++s)if(!core->slots[s].active) {
            const int id=free_tracking_id(core);if(!id){++core->id_quarantine_drops;break;}
            struct nvt_slot *slot=&core->slots[s];
            memset(slot,0,sizeof(*slot));slot->active=1;slot->tracking_id=id;slot->age=1;
            slot->x=slot->rx=slot->origin_x=slot->filter_raw_x=found[c].x;
            slot->y=slot->ry=slot->origin_y=slot->filter_raw_y=found[c].y;
            slot->fx=(long)found[c].x*16;slot->fy=(long)found[c].y*16;
            slot->born_us=slot->last_observed_us=slot->last_filter_us=slot->last_presence_us=now_us;
            slot->generation=++core->next_generation;slot->last_interval_us=8333;
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
    core->timed_enabled=1;core->clock_valid=1;core->now_us=now_us;
    struct track_projection projection[NVT_SLOTS];
    int active=0;
    for(int s=0;s<NVT_SLOTS;++s)active+=core->slots[s].active!=0;
    project_tracks(core,active<=3,projection);
    int changed=0;
    for(int j=0;j<count;++j)for(int s=0;s<NVT_SLOTS;++s) {
        struct nvt_slot *slot=&core->slots[s];
        if(!slot->active || slot->age<2 || slot->tracking_id!=measured[j].tracking_id)continue;
        if(now_us<=slot->last_observed_us)break;
        const uint64_t dt=now_us-slot->last_observed_us;
        if(dt>=NVT_OBSERVATION_MAX_US || (slot->missed && dt>=NVT_DROPOUT_MAX_US))break;
        if(track_cost(slot,&measured[j],1,0,&projection[s],NULL)>(long)TRACK_RADIUS*TRACK_RADIUS)break;
        const uint64_t filter_dt=now_us>slot->last_filter_us ? now_us-slot->last_filter_us : 1;
        const int dx=measured[j].x-slot->x,dy=measured[j].y-slot->y;
        (void)follow(slot,measured[j].x,measured[j].y,0,filter_dt);
        slot->dx=dx;slot->dy=dy;slot->x=measured[j].x;slot->y=measured[j].y;
        slot->last_interval_us=dt;slot->last_observed_us=slot->last_filter_us=now_us;
        slot->last_presence_us=now_us;
        if(slot->missed)++core->holds_bridged;
        slot->missed=0;if(slot->age<__INT_MAX__)++slot->age;
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

int nvt_touch_expire(struct nvt_touch_core *core,uint64_t now_us,struct nvt_contact *out,int max) {
    if(!core || !out || max<0)return -1;
    core->timed_enabled=1;
    const int rollback=core->clock_valid && now_us<core->now_us;
    if(rollback) {
        ++core->clock_rebases;
        core->region_clock_valid=0;core->last_region_us=0;
    }
    core->clock_valid=1;core->now_us=now_us;
    int visible_changed=0;
    for(int s=0;s<NVT_SLOTS;++s) {
        struct nvt_slot *slot=&core->slots[s];
        if(!slot->active)continue;
        const uint64_t age=now_us>=slot->born_us ? now_us-slot->born_us : 0;
        const uint64_t gap=now_us>=slot->last_observed_us ? now_us-slot->last_observed_us : 0;
        const int pending=slot->age<2;
        const int stale_pending=pending && age>=NVT_PENDING_MAX_US;
        const int stale_visible=!pending && gap>=NVT_OBSERVATION_MAX_US;
        const int stale_dropout=!pending && slot->missed && gap>=NVT_DROPOUT_MAX_US;
        if(!rollback && !stale_pending && !stale_visible && !stale_dropout)continue;
        if(pending)++core->pending_expired;
        else {
            core->retired_ids|=1u<<slot->tracking_id;visible_changed=1;
            if(stale_dropout)++core->dropout_expired;
            else ++core->observation_cancelled;
        }
        slot->active=0;
    }
    return visible_changed ? visible_set(core,out,max) : -1;
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
    core->previous_retired_ids=core->retired_ids;core->retired_ids=0;
    // Expire again for direct callers; the production adapter publishes expiry
    // before frame(). Retired IDs remain unavailable for the current next scan.
    (void)nvt_touch_expire(core,now_us,out,max);
    core->previous_matrix_us=now_us;core->matrix_clock_valid=1;
    return process_matrix(core,matrix,frame_type,out,max);
}

int nvt_touch_process(struct nvt_touch_core *core,const int16_t *matrix,unsigned frame_type,
                      struct nvt_contact *out,int max) {
    core->timed_enabled=0;
    return process_matrix(core,matrix,frame_type,out,max);
}
