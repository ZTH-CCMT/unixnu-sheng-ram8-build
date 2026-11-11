// SPDX-License-Identifier: Apache-2.0
#ifndef NVT_PARTIAL_MATH_INLINE_H
#define NVT_PARTIAL_MATH_INLINE_H
/* Integer-only formula evaluation. Explicit wide arithmetic avoids variable
 * wide division in this source; absence of target runtime helper imports must
 * still be checked after a future target build. No such check ran in V3.1. */
typedef __int128 pm_i128;
typedef unsigned __int128 pm_u128;
struct pm_u256 {pm_u128 hi,lo;};
static pm_u128 pm_abs128(pm_i128 v) {return v<0?(pm_u128)(-(v+1))+1:(pm_u128)v;}
static pm_u128 pm_div128(pm_u128 n,pm_u128 d) {
    pm_u128 q=0,r=0;
    if(!d)return 0;
    for(int i=127;i>=0;--i) {
        const unsigned carry=(unsigned)(r>>127);
        r=(r<<1)|((n>>i)&1);
        if(carry || r>=d){r-=d;q|=(pm_u128)1<<i;}
    }
    return q;
}
static pm_i128 pm_div_round(pm_i128 n,pm_i128 d) {
    const pm_u128 den=(pm_u128)d;
    const pm_u128 q=pm_div128(pm_abs128(n)+den/2,den);
    return n<0?-(pm_i128)q:(pm_i128)q;
}
static struct pm_u256 pm_mul128(pm_u128 a,pm_u128 b) {
    const uint64_t a0=(uint64_t)a,a1=(uint64_t)(a>>64),b0=(uint64_t)b,b1=(uint64_t)(b>>64);
    const pm_u128 p00=(pm_u128)a0*b0,p01=(pm_u128)a0*b1;
    const pm_u128 p10=(pm_u128)a1*b0,p11=(pm_u128)a1*b1;
    struct pm_u256 p;p.lo=p00;p.hi=p11+(p01>>64)+(p10>>64);
    pm_u128 old=p.lo;p.lo+=p01<<64;if(p.lo<old)++p.hi;
    old=p.lo;p.lo+=p10<<64;if(p.lo<old)++p.hi;
    return p;
}
static pm_u128 pm_div256(struct pm_u256 n,pm_u128 d,int *overflow) {
    pm_u128 q=0,r=0;*overflow=0;
    if(!d){*overflow=1;return 0;}
    for(int i=255;i>=0;--i) {
        const unsigned bit=(unsigned)((i>=128?n.hi>>(i-128):n.lo>>i)&1);
        const unsigned carry=(unsigned)(r>>127);
        r=(r<<1)|bit;
        if(carry || r>=d) {
            r-=d;
            if(i>=128)*overflow=1;else q|=(pm_u128)1<<i;
        }
    }
    return q;
}
static pm_i128 pm_muldiv(pm_i128 a,pm_i128 b,pm_i128 d,int *ok) {
    int overflow=0;
    const pm_u128 q=pm_div256(pm_mul128(pm_abs128(a),pm_abs128(b)),(pm_u128)d,&overflow);
    if(overflow || (q>>127)){*ok=0;return 0;}
    return (a<0)!=(b<0)?-(pm_i128)q:(pm_i128)q;
}
static pm_u128 pm_isqrt128(pm_u128 n) {
    pm_u128 bit=(pm_u128)1<<126,result=0;
    while(bit>n)bit>>=2;
    while(bit) {
        if(n>=result+bit){n-=result+bit;result=(result>>1)+bit;}else result>>=1;
        bit>>=2;
    }
    return result;
}
static int pm_config_valid(const struct nvt_partial_config *c) {
    return c && c->sigma_q16 && c->sigma_q16<=UINT32_C(536870912) &&
        c->huber_delta_q16 && c->huber_delta_q16<=UINT32_C(536870912) &&
        c->rms_limit_q16 && c->rms_limit_q16<=UINT32_C(536870912) &&
        c->budget_num && c->budget_num<=65536 && c->budget_den && c->budget_den<=65536 &&
        c->amplitude_min_q16>=0 && c->amplitude_max_q16>c->amplitude_min_q16 &&
        c->amplitude_max_q16<=INT32_C(536870912) &&
        c->background_min_q16>=-INT32_C(536870912) &&
        c->background_max_q16<=INT32_C(536870912) &&
        c->background_min_q16<c->background_max_q16 &&
        c->search_radius_q8 && c->search_radius_q8<=NVT_COLUMNS*256u &&
        c->max_evaluations && c->max_evaluations<=4096;
}
int nvt_partial_configure(struct nvt_partial_state *s,const struct nvt_partial_config *c) {
    if(!s || !pm_config_valid(c))return 0;
    s->config=*c;s->config_valid=1;return 1;
}
static uint32_t pm_weight(int32_t residual,const struct nvt_partial_config *c) {
    const uint64_t e=residual<0?-(int64_t)residual:residual;
    if(e<=c->huber_delta_q16)return 4194304;
    return (uint32_t)(((uint64_t)c->huber_delta_q16*4194304+e/2)/e);
}
struct pm_normal {
    pm_i128 xx,xy,yy,gx,gy; /* weighted projected H/score, common Q54: WQ22,JQ16 */
    pm_u128 sse,cost2;
    pm_u128 weight_den,weight_error; /* exact relative Q22 rounding interval */
};
static int pm_normal_build(const struct nvt_partial_math_sample *p,int n,
                  const struct nvt_partial_config *c,struct pm_normal *o) {
    pm_i128 sw=0,sk=0,sx=0,sy=0,se=0,kk=0,xk=0,yk=0,ke=0;
    pm_i128 xx=0,xy=0,yy=0,gx=0,gy=0;
    o->sse=o->cost2=0;
    o->weight_den=(pm_u128)c->huber_delta_q16*4194304;
    o->weight_error=0;
    if(n<1 || n>NVT_PM_MAX_SAMPLES)return 0;
    for(int j=0;j<n;++j) {
        if(p[j].k_q20<0 || p[j].k_q20>1048576 ||
           p[j].jx_q16>1073741824 || p[j].jx_q16< -1073741824 ||
           p[j].jy_q16>1073741824 || p[j].jy_q16< -1073741824)return 0;
        const pm_i128 w=pm_weight(p[j].residual_q16,c),k=p[j].k_q20;
        const pm_i128 x=p[j].jx_q16,y=p[j].jy_q16,e=p[j].residual_q16;
        sw+=w;sk+=w*k;sx+=w*x;sy+=w*y;se+=w*e;
        kk+=w*k*k;xk+=w*x*k;yk+=w*y*k;ke+=w*k*e;
        xx+=w*x*x;xy+=w*x*y;yy+=w*y*y;gx+=w*x*e;gy+=w*y*e;
        const pm_u128 ae=pm_abs128(e),delta=c->huber_delta_q16;
        if(ae>delta) {
            const pm_u128 rounded=(pm_u128)w*ae;
            const pm_u128 error=rounded>o->weight_den?rounded-o->weight_den:o->weight_den-rounded;
            if(error>o->weight_error)o->weight_error=error;
        }
        o->sse+=ae*ae;
        o->cost2+=ae<=delta?ae*ae:delta*(2*ae-delta);
    }
    if(sw<=0)return 0;
    /* Exact numerator Schur, not early-rounded means/centered K.
     * KK=sw*Skk-Sk虏; Bx=sw*Sxk-Sx*Sk. The final correction is
     * Bx*By/(sw*KK), evaluated by wide multiplication/division. Each final
     * entry is rounded only by its background and amplitude quotient.
     * K constant -> KK=0 -> remove only the shared constant nuisance. */
    const pm_i128 kn=sw*kk-sk*sk;
    const pm_i128 xn=sw*xk-sx*sk,yn=sw*yk-sy*sk,en=sw*ke-se*sk;
    xx-=pm_div_round(sx*sx,sw);xy-=pm_div_round(sx*sy,sw);
    yy-=pm_div_round(sy*sy,sw);gx-=pm_div_round(sx*se,sw);gy-=pm_div_round(sy*se,sw);
    if(kn<0)return 0;
    if(kn>0) {
        const pm_i128 denominator=sw*kn;
        int ok=1;
        xx-=pm_muldiv(xn,xn,denominator,&ok);xy-=pm_muldiv(xn,yn,denominator,&ok);
        yy-=pm_muldiv(yn,yn,denominator,&ok);gx-=pm_muldiv(xn,en,denominator,&ok);
        gy-=pm_muldiv(yn,en,denominator,&ok);
        if(!ok)return 0;
    }
    o->xx=xx;o->xy=xy;o->yy=yy;o->gx=gx;o->gy=gy;return 1;
}
static void pm_eigen_bounds(const struct pm_normal *a,pm_i128 *low,pm_i128 *high,
                              pm_i128 *major_low,pm_i128 *major_high) {
    pm_i128 x=a->xx,y=a->yy,b=a->xy,scale=1;
    /* Common scaling preserves orientation/eigenvalues. At most1unit per
     * coefficient is dropped; the spectral rounding bound is2*scale. */
    while(pm_abs128(x)>((pm_u128)1<<60) || pm_abs128(y)>((pm_u128)1<<60) ||
          pm_abs128(b)>((pm_u128)1<<60)){x>>=1;y>>=1;b>>=1;scale<<=1;}
    const pm_i128 trace=x+y,difference=x-y;
    const pm_u128 disc=pm_abs128(difference)*pm_abs128(difference)+4*pm_abs128(b)*pm_abs128(b);
    const pm_u128 sf=pm_isqrt128(disc);
    const pm_u128 sc=sf*sf==disc?sf:sf+1;
    const pm_i128 slack=2*scale+4; /* coefficient scaling + <=3raw Schur spectral error */
    *low=((trace-(pm_i128)sc)*scale)/2-slack;
    *high=((trace-(pm_i128)sf)*scale+1)/2+slack;
    *major_low=((trace+(pm_i128)sf)*scale)/2-slack;
    *major_high=((trace+(pm_i128)sc)*scale+1)/2+slack;
}
static uint64_t pm_sat64(pm_u128 value) {return value>UINT64_MAX?UINT64_MAX:(uint64_t)value;}
static int pm_compare256(struct pm_u256 a,struct pm_u256 b) {
    if(a.hi!=b.hi)return a.hi>b.hi?1:-1;
    return a.lo==b.lo?0:a.lo>b.lo?1:-1;
}
/* lambda_min(H)>T iff H-TI is positive definite. This is exactly the
 * eigenvalue admission criterion, without a det/trace surrogate or a
 * conservative sqrt interval moving the intended strict boundary. */
static int pm_positive_shift(const struct pm_normal *h,pm_u128 multiplier,pm_u128 threshold) {
    const pm_i128 a=h->xx*(pm_i128)multiplier-(pm_i128)threshold;
    const pm_i128 d=h->yy*(pm_i128)multiplier-(pm_i128)threshold;
    const pm_i128 b=h->xy*(pm_i128)multiplier;
    return a>0 && d>0 &&
        pm_compare256(pm_mul128((pm_u128)a,(pm_u128)d),pm_mul128(pm_abs128(b),pm_abs128(b)))>0;
}
static int pm_major_shift(const struct pm_normal *h,pm_u128 multiplier,pm_u128 threshold) {
    const pm_i128 a=h->xx*(pm_i128)multiplier-(pm_i128)threshold;
    const pm_i128 d=h->yy*(pm_i128)multiplier-(pm_i128)threshold;
    const pm_i128 b=h->xy*(pm_i128)multiplier;
    if(a>0 || d>0)return 1;
    return pm_compare256(pm_mul128(pm_abs128(a),pm_abs128(d)),pm_mul128(pm_abs128(b),pm_abs128(b)))<0;
}
/* Projected weighted information obeys the variational identity
 * v'H_W v=min_b sum_j w_j(Jp*v-Jn*b)^2. Therefore the relative Q22 weight
 * interval carries through nuisance elimination even if K is ill-conditioned.
 * Final Schur quotients contribute <=1.5raw/entry, <=3raw spectrally.
 * Classify against the original strict threshold, reporting UNKNOWN when these
 * numeric intervals overlap it. This is not an extra physical acceptance gate. */
static int pm_threshold_bounds(const struct pm_normal *a,pm_u128 numerator,
                               pm_u128 denominator,pm_i128 *upper,pm_i128 *lower) {
    if(!denominator || !a->weight_den || a->weight_error>=a->weight_den)return 0;
    const pm_u128 den=denominator*a->weight_den;
    int overflow=0;
    struct pm_u256 product=pm_mul128(numerator,a->weight_den+a->weight_error);
    pm_u128 q=pm_div256(product,den,&overflow);
    if(overflow || q>((pm_u128)1<<125))return 0;
    /* ceil without overflow: compare q*den to the original product. */
    if(pm_compare256(pm_mul128(q,den),product)<0)++q;
    *upper=(pm_i128)q+3;
    product=pm_mul128(numerator,a->weight_den-a->weight_error);
    q=pm_div256(product,den,&overflow);
    if(overflow || q>((pm_u128)1<<125))return 0;
    *lower=(pm_i128)q-3;
    return 1;
}
static int pm_classify_min(const struct pm_normal *a,pm_u128 numerator,pm_u128 denominator) {
    pm_i128 upper,lower;
    if(!pm_threshold_bounds(a,numerator,denominator,&upper,&lower))return 0;
    if(pm_positive_shift(a,1,(pm_u128)upper))return 1;
    if(lower>=0 && !pm_positive_shift(a,1,(pm_u128)lower))return -1;
    return 0;
}
static int pm_classify_major(const struct pm_normal *a,pm_u128 numerator,pm_u128 denominator) {
    pm_i128 upper,lower;
    if(!pm_threshold_bounds(a,numerator,denominator,&upper,&lower))return 0;
    if(pm_major_shift(a,1,(pm_u128)upper))return 1;
    if(lower>=0 && !pm_major_shift(a,1,(pm_u128)lower))return -1;
    return 0;
}
int nvt_partial_math_evaluate(const struct nvt_partial_math_sample *p,int n,
                               const struct nvt_partial_config *c,
                               struct nvt_partial_math_result *r) {
    if(!r)return 0;memset(r,0,sizeof(*r));
    if(!p || !pm_config_valid(c)){r->numerical_status=1;return 0;}
    struct pm_normal a;
    if(!pm_normal_build(p,n,c,&a)){r->numerical_status=2;return 0;}
    pm_i128 lo,hi,major_lo,major_hi;
    pm_eigen_bounds(&a,&lo,&hi,&major_lo,&major_hi);
    r->mse_q32=pm_sat64(pm_div128(a.sse,(unsigned)n));
    r->cost2_lo=(uint64_t)a.cost2;r->cost2_hi=(uint64_t)(a.cost2>>64);
    r->schur_xx_q16=(int64_t)pm_div_round(a.xx,(pm_i128)1<<38);
    r->schur_xy_q16=(int64_t)pm_div_round(a.xy,(pm_i128)1<<38);
    r->schur_yy_q16=(int64_t)pm_div_round(a.yy,(pm_i128)1<<38);
    if(hi>0)r->lambda_min_q16=pm_sat64(pm_div128((pm_u128)(lo+hi>0?(lo+hi)/2:0),(pm_u128)1<<38));
    if(major_hi>0)r->lambda_max_q16=pm_sat64(pm_div128((pm_u128)(major_lo+major_hi)/2,(pm_u128)1<<38));
    const int rms_ok=a.sse<(pm_u128)(unsigned)n*c->rms_limit_q16*c->rms_limit_q16;
    if(!rms_ok)return 1; /* exact unweighted RMS rejection */
    const pm_u128 sigma2=(pm_u128)c->sigma_q16*c->sigma_q16;
    const pm_u128 bn=(pm_u128)c->budget_num*c->budget_num,bd=(pm_u128)c->budget_den*c->budget_den;
    const pm_u128 minimum=sigma2*4194304;
    const pm_u128 threshold=9*minimum*bd;
    const int min_ok=pm_classify_min(&a,minimum,UINT64_C(10000000000));
    const int budget_ok=pm_classify_min(&a,threshold,bn);
    const int major_min=pm_classify_major(&a,minimum,UINT64_C(10000000000));
    const int major_budget=pm_classify_major(&a,threshold,bn);
    r->admitted=min_ok==1 && budget_ok==1;
    r->one_axis=major_min==1 && major_budget==1;
    if(!r->admitted && min_ok!=-1 && budget_ok!=-1)r->numerical_status=3;
    return 1;
}

#endif
