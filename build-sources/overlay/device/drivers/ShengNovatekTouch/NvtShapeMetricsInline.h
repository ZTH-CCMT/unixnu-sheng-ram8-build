// SPDX-License-Identifier: Apache-2.0
// HEADER-ONLY INTEGRATION ADAPTER OF FROZEN INDEPENDENT SHAPE PROTOTYPE.
// Only linkage/private helper names change; algorithm and metrics types identical.
// No upstream GPL function body is copied. Same-board Apache2 and iptsd design
// references are documented in README.txt and the research source index.
// Requires ordinary-M caller-owned labels/floor; no confidence/palm/UP decision.
// SENSOR node units/Q16, NOT rotated panel/HID coordinates. exact spatial_rank
// decides non-collinearity; approximate moment roundness is not a probability.
// No heap/floating point/hardware I/O. Include this instead of the standalone.h.
#ifndef NVT_SHAPE_METRICS_INLINE_H
#define NVT_SHAPE_METRICS_INLINE_H
// SPDX-License-Identifier: Apache-2.0
// Independent host-only geometry prototype; no classification, filter or lifecycle.
#include <stdint.h>
#define NVT_SHAPE_ROWS 40
#define NVT_SHAPE_COLUMNS 60
#define NVT_SHAPE_NODES (NVT_SHAPE_ROWS*NVT_SHAPE_COLUMNS)
enum { NVT_SHAPE_EDGE_TOP=1, NVT_SHAPE_EDGE_RIGHT=2,
       NVT_SHAPE_EDGE_BOTTOM=4, NVT_SHAPE_EDGE_LEFT=8 };
struct nvt_shape_metrics {
    uint32_t labelled_nodes, support_nodes, support_rows, support_columns;
    uint32_t spatial_rank; // EXACT support affine rank0/1/2; no moment rounding.
    uint32_t row_min, row_max, column_min, column_max;
    uint32_t row_span, column_span, edge_flags;
    uint32_t peak, floor;
    uint64_t weight, weight_squared_sum;
    uint32_t effective_support_q16; // W^2/sum(w^2), nodes in Q16; NOT confidence.
    uint32_t mean_row_q16, mean_column_q16; // SENSOR node units, not panel pixels.
    uint32_t variance_row_q16, variance_column_q16; // node^2 Q16.
    int32_t covariance_q16; // node^2 Q16.
    uint64_t covariance_determinant_q32; // node^4 Q32, clamped rounding residue.
    uint32_t roundness_q16; // 4*det/(trace^2), 0..65535; NOT probability.
    uint32_t background_nodes, background_abs_max, background_abs_mean_q16;
    int64_t background_mean_q16, peak_minus_background_abs_q16;
};
// delta must be exact signed int16-matrix difference, hence [-65535,65535].
// labels >=0 are blobs; labels <0 are background. Input arrays have2400 entries.
// weight=max(delta-floor,0) for selected label. floor0..65535 is caller-owned.
// Returns1 for positive support,0 for no support,-1 invalid input; out zero on failure.
// No thresholds for finger trust/palm/UP are selected here. No M/P decoding here.

// SPDX-License-Identifier: Apache-2.0
#include <stddef.h>
#include <string.h>
// Computes floor((n/d)*65536) for0<=n<d without n<<16 overflow.
// All callers have d<=~2^53, hence doubling remainder stays inside uint64_t.
static inline uint32_t nvt_shape_fraction_q16(uint64_t n,uint64_t d) {
    uint32_t result=0;
    for(unsigned i=0;i<16;++i) {
        n*=2;result*=2;
        if(n>=d){n-=d;++result;}
    }
    return result;
}
static inline uint32_t nvt_shape_bits(uint64_t value) {
    uint32_t count=0;while(value){value&=value-1;++count;}return count;
}
static inline int nvt_shape_measure(const int *delta,const int *labels,int label,int floor,
                      struct nvt_shape_metrics *out) {
    if(!out)return -1;
    memset(out,0,sizeof *out);
    if(!delta || !labels || label<0 || label>=NVT_SHAPE_NODES || floor<0 || floor>65535)return -1;
    struct nvt_shape_metrics m={0};
    m.row_min=NVT_SHAPE_ROWS;m.column_min=NVT_SHAPE_COLUMNS;m.floor=(uint32_t)floor;
    uint64_t rows=0,columns=0,sr=0,sc=0,srr=0,scc=0,src=0;
    int origin_r=0,origin_c=0,axis_r=0,axis_c=0;
    for(unsigned i=0;i<NVT_SHAPE_NODES;++i) {
        const int value=delta[i];
        if(value < -65535 || value > 65535)return -1;
        if(labels[i]!=label)continue;
        ++m.labelled_nodes;
        if(value<=floor)continue;
        const uint32_t w=(uint32_t)(value-floor),r=i/NVT_SHAPE_COLUMNS,c=i%NVT_SHAPE_COLUMNS;
                ++m.support_nodes;
        // Positive weights have2D covariance iff their support is not collinear.
        // Test rank exactly; Q16 covariance determinant can misclassify a line.
        if(m.support_nodes==1){origin_r=(int)r;origin_c=(int)c;}
        else if(m.spatial_rank==0){axis_r=(int)r-origin_r;axis_c=(int)c-origin_c;m.spatial_rank=1;}
        else if(m.spatial_rank==1 && ((int)r-origin_r)*axis_c-((int)c-origin_c)*axis_r!=0)m.spatial_rank=2;
        m.weight+=w;m.weight_squared_sum+=(uint64_t)w*w;
        sr+=(uint64_t)w*r;sc+=(uint64_t)w*c;
        srr+=(uint64_t)w*r*r;scc+=(uint64_t)w*c*c;src+=(uint64_t)w*r*c;
        rows|=UINT64_C(1)<<r;columns|=UINT64_C(1)<<c;
        if((uint32_t)value>m.peak)m.peak=(uint32_t)value;
        if(r<m.row_min)m.row_min=r;if(r>m.row_max)m.row_max=r;
        if(c<m.column_min)m.column_min=c;if(c>m.column_max)m.column_max=c;
    }
    if(!m.weight)return 0;
    m.support_rows=nvt_shape_bits(rows);m.support_columns=nvt_shape_bits(columns);
    m.row_span=m.row_max-m.row_min+1;m.column_span=m.column_max-m.column_min+1;
    if(m.row_min==0)m.edge_flags|=NVT_SHAPE_EDGE_TOP;
    if(m.column_max==NVT_SHAPE_COLUMNS-1)m.edge_flags|=NVT_SHAPE_EDGE_RIGHT;
    if(m.row_max==NVT_SHAPE_ROWS-1)m.edge_flags|=NVT_SHAPE_EDGE_BOTTOM;
    if(m.column_min==0)m.edge_flags|=NVT_SHAPE_EDGE_LEFT;
    m.mean_row_q16=(uint32_t)((sr<<16)/m.weight);
    m.mean_column_q16=(uint32_t)((sc<<16)/m.weight);
    // Avoid Srr*W-Sr^2: that form can overflow even with a2400-node valid input.
    int64_t vr=(int64_t)((srr<<16)/m.weight)-(int64_t)(((uint64_t)m.mean_row_q16*m.mean_row_q16)>>16);
    int64_t vc=(int64_t)((scc<<16)/m.weight)-(int64_t)(((uint64_t)m.mean_column_q16*m.mean_column_q16)>>16);
    const int64_t cov=(int64_t)((src<<16)/m.weight)-(int64_t)(((uint64_t)m.mean_row_q16*m.mean_column_q16)>>16);
    if(vr<0)vr=0;if(vc<0)vc=0;
    m.variance_row_q16=(uint32_t)vr;m.variance_column_q16=(uint32_t)vc;m.covariance_q16=(int32_t)cov;
    const int64_t determinant=vr*vc-cov*cov;
    m.covariance_determinant_q32=determinant>0?(uint64_t)determinant:0;
    const uint64_t trace=(uint64_t)vr+(uint64_t)vc,denominator=trace*trace;
    if(denominator) {
        const uint64_t numerator=4*m.covariance_determinant_q32;
        m.roundness_q16=numerator>=denominator?65535:nvt_shape_fraction_q16(numerator,denominator);
    }
    const uint64_t weight2=m.weight*m.weight;
    m.effective_support_q16=(uint32_t)((weight2/m.weight_squared_sum)*65536)+nvt_shape_fraction_q16(weight2%m.weight_squared_sum,m.weight_squared_sum);
    // One-node rectangular exterior ring only. Other labelled blobs are excluded.
    const unsigned top=m.row_min?m.row_min-1:0,left=m.column_min?m.column_min-1:0;
    const unsigned bottom=m.row_max+1<NVT_SHAPE_ROWS?m.row_max+1:NVT_SHAPE_ROWS-1;
    const unsigned right=m.column_max+1<NVT_SHAPE_COLUMNS?m.column_max+1:NVT_SHAPE_COLUMNS-1;
    uint64_t abs_sum=0;int64_t signed_sum=0;
    for(unsigned r=top;r<=bottom;++r)for(unsigned c=left;c<=right;++c) {
        if(r>=m.row_min && r<=m.row_max && c>=m.column_min && c<=m.column_max)continue;
        const unsigned i=r*NVT_SHAPE_COLUMNS+c;if(labels[i]>=0)continue;
        const int value=delta[i];const uint32_t absolute=(uint32_t)(value<0?-value:value);
        ++m.background_nodes;abs_sum+=absolute;signed_sum+=value;
        if(absolute>m.background_abs_max)m.background_abs_max=absolute;
    }
    if(m.background_nodes) {
        m.background_abs_mean_q16=(uint32_t)((abs_sum<<16)/m.background_nodes);
        // Signed multiplication avoids left-shifting negative values.
        m.background_mean_q16=(signed_sum*65536)/(int64_t)m.background_nodes;
        m.peak_minus_background_abs_q16=(int64_t)m.peak*65536-(int64_t)m.background_abs_mean_q16;
    }
    *out=m;return 1;
}

#endif // NVT_SHAPE_METRICS_INLINE_H
