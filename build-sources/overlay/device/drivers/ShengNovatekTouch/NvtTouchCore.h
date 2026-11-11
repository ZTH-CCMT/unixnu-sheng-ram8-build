// SPDX-License-Identifier: Apache-2.0
// Finger-touch processing for the NT36532E THP matrix: baseline, delta, peaks, centroid and slot tracking.
// Constants and axis mapping follow xiaomi-sheng-thp (Apache-2.0); see NOTICE. Plain C, no allocation.
#ifndef NVT_TOUCH_CORE_H
#define NVT_TOUCH_CORE_H
#include <stdint.h>
#include <stddef.h>

#define NVT_ROWS 40
#define NVT_COLUMNS 60
#define NVT_NODES (NVT_ROWS*NVT_COLUMNS)
#define NVT_SUPER_RESOLUTION 10
// Steps below 4 * NVT_FUZZ count as the positional noise of a resting or slow finger (jitter_*), in coordinate units (0.1 panel px).
#define NVT_FUZZ 60
// Reported positions are smoothed with an adaptive low-pass filter: its cutoff is NVT_MIN_CUTOFF_MHZ for a resting finger and rises by
// NVT_CUTOFF_PER_SPEED mHz for each coordinate unit per second of the finger's smoothed speed, so a still finger keeps its position
// through the centroid noise and a moving one is followed with a frame or two of lag. The speed itself is smoothed at NVT_SPEED_CUTOFF_MHZ.
// int-271: resting fingers wandered 5 px per frame (24 px at most). int-273: a lock that held a slow finger until it was 20 px away
// reported every slow movement as rests and 20 px jumps (330 unlocks for 171 strokes), and swipes that paused rarely opened the switcher.
#define NVT_FRAME_HZ 120
#define NVT_MIN_CUTOFF_MHZ 1000
#define NVT_CUTOFF_PER_SPEED 3   // mHz per (unit/s), halved: 1.5
#define NVT_SPEED_CUTOFF_MHZ 1000
// The centroid weighs a node by how far it is above this share of its blob's peak (percent), so the fringe nodes that come and go with
// noise do not pull the position around.
#define NVT_CENTROID_FLOOR 30
// Merge nearby maxima of a finger, preserving separate peaks assigned to established fingers.
#define NVT_MERGE_SPAN 2
// Grace for partial dropouts (8.3 ms per scan at 120 Hz). A quiet scan releases immediately.
#define NVT_HOLD_FRAMES 3
#define NVT_SLOTS 12
#define NVT_STARTUP_FRAMES 72
// Output axes (short axis follows rows, long axis follows columns), before any display orientation.
#define NVT_LONG_AXIS_MAX 30480
#define NVT_SHORT_AXIS_MAX 20320

#define NVT_NOISE_THRESHOLD 100
#define NVT_PEAK_THRESHOLD 220
#define NVT_DOWN_THRESHOLD 300
#define NVT_UP_THRESHOLD 220
#define NVT_COORDINATE_REDUCE 25
#define NVT_PALM_MIN_NODES 60
// The reference follows the slow drift of the panel: in a frame without a node at the peak threshold every node moves
// 1/NVT_REFERENCE_RATE of the way to its value (nodes near a finger are left alone: absorbing its fringe would shift its centroid).
// A reference taken once at startup drifted far enough in ten minutes for maxima above the noise floor to appear all over the panel
// (int-274: 51765 merges).
#define NVT_REFERENCE_RATE 32

struct nvt_contact {
    int slot, tracking_id;
    int x, y;        // x along the long axis, y along the short axis, 0 .. max-1
    int peak, area, weight;
};

#define NVT_REGION_MAX_CANDIDATES 24
struct nvt_region_candidates {
    struct nvt_contact contact[NVT_REGION_MAX_CANDIDATES];
    unsigned quality_mask;
    int count,clipped_count,overflow,first_row,row_count;
};

struct nvt_slot {
    int active, tracking_id, age, x, y;
    int rx, ry;      // the position last reported, after the filter (x, y stay raw: tracking uses them)
    long fx, fy;     // the filtered position, in 1/16 coordinate units
    long vx, vy;     // the smoothed speed, coordinate units per second
    int dx, dy;      // the last raw step per frame: tracking looks for the blob where the finger is heading
    int missed;      // consecutive frames without a matching blob (held while <= NVT_HOLD_FRAMES)
    int miss_reason; // why the first of those frames had no match (NVT_MISS_*)
    // Timed API: only accepted M or separately qualified PM positions refresh observation time.
    uint64_t born_us, last_observed_us, last_interval_us;
    uint64_t last_filter_us; // Last actual accepted position/filter-state update.
    int filter_raw_x, filter_raw_y; // Previous accepted raw position, never origin/q.
    int origin_x, origin_y;         // Actual first sample for report confirmation.
    uint64_t generation;            // Changes when a slot is allocated to a new contact.
    uint64_t last_presence_us;       // Separate evidence; never grants position renewal.
    uint64_t last_covered_negative_us; // Explicit qualified negative-decision time.
};

// Why a tracked finger found no blob: none at all, the nearest beyond the tracking radius, below the up threshold, palm-sized, or already
// taken by another finger.
enum {NVT_MISS_NONE, NVT_MISS_FAR, NVT_MISS_WEAK, NVT_MISS_PALM, NVT_MISS_TAKEN, NVT_MISS_KINDS};

struct nvt_touch_core {
    int16_t reference[NVT_NODES];
    int have_reference;
    unsigned startup_count;
    int16_t startup[NVT_STARTUP_FRAMES][NVT_NODES];   // collected until the median reference is formed
    // Raw frame-to-frame movement of tracked contacts that moved less than 4 * NVT_FUZZ (a resting or slow finger): frames, summed
    // and largest step per axis, in coordinate units: the noise the filter has to cover. Smoothed: the reported steps of those frames,
    // summed and largest: what is left of that noise.
    unsigned long jitter_frames, jitter_sum;
    int jitter_max;
    unsigned long smoothed_sum;
    int smoothed_max;
    // Maxima folded into a stronger one within NVT_MERGE_SPAN nodes (one finger seen as two peaks).
    unsigned long merges;
    // Most maxima in one frame, frames with more maxima than the table holds (the weakest are dropped), and frames whose quiet nodes
    // moved the reference.
    unsigned long peaks_max, peak_overflows, reference_frames;
    // Fingers that went unseen and came back within NVT_HOLD_FRAMES, fingers lifted per miss reason, and frames by type (2 / 4).
    unsigned long holds_bridged, ends[NVT_MISS_KINDS], frames_by_type[2];
    struct nvt_slot slots[NVT_SLOTS];
    // Experimental aggressive policy, separate from legacy scan-count behavior.
    uint64_t now_us, previous_matrix_us;
    unsigned timed_enabled, clock_valid, matrix_clock_valid;
    unsigned retired_ids, previous_retired_ids;
    uint64_t next_generation;
    uint64_t last_region_us;
    unsigned region_clock_valid;
    unsigned long pending_expired, observation_cancelled, dropout_expired;
    unsigned long far_retired, clock_rebases, id_quarantine_drops;
    // Scratch for nvt_touch_process: 19 KB, kept here because kernel threads have 16 KiB stacks (a stack probe in the
    // function prologue faulted on hardware when these were locals).
    int delta[NVT_NODES];
    int label[NVT_NODES];
};

void nvt_touch_reset(struct nvt_touch_core *core);
// Feed one touch matrix (row-major, 40x60 int16). Returns the number of visible contacts written to out[0..max)
// (-1 while the startup baseline is still being collected). frame_type 2 = baseline frame.
int nvt_touch_process(struct nvt_touch_core *core, const int16_t *matrix, unsigned frame_type,
                      struct nvt_contact *out, int max);
// Real-time cancellation budgets, NOT sensor proof of a physical lift.
// Only qualified M/PM finger positions can renew the position deadline.
#define NVT_PENDING_MAX_US UINT64_C(25000)
#define NVT_DROPOUT_MAX_US UINT64_C(25000)
#define NVT_OBSERVATION_MAX_US UINT64_C(100000)
// Like the legacy API, but timestamps are monotonic microseconds. Caller should
// publish an expire() cancellation set before processing a new observation.
int nvt_touch_process_timed(struct nvt_touch_core *core, const int16_t *matrix,
                           unsigned frame_type, uint64_t now_us,
                           struct nvt_contact *out, int max);
// Extract covered rows using the SAME segmentation/merging/centroid as full M.
// Values are contiguous row_count*60 real signed16 samples. Clipped components
// and those without the required two-node background/merge ring are excluded.
// Does not update reference, contact state or negative-evidence/lifetime clocks.
int nvt_touch_extract_region(struct nvt_touch_core *core,const int16_t *rows,
                             int first_row,int row_count,struct nvt_region_candidates *out);
// Publish expire() first. Only fresh independent regional observations may be
// consumed; unmatched IDs are UNKNOWN. Full local blobs may confirm ordinary
// two-observation DOWN; the global-single-peak fast path is not asserted by a strip.
int nvt_touch_process_region(struct nvt_touch_core *core,
                             const struct nvt_region_candidates *observation,
                             uint64_t now_us,struct nvt_contact *out,int max);
// Apply already-qualified regional PM positions to existing visible tracking IDs.
// Unobserved IDs are UNKNOWN: no implicit misses, UP, new DOWN or baseline update.
// Caller validates frame freshness/epoch and measurement quality before this API;
// PM fit ownership must include the slot generation, not only its reusable ID.
// Returns complete visible set after an accepted update, or -1 if none accepted.
int nvt_touch_process_partial(struct nvt_touch_core *core,
                             const struct nvt_contact *measured, int count,
                             uint64_t now_us, struct nvt_contact *out, int max);
// Caller-certified presence may annotate a visible contact without renewing its
// position deadline. Generation prevents late evidence reviving a reused ID.
int nvt_touch_note_presence(struct nvt_touch_core *core, int tracking_id,
                           uint64_t generation, uint64_t now_us);
// Caller has established current coverage of the contact's motion/shape region,
// trustworthy baseline and actual negative signal. This API does NOT infer that
// condition from an empty stripe. Ends only that generation and returns full set.
int nvt_touch_covered_negative(struct nvt_touch_core *core, int tracking_id,
                              uint64_t generation, uint64_t now_us,
                              struct nvt_contact *out, int max);
// Remove expired hidden/visible slots without fake matrices or reference updates.
// -1 = no visible change; >=0 = replacement visible set requiring publication.
int nvt_touch_expire(struct nvt_touch_core *core, uint64_t now_us,
                     struct nvt_contact *out, int max);
// 0 = no pending/visible deadline, otherwise microseconds until next deadline
// (1 when overdue). Includes hidden age1, so complete silence can expire it.
uint64_t nvt_touch_wait_us(const struct nvt_touch_core *core, uint64_t now_us);
// Edge compensation applied to a raw centroid coordinate.
int nvt_touch_map_edge_axis(int value, int maximum, int low_edge_bias);
// Row/column centre tables (before super resolution).
int nvt_touch_row_center(int row);
int nvt_touch_column_center(int column);
#endif
