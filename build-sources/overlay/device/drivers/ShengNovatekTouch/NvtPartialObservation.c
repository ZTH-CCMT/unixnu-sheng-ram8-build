// SPDX-License-Identifier: Apache-2.0
#include "NvtPartialObservation.h"
#include "NvtProtocol.h"
#include <string.h>
#include "NvtPartialMathInline.h"

static int absi(int v) {return v<0?-v:v;}
static int mini(int a,int b) {return a<b?a:b;}
static int maxi(int a,int b) {return a>b?a:b;}
static int64_t round_i64(int64_t n,int64_t d) {return n>=0?(n+d/2)/d:-((-n+d/2)/d);}
static int divide_round(int64_t n,int64_t d) {return (int)round_i64(n,d);}
static int floor_q8(int v) {return v>=0?v/256:-((-v+255)/256);}
static int axis_at_q8(int value,int rows) {
    const int count=rows?NVT_ROWS:NVT_COLUMNS;
    const int index=floor_q8(value),fraction=value-index*256;
    if(index<0)return (rows?nvt_touch_row_center(0):nvt_touch_column_center(0))*10;
    if(index>=count-1)return (rows?nvt_touch_row_center(count-1):nvt_touch_column_center(count-1))*10;
    const int a=rows?nvt_touch_row_center(index):nvt_touch_column_center(index);
    const int b=rows?nvt_touch_row_center(index+1):nvt_touch_column_center(index+1);
    return a*10+divide_round((int64_t)(b-a)*10*fraction,256);
}
static int map_q8(int v,int rows) {
    return nvt_touch_map_edge_axis(axis_at_q8(v,rows),rows?NVT_SHORT_AXIS_MAX:NVT_LONG_AXIS_MAX,rows);
}
static int inverse_axis(int coordinate,int rows) {
    int low=0,high=((rows?NVT_ROWS:NVT_COLUMNS)-1)*256;
    while(low<high){const int mid=low+(high-low)/2;if(map_q8(mid,rows)<coordinate)low=mid+1;else high=mid;}
    return low;
}
static int slot_current(const struct nvt_partial_template *t,const struct nvt_slot *s) {
    return t->valid && s->active && s->age>=2 && t->tracking_id==s->tracking_id &&
           t->generation==s->generation && t->born_us==s->born_us;
}
void nvt_partial_reset(struct nvt_partial_state *s) {if(s)memset(s,0,sizeof(*s));}
static int isolated(const struct nvt_touch_core *core,int which,int row,int col) {
    for(int i=0;i<NVT_SLOTS;++i) {
        if(i==which || !core->slots[i].active)continue;
        const int r=inverse_axis(core->slots[i].y,1),c=inverse_axis(core->slots[i].x,0);
        const int rr=(r+128)/256,cc=(c+128)/256;
        const int own_r=(row+128)/256,own_c=(col+128)/256;
        if(maxi(0,rr-5)<=mini(NVT_ROWS-1,own_r+5) &&
           maxi(0,own_r-5)<=mini(NVT_ROWS-1,rr+5) &&
           maxi(0,cc-5)<=mini(NVT_COLUMNS-1,own_c+5) &&
           maxi(0,own_c-5)<=mini(NVT_COLUMNS-1,cc+5))return 0;
    }
    return 1;
}
static int seed_model_valid(struct nvt_partial_state *,struct nvt_partial_template *,
                             const struct nvt_touch_core *,const int16_t *);
/* Optional Gaussian specialization. Generic K has an independent binding API.
 * Shape is learned from a complete M, not supplied by a test's truth values.
 * exp approximation and analytic derivative describe the same bounded model. */
static int pm_gaussian_eval(const void *ctx,int col,int row,int32_t *k,int32_t *gx,int32_t *gy) {
    const struct nvt_partial_template *t=(const struct nvt_partial_template *)ctx;
    if(!t || !k || !gx || !gy)return 0;
    const int64_t x=(int64_t)col-t->origin_col_q8,y=(int64_t)row-t->origin_row_q8;
    if(x>2048 || x< -2048 || y>2048 || y< -2048){*k=*gx=*gy=0;return 1;}
    const int64_t det=(int64_t)t->variance_x_q16*t->variance_y_q16-
                       (int64_t)t->covariance_xy_q16*t->covariance_xy_q16;
    if(det<=0)return 0;
    const pm_i128 n=(pm_i128)t->variance_y_q16*x*x-
       2*(pm_i128)t->covariance_xy_q16*x*y+(pm_i128)t->variance_x_q16*y*y;
    if(n<0)return 0;
    const pm_i128 exponent=pm_div_round(n*524288,det);
    if(exponent>=16*1048576){*k=*gx=*gy=0;return 1;}
    const int64_t base=INT64_C(1073741824)-(int64_t)exponent;
    int64_t value=base;
    for(int i=0;i<10;++i)value=(value*value+INT64_C(536870912))/INT64_C(1073741824);
    *k=(int32_t)((value+512)/1024);
    const pm_i128 nx=(pm_i128)t->variance_y_q16*x-(pm_i128)t->covariance_xy_q16*y;
    const pm_i128 ny=(pm_i128)t->variance_x_q16*y-(pm_i128)t->covariance_xy_q16*x;
    const pm_i128 dx=pm_div_round((pm_i128)*k*nx*256*1073741824,(pm_i128)det*base);
    const pm_i128 dy=pm_div_round((pm_i128)*k*ny*256*1073741824,(pm_i128)det*base);
    if(dx>INT32_MAX || dx<INT32_MIN || dy>INT32_MAX || dy<INT32_MIN)return 0;
    *gx=(int32_t)dx;*gy=(int32_t)dy;return 1;
}
int nvt_partial_bind_template(struct nvt_partial_state *s,const struct nvt_touch_core *core,
              int slot,const struct nvt_partial_template_binding *b,uint64_t now) {
    if(!s || !core || !b || !b->evaluate || !s->config_valid || slot<0 || slot>=NVT_SLOTS)return 0;
    const struct nvt_slot *contact=&core->slots[slot];
    if(!contact->active || contact->age<2 || !contact->generation || now<contact->last_observed_us ||
       b->origin_col_q8<0 || b->origin_col_q8>(NVT_COLUMNS-1)*256 ||
       b->origin_row_q8<0 || b->origin_row_q8>(NVT_ROWS-1)*256)return 0;
    struct nvt_partial_template *t=&s->finger[slot];memset(t,0,sizeof(*t));
    t->valid=1;t->tracking_id=contact->tracking_id;t->generation=contact->generation;
    t->born_us=contact->born_us;t->seed_us=now;t->origin_col_q8=b->origin_col_q8;t->origin_row_q8=b->origin_row_q8;
    t->anchor_col=(b->origin_col_q8+128)/256;t->anchor_row=(b->origin_row_q8+128)/256;
    t->evaluate=b->evaluate;t->context=b->context;return 1;
}
void nvt_partial_seed(struct nvt_partial_state *s,const struct nvt_touch_core *core,
                        const int16_t *matrix,uint64_t now) {
    if(!s || !core || !matrix || !core->have_reference || !s->config_valid ||
       !s->config.allow_gaussian_learning)return;
    for(int i=0;i<NVT_SLOTS;++i) {
        const struct nvt_slot *slot=&core->slots[i];
        struct nvt_partial_template *old=&s->finger[i];
        if(!slot_current(old,slot))old->valid=0;
        if(old->valid && old->evaluate!=pm_gaussian_eval)continue;
        if(!slot->active || slot->age<2 || slot->last_observed_us!=now)continue;
        const int row=inverse_axis(slot->y,1),col=inverse_axis(slot->x,0);
        const int ar=(row+128)/256,ac=(col+128)/256;
        if(ar<4 || ac<4 || ar+4>=NVT_ROWS || ac+4>=NVT_COLUMNS || !isolated(core,i,row,col))continue;
        int peak=0,support=0,minr=9,maxr=-1,minc=9,maxc=-1,edge=0,border[32],nb=0;
        int in_range=1;
        for(int r=-4;r<=4;++r)for(int c=-4;c<=4;++c) {
            const int node=(ar+r)*NVT_COLUMNS+ac+c,d=(int)matrix[node]-core->reference[node];
            if(d>8192 || d< -8192)in_range=0;
            if(d>peak)peak=d;
            if(d>=NVT_UP_THRESHOLD){++support;if(r<minr)minr=r;if(r>maxr)maxr=r;if(c<minc)minc=c;if(c>maxc)maxc=c;}
            if(r==-4 || r==4 || c==-4 || c==4){border[nb++]=d;if(d>edge)edge=d;}
        }
        if(!in_range || peak<NVT_DOWN_THRESHOLD || support<3 || support>=NVT_PALM_MIN_NODES ||
           minr==maxr || minc==maxc || edge*5>peak)continue;
        for(int j=1;j<nb;++j){const int v=border[j];int k=j;while(k && border[k-1]>v){border[k]=border[k-1];--k;}border[k]=v;}
        const int bg=(border[15]+border[16])/2;
        int64_t mass=0,sr=0,sc=0;
        for(int r=-4;r<=4;++r)for(int c=-4;c<=4;++c) {
            const int node=(ar+r)*NVT_COLUMNS+ac+c,d=(int)matrix[node]-core->reference[node]-bg;
            mass+=d;sr+=(int64_t)d*r*256;sc+=(int64_t)d*c*256;
        }
        if(mass<=0)continue;
        const int cr=divide_round(sr,mass),cc=divide_round(sc,mass);
        if(absi(cr)>256 || absi(cc)>256)continue;
        int64_t vr=0,vc=0,cov=0;
        for(int r=-4;r<=4;++r)for(int c=-4;c<=4;++c) {
            const int node=(ar+r)*NVT_COLUMNS+ac+c,d=(int)matrix[node]-core->reference[node]-bg;
            const int rr=r*256-cr,co=c*256-cc;
            vr+=(int64_t)d*rr*rr;vc+=(int64_t)d*co*co;cov+=(int64_t)d*rr*co;
        }
        const pm_i128 wide_r=pm_div_round(vr,mass),wide_c=pm_div_round(vc,mass),wide_xy=pm_div_round(cov,mass);
        if(wide_r<16384 || wide_r>262144 || wide_c<16384 || wide_c>262144 ||
           wide_xy< -262144 || wide_xy>262144)continue;
        const int rvar=(int)wide_r,cvar=(int)wide_c,cross=(int)wide_xy;
        if(rvar<16384 || cvar<16384 || rvar>262144 || cvar>262144 ||
           (int64_t)rvar*cvar-(int64_t)cross*cross<(int64_t)(rvar+cvar)*4096)continue;
        struct nvt_partial_template candidate;memset(&candidate,0,sizeof(candidate));
        candidate.valid=1;candidate.tracking_id=slot->tracking_id;candidate.generation=slot->generation;
        candidate.born_us=slot->born_us;candidate.seed_us=now;candidate.origin_row_q8=ar*256+cr;candidate.origin_col_q8=ac*256+cc;
        candidate.anchor_row=ar;candidate.anchor_col=ac;candidate.seed_peak=peak;candidate.support=support;
        candidate.variance_x_q16=cvar;candidate.variance_y_q16=rvar;candidate.covariance_xy_q16=cross;
        candidate.evaluate=pm_gaussian_eval;candidate.context=&candidate;
        for(int r=-4;r<=4;++r)for(int c=-4;c<=4;++c) {
            const int node=(ar+r)*NVT_COLUMNS+ac+c,d=maxi(0,mini((int)matrix[node]-core->reference[node],peak));
            candidate.values[(r+4)*9+c+4]=(int16_t)divide_round((int64_t)d*1024,peak);
        }
        if(!seed_model_valid(s,&candidate,core,matrix))continue;
        *old=candidate;old->context=old;++s->seeded;
    }
}


/* rms_q8 is the retained diagnostic name for MSE*256, never RMS. */
struct fit_result {
    int dx,dy,valid,nuisance_converged,pose_converged,peak,rms_q8,amp,bg,information,one_axis_valid;
    unsigned evaluations;
    pm_u128 cost,sse;
    struct pm_normal normal;
    struct nvt_partial_math_result math_result;
};
struct pm_fit_budget {unsigned used,limit;};
static int clamp_parameter(pm_i128 v,int low,int high) {
    if(v<low)return low;if(v>high)return high;return (int)v;
}
static int model_at(const struct nvt_partial_template *t,const struct nvt_partial_sample *p,
                    int dx,int dy,int32_t *k,int32_t *gx,int32_t *gy) {
    if(!t->evaluate || !t->evaluate(t->context,p->col*256-dx,p->row*256-dy,k,gx,gy))return 0;
    return *k>=0 && *k<=1048576;
}
static int pred_q16(int amp,int bg,int k) {
    return (int)round_i64((int64_t)amp*k,1048576)+bg;
}
static struct pm_u256 quad_add(struct pm_u256 a,struct pm_u256 b) {
    const pm_u128 before=a.lo;a.lo+=b.lo;a.hi+=b.hi+(a.lo<before);return a;
}
static struct pm_u256 quad_sub(struct pm_u256 a,struct pm_u256 b) {
    const pm_u128 before=a.lo;a.lo-=b.lo;a.hi-=b.hi+(before<b.lo);return a;
}
static struct pm_u256 quad_shift(struct pm_u256 a,unsigned shift) {
    if(!shift)return a;
    a.hi=(a.hi<<shift)|(a.lo>>(128-shift));a.lo<<=shift;return a;
}
static void quad_accumulate(struct pm_u256 *sum,pm_i128 a,pm_i128 b,unsigned shift,int negative) {
    const struct pm_u256 term=quad_shift(pm_mul128(pm_abs128(a),pm_abs128(b)),shift);
    if(((a<0)!=(b<0))!=negative)*sum=quad_sub(*sum,term);else *sum=quad_add(*sum,term);
}
/* Exact weighted quadratic from cached moments, Q94 (WQ22,residualQ36). No residual-vector
 * evaluations are hidden inside the five rectangle candidates. */
static struct pm_u256 nuisance_quadratic(int a,int b,pm_i128 sw,pm_i128 sk,
             pm_i128 sd,pm_i128 skk,pm_i128 skd,pm_i128 sdd) {
    struct pm_u256 value={0,0};
    quad_accumulate(&value,a,(pm_i128)a*skk,0,0);
    quad_accumulate(&value,b,(pm_i128)b*sw,40,0);
    quad_accumulate(&value,a,(pm_i128)b*sk,21,0);
    quad_accumulate(&value,a,skd,37,1);
    quad_accumulate(&value,b,sd,57,1);
    quad_accumulate(&value,sdd,1,72,0);
    return value;
}
static pm_i128 div_floor(pm_i128 n,pm_i128 d) {
    pm_i128 q=pm_div_round(n,d);if(q*d>n)--q;return q;
}
static pm_i128 div_ceil(pm_i128 n,pm_i128 d) {
    pm_i128 q=pm_div_round(n,d);if(q*d<n)++q;return q;
}
static int fill_math(struct nvt_partial_state *s,int count,const struct fit_result *f) {
    for(int j=0;j<count;++j) {
        struct nvt_partial_math_sample *p=&s->math[j];
        const int64_t x=round_i64((int64_t)f->amp*s->model_x[j],1048576);
        const int64_t y=round_i64((int64_t)f->amp*s->model_y[j],1048576);
        const int64_t e=s->math[j].residual_q16; /* cached exact same parameters */
        /* Range checks precede every narrowing. This is a representational
         * boundary, not an alternative confidence/physical detector threshold. */
        if(x>1073741824 || x< -1073741824 || y>1073741824 || y< -1073741824 ||
           e>INT32_MAX || e<INT32_MIN)return 0;
        p->k_q20=s->model[j];p->jx_q16=(int32_t)x;p->jy_q16=(int32_t)y;p->residual_q16=(int32_t)e;
    }
    return 1;
}
/* The convex weighted amplitude/background subproblem is solved over its
 * rectangle: interior and each of four edges (corners included by clamping). */
static int solve_nuisance(struct nvt_partial_state *s,int count,int *amp,int *bg) {
    const struct nvt_partial_config *c=&s->config;
    pm_i128 sw=0,sk=0,sd=0,skk=0,skd=0,sdd=0;
    for(int j=0;j<count;++j) {
        const pm_i128 w=s->sample[j].weight,k=s->model[j],d=s->sample[j].delta;
        sw+=w;sk+=w*k;sd+=w*d;skk+=w*k*k;skd+=w*k*d;sdd+=w*d*d;
    }
    const pm_i128 den=sw*skk-sk*sk,num=sw*skd-sk*sd;
    if(sw<=0 || den<0)return 0;
    if(den==0) {
        /* Rank-one nuisance: only the combined intercept is identified. Choose
         * an interior feasible (a,b) on that line, not a false math failure. */
        const pm_i128 target=pm_div_round(sd*65536,sw);
        const pm_i128 k=pm_div_round(sk,sw);
        if(k==0) {
            *amp=c->amplitude_min_q16+(c->amplitude_max_q16-c->amplitude_min_q16)/2;
            *bg=clamp_parameter(target,c->background_min_q16,c->background_max_q16);return 1;
        }
        pm_i128 lo=div_ceil((target-c->background_max_q16)*1048576,k);
        pm_i128 hi=div_floor((target-c->background_min_q16)*1048576,k);
        if(lo<c->amplitude_min_q16)lo=c->amplitude_min_q16;
        if(hi>c->amplitude_max_q16)hi=c->amplitude_max_q16;
        if(lo>hi) {
            const pm_i128 low=pm_div_round((pm_i128)c->amplitude_min_q16*k,1048576)+c->background_min_q16;
            const pm_i128 high=pm_div_round((pm_i128)c->amplitude_max_q16*k,1048576)+c->background_max_q16;
            if(pm_abs128(target-low)<=pm_abs128(target-high)) {
                *amp=c->amplitude_min_q16;*bg=c->background_min_q16;
            } else {*amp=c->amplitude_max_q16;*bg=c->background_max_q16;}
            return 1;
        } else *amp=(int)((lo+hi)/2);
        *bg=clamp_parameter(target-pm_div_round((pm_i128)*amp*k,1048576),
                           c->background_min_q16,c->background_max_q16);return 1;
    }
    if(skk<=0)return 0;
    int a[5],b[5];
    a[0]=clamp_parameter(pm_div_round(num*((pm_i128)1<<36),den),c->amplitude_min_q16,c->amplitude_max_q16);
    b[0]=clamp_parameter(pm_div_round(sd*((pm_i128)1<<36)-(pm_i128)a[0]*sk,sw*1048576),
                          c->background_min_q16,c->background_max_q16);
    a[1]=c->amplitude_min_q16;a[2]=c->amplitude_max_q16;
    for(int j=1;j<=2;++j)b[j]=clamp_parameter(pm_div_round(sd*((pm_i128)1<<36)-(pm_i128)a[j]*sk,sw*1048576),
                                               c->background_min_q16,c->background_max_q16);
    b[3]=c->background_min_q16;b[4]=c->background_max_q16;
    for(int j=3;j<=4;++j)a[j]=clamp_parameter(pm_div_round((skd*65536-(pm_i128)b[j]*sk)*1048576,skk),
                                               c->amplitude_min_q16,c->amplitude_max_q16);
    struct pm_u256 best={(pm_u128)-1,(pm_u128)-1};int choice=0;
    for(int j=0;j<5;++j) {
        const struct pm_u256 v=nuisance_quadratic(a[j],b[j],sw,sk,sd,skk,skd,sdd);
        if(pm_compare256(v,best)<0){best=v;choice=j;}
    }
    *amp=a[choice];*bg=b[choice];return 1;
}
/* One inexact nuisance block update is allowed only as an intermediate
 * joint IRLS/GN iterate. terminal=1 solves the nuisance block to the unchanged
 * Q16 convergence rule. A provisional valid point cannot by itself be accepted. */
static struct fit_result fit_at(struct nvt_partial_state *s,
          const struct nvt_partial_template *t,int count,int dx,int dy,struct pm_fit_budget *budget,
          const struct fit_result *initial,int terminal) {
    struct fit_result f;memset(&f,0,sizeof(f));f.dx=dx;f.dy=dy;f.cost=(pm_u128)-1;
    if(!s->config_valid || count<1 || !budget || budget->used>=budget->limit)return f;
    ++budget->used;
    int peak=0;
    for(int j=0;j<count;++j) {
        if(!model_at(t,&s->sample[j],dx,dy,&s->model[j],&s->model_x[j],&s->model_y[j]))return f;
        if(s->sample[j].delta>peak)peak=s->sample[j].delta;
    }
    int amp=initial?initial->amp:clamp_parameter((pm_i128)peak*65536,
                             s->config.amplitude_min_q16,s->config.amplitude_max_q16);
    int bg=initial?initial->bg:clamp_parameter(0,s->config.background_min_q16,s->config.background_max_q16);
    for(int j=0;j<count;++j) {
        const int64_t e=(int64_t)pred_q16(amp,bg,s->model[j])-(int64_t)s->sample[j].delta*65536;
        if(e>INT32_MAX || e<INT32_MIN)return f;
        s->math[j].residual_q16=(int32_t)e;s->sample[j].weight=pm_weight((int32_t)e,&s->config);
    }
    const unsigned iterations=terminal?64:1;
    for(unsigned pass=0;pass<iterations;++pass) {
        int a=amp,b=bg;if(!solve_nuisance(s,count,&a,&b))return f;
        const int settled=(int64_t)a-amp<=1 && (int64_t)amp-a<=1 &&
                          (int64_t)b-bg<=1 && (int64_t)bg-b<=1;
        const int changed=a!=amp || b!=bg;
        if(changed) {
            if(budget->used>=budget->limit)return f;
            ++budget->used;
        }
        amp=a;bg=b;
        for(int j=0;j<count && changed;++j) {
            const int64_t e=(int64_t)pred_q16(amp,bg,s->model[j])-(int64_t)s->sample[j].delta*65536;
            if(e>INT32_MAX || e<INT32_MIN)return f;
            s->math[j].residual_q16=(int32_t)e;s->sample[j].weight=pm_weight((int32_t)e,&s->config);
        }
        if(settled){f.nuisance_converged=1;break;}
    }
    if(!f.nuisance_converged) {
        int a=amp,b=bg;
        if(!solve_nuisance(s,count,&a,&b))return f;
        f.nuisance_converged=(int64_t)a-amp<=1 && (int64_t)amp-a<=1 &&
                             (int64_t)b-bg<=1 && (int64_t)bg-b<=1;
    }
    if(terminal && !f.nuisance_converged)return f;
    f.amp=amp;f.bg=bg;f.peak=maxi(0,peak-divide_round(bg,65536));
    if(!fill_math(s,count,&f))return f;
    struct pm_normal n;if(!pm_normal_build(s->math,count,&s->config,&n))return f;
    f.cost=n.cost2;f.sse=n.sse;f.normal=n;
    if(!nvt_partial_math_evaluate(s->math,count,&s->config,&f.math_result))return f;
    const pm_u128 diagnostic=pm_div128(f.sse,(unsigned)count)>>24;
    f.rms_q8=diagnostic>INT32_MAX?INT32_MAX:(int)diagnostic;f.valid=1;return f;
}
static int inside(const struct nvt_partial_config *c,int x,int y,int ox,int oy) {
    const int64_t dx=(int64_t)x-ox,dy=(int64_t)y-oy;
    return (dx<0?-dx:dx)<=c->search_radius_q8 && (dy<0?-dy:dy)<=c->search_radius_q8;
}
static int position_step(const struct pm_normal *n,int *dx,int *dy) {
    pm_i128 xx=n->xx,xy=n->xy,yy=n->yy,gx=n->gx,gy=n->gy;
    const pm_u128 limit=(pm_u128)1<<52;
    while(pm_abs128(xx)>limit || pm_abs128(xy)>limit || pm_abs128(yy)>limit ||
          pm_abs128(gx)>limit || pm_abs128(gy)>limit) {xx>>=1;xy>>=1;yy>>=1;gx>>=1;gy>>=1;}
    const pm_i128 det=xx*yy-xy*xy;
    if(xx<=0 || yy<=0 || det<=0)return 0;
    const pm_i128 x=pm_div_round((xy*gy-yy*gx)*256,det),y=pm_div_round((xy*gx-xx*gy)*256,det);
    if(x>INT32_MAX || x<INT32_MIN || y>INT32_MAX || y<INT32_MIN)return 0;
    *dx=(int)x;*dy=(int)y;return 1;
}


/* Same masked Huber objective, interleaved IRLS/Gauss-Newton. Intermediate
 * nuisance blocks are not forced to converge at a known-wrong pose. Every
 * trial compares its full (p,a,b) Huber objective, never a surrogate for final
 * acceptance. Final nuisance fixed-point + neighbour proof remain mandatory.
 * Q8 position resolution is explicit. Success requires all feasible1LSB
 * neighbours to be non-decreasing after nuisance convergence. Exhausting the
 * profile evaluation budget is UNKNOWN, not a successful best-grid estimate.
 * This is finite-resolution local stationarity, not a proof that every
 * continuous/nonconvex K has the same minimizer as SciPy. */
static struct fit_result solve_pose(struct nvt_partial_state *s,
           const struct nvt_partial_template *t,int count,int x0,int y0) {
    struct pm_fit_budget budget={0,s->config.max_evaluations};
    struct fit_result best=fit_at(s,t,count,x0,y0,&budget,0,0);
    if(!best.valid){best.evaluations=budget.used;return best;}
    while(budget.used<budget.limit) {
        int sx=0,sy=0,moved=0;
        const int step_ok=position_step(&best.normal,&sx,&sy);
        while(step_ok && (sx || sy) && budget.used<budget.limit) {
            const int64_t x=(int64_t)best.dx+sx,y=(int64_t)best.dy+sy;
            if(x>=INT32_MIN && x<=INT32_MAX && y>=INT32_MIN && y<=INT32_MAX &&
               inside(&s->config,(int)x,(int)y,x0,y0)) {
                struct fit_result trial=fit_at(s,t,count,(int)x,(int)y,&budget,&best,0);
                if(trial.valid && trial.cost<best.cost){best=trial;moved=1;break;}
            }
            sx/=2;sy/=2;
        }
        if(moved)continue;
        /* Schur normal/score project the current nuisance gradient as well as
         * curvature, so a GN step does not require an already-solved Huber
         * nuisance block. If pose has stalled, advance that block once. */
        if(!best.nuisance_converged) {
            if(budget.used>=budget.limit)break;
            struct fit_result progress=fit_at(s,t,count,best.dx,best.dy,&budget,&best,0);
            if(progress.valid && progress.cost<=best.cost &&
               (progress.amp!=best.amp || progress.bg!=best.bg || progress.nuisance_converged)) {
                best=progress;continue;
            }
            break;
        }
        struct fit_result neighbour=best;unsigned checked=0,unresolved=0;
        for(int y=-1;y<=1;++y)for(int x=-1;x<=1;++x) {
            if(!x && !y)continue;
            if(!inside(&s->config,best.dx+x,best.dy+y,x0,y0)){++checked;continue;}
            if(budget.used>=budget.limit){unresolved=1;continue;}
            struct fit_result trial=fit_at(s,t,count,best.dx+x,best.dy+y,&budget,&best,1);++checked;
            if(!trial.valid || !trial.nuisance_converged)unresolved=1;
            else if(trial.cost<neighbour.cost)neighbour=trial;
        }
        if(neighbour.dx!=best.dx || neighbour.dy!=best.dy){best=neighbour;continue;}
        if(checked==8 && !unresolved){best.pose_converged=1;break;}
        break;
    }
    best.evaluations=budget.used;return best;
}
static int information_ok(struct nvt_partial_state *s,int count,struct fit_result *f) {
    (void)s;(void)count;
    const struct nvt_partial_math_result r=f->math_result;
    f->one_axis_valid=r.one_axis;
    f->information=r.lambda_min_q16>INT32_MAX?INT32_MAX:(int)r.lambda_min_q16;
    return r.admitted && r.numerical_status==0;
}
static int seed_model_valid(struct nvt_partial_state *s,struct nvt_partial_template *t,
                     const struct nvt_touch_core *core,const int16_t *matrix) {
    int count=0;
    for(int r=-4;r<=4;++r)for(int c=-4;c<=4;++c) {
        const int row=t->anchor_row+r,col=t->anchor_col+c,node=row*NVT_COLUMNS+col;
        const int delta=(int)matrix[node]-core->reference[node];
        if(delta>8192 || delta< -8192)return 0;
        struct nvt_partial_sample *p=&s->sample[count++];
        p->row=(int16_t)row;p->col=(int16_t)col;p->delta=(int16_t)delta;p->weight=4194304;
    }
    struct pm_fit_budget budget={0,s->config.max_evaluations};
    struct fit_result f=fit_at(s,t,count,0,0,&budget,0,1);t->seed_mse_q8=f.rms_q8;
    /* Optional learner validity uses the same explicit profile, no peak-derived
     * RMS floor. A rejected candidate never mutates the prior good template. */
    return f.valid && f.nuisance_converged &&
           f.amp>s->config.amplitude_min_q16 && f.amp<s->config.amplitude_max_q16 &&
           f.bg>s->config.background_min_q16 && f.bg<s->config.background_max_q16 &&
           f.sse<(pm_u128)(unsigned)count*s->config.rms_limit_q16*s->config.rms_limit_q16 &&
           information_ok(s,count,&f);
}
int nvt_partial_measure(struct nvt_partial_state *s,const struct nvt_touch_core *core,
         const uint8_t *raw,size_t length,uint64_t now,struct nvt_contact *out,int max) {
    if(!s || !core || !raw || !out || max<0)return -1;
    s->presence_ids=0;s->one_axis_ids=0;
    if(!s->config_valid){s->last_reason=NVT_PM_NOT_CONFIGURED;++s->rejected[NVT_PM_NOT_CONFIGURED];return -1;}
    s->last_reason=NVT_PM_BAD_PACKET;
    if(length!=NVT_THP_FRAME_LEN || !nvt_frame_header_valid(raw) || !nvt_frame_is_stylus(raw) ||
       (nvt_frame_type(raw)!=2 && nvt_frame_type(raw)!=4) || raw[332]<1 || raw[332]>4) {
        ++s->rejected[NVT_PM_BAD_PACKET];return -1;
    }
    if(s->packet_clock_valid && now<=s->last_packet_us) {
        s->last_reason=NVT_PM_STALE_PACKET;++s->rejected[NVT_PM_STALE_PACKET];return -1;
    }
    s->packet_clock_valid=1;s->last_packet_us=now;s->last_block=raw[332];
    if(!core->have_reference)return 0;
    const int row0=(raw[332]-1)*10;int found=0;
    for(int i=0;i<NVT_SLOTS && found<max;++i) {
        const struct nvt_slot *slot=&core->slots[i];
        struct nvt_partial_template *t=&s->finger[i];
        if(!slot_current(t,slot)){t->valid=0;++s->rejected[NVT_PM_NO_TEMPLATE];continue;}
        /* Complete-region extraction runs first. Its same-packet measurement
         * must never be refitted/reconfirmed or counted again by this path. */
        if(slot->last_observed_us>=now)continue;
        const int row=inverse_axis(slot->y,1),col=inverse_axis(slot->x,0);
        if(!isolated(core,i,row,col)){++s->rejected[NVT_PM_AMBIGUOUS];continue;}
        const int cr=(row+128)/256,cc=(col+128)/256;
        int count=0,peak=0,above=0,in_range=1;
        for(int r=maxi(row0,cr-5);r<=mini(row0+9,cr+5);++r)
            for(int c=maxi(0,cc-5);c<=mini(NVT_COLUMNS-1,cc+5);++c) {
                const unsigned off=4217u+2u*(unsigned)((r-row0)*NVT_COLUMNS+c);
                const unsigned wire=(unsigned)raw[off]|((unsigned)raw[off+1]<<8);
                const int value=(wire&0x8000u)?(int)wire-65536:(int)wire;
                const int delta=(int)value-core->reference[r*NVT_COLUMNS+c];
                if(delta>8192 || delta< -8192){in_range=0;continue;}
                struct nvt_partial_sample *p=&s->sample[count++];
                p->row=(int16_t)r;p->col=(int16_t)c;p->delta=(int16_t)delta;p->weight=4194304;
                if(delta>peak)peak=delta;
                if(delta>=NVT_NOISE_THRESHOLD)++above; /* diagnostic area, not K-fit admission */
            }
        if(!in_range || count<8 || peak<NVT_UP_THRESHOLD) {
            s->last_reason=NVT_PM_NO_SIGNAL;++s->rejected[NVT_PM_NO_SIGNAL];continue;
        }
        const int x0=col-t->origin_col_q8,y0=row-t->origin_row_q8;
        struct fit_result best=solve_pose(s,t,count,x0,y0);int reason;
        if(!best.valid || !best.nuisance_converged || !best.pose_converged)reason=NVT_PM_BAD_FIT;
        else if(absi(best.dx-x0)>=(int)s->config.search_radius_q8 ||
                absi(best.dy-y0)>=(int)s->config.search_radius_q8 ||
                best.amp<=s->config.amplitude_min_q16 || best.amp>=s->config.amplitude_max_q16 ||
                best.bg<=s->config.background_min_q16 || best.bg>=s->config.background_max_q16)
            reason=NVT_PM_SEARCH_BOUND;
        else if(best.peak<NVT_UP_THRESHOLD)reason=NVT_PM_NO_SIGNAL;
        else if(best.sse>=(pm_u128)(unsigned)count*s->config.rms_limit_q16*s->config.rms_limit_q16)
            reason=NVT_PM_BAD_FIT;
        else if(!information_ok(s,count,&best)) {
            s->presence_ids|=1u<<slot->tracking_id;
            if(best.one_axis_valid)s->one_axis_ids|=1u<<slot->tracking_id;
            reason=NVT_PM_NO_INFORMATION;
        } else reason=NVT_PM_ACCEPTED;
        s->last_reason=reason;s->last_rms_q8=best.rms_q8;s->last_information=best.information;
        if(reason!=NVT_PM_ACCEPTED){++s->rejected[reason];continue;}
        struct nvt_contact *contact=&out[found++];
        contact->slot=i;contact->tracking_id=slot->tracking_id;
        contact->x=map_q8(t->origin_col_q8+best.dx,0);contact->y=map_q8(t->origin_row_q8+best.dy,1);
        contact->peak=best.peak;contact->area=above;contact->weight=peak*above;++s->accepted;
    }
    return found;
}

