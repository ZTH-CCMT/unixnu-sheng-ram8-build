// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef SHENG_EXPERIMENTAL_TOUCH_ENGINE_H
#define SHENG_EXPERIMENTAL_TOUCH_ENGINE_H
#include <stdint.h>
extern "C" {
#include "NvtProtocol.h"
#include "NvtTouchCore.h"
#include "NvtPartialObservation.h"
#include "NvtCoverage.h"
}
// One owner, no allocation or device I/O. Only accepted frames advance the input stream.
// Only typed mutual observations create/update fingers; pen planes stay separate.
struct TouchEngine {
    enum { SpecialP=-2, Rejected=-3, Stale=-4 };
    nvt_touch_core *core;
    nvt_partial_state *partial;
    uint64_t streamUs, previousUs;
    uint16_t streamCounter=0;
    bool haveStreamCounter=false;
    uint16_t recoveryCounter=0;
    bool haveRecoveryCounter=false;
    unsigned partialFrames, partialPositions, partialRejected, transportCancelled;
    bool isPartial;
    struct CoverageFacts {
        nvt_coverage_policy policy;
        nvt_coverage_contact contact;
        uint64_t observed_epoch;
        unsigned epoch_trusted,baseline_trusted,mode_trusted;
    };
    // Owner-supplied verified facts only. No inferred human speed, contact
    // support radius or controller epoch is silently invented by the engine.
    typedef bool (*CoverageProvider)(void *,const nvt_slot &,uint64_t,CoverageFacts *);
    CoverageProvider coverageProvider;
    void *coverageContext;
    uint64_t sampleToken;
    explicit TouchEngine(nvt_touch_core *value,nvt_partial_state *pm=nullptr):
        core(value),partial(pm),streamUs(0),previousUs(0),
        partialFrames(0),partialPositions(0),partialRejected(0),transportCancelled(0),isPartial(false),
        coverageProvider(nullptr),coverageContext(nullptr),sampleToken(0) {}
    // The owner supplied this configuration explicitly. Reset clears templates
    // and packet age while preserving that same reviewed numeric profile.
    void resetPartial() {
        if(!partial)return;
        const bool configured=partial->config_valid!=0;
        const nvt_partial_config saved=partial->config;
        nvt_partial_reset(partial);
        if(configured)(void)nvt_partial_configure(partial,&saved);
    }
    void setCoverageProvider(CoverageProvider provider,void *context) {
        coverageProvider=provider;coverageContext=context;
    }
private:
    static constexpr unsigned PartialSamples = NVT_THP_PARTIAL_BLOCK_ROWS * NVT_COLUMNS;
    static_assert(NVT_THP_PARTIAL_BLOCK_ROWS * NVT_THP_PARTIAL_BLOCKS == NVT_ROWS,
                  "P block layout must cover the physical panel");
    static_assert(NVT_THP_PARTIAL_VALUES_OFFSET + 2 * PartialSamples <= NVT_THP_FRAME_LEN,
                  "P samples must fit in the THP frame");
    int coveredNegative(const uint8_t *values, unsigned firstRow, uint64_t nowUs,
                        uint32_t &fallbackIds, nvt_contact *out, int max) {
        if(!coverageProvider || !core->have_reference)return -1; // UNKNOWN
        int changed=-1;
        for(int i=0;i<NVT_SLOTS;++i) {
            const nvt_slot slot=core->slots[i];
            if(!slot.active || slot.age<2 || slot.last_observed_us>=nowUs ||
               slot.last_presence_us>=nowUs)continue;
            CoverageFacts facts={};
            if(!coverageProvider(coverageContext,slot,nowUs,&facts) ||
               facts.contact.position_us!=slot.last_observed_us ||
               !facts.policy.motion_bounds_verified ||
               !facts.contact.position_support_bounds_verified)continue;
            // A verified motion policy owns this contact, including cases
            // where its uncertainty needs more rows or signal is present.
            fallbackIds &= ~(UINT32_C(1) << slot.tracking_id);
            nvt_coverage_observation observation={};
            observation.epoch=facts.observed_epoch;observation.sample_token=sampleToken;
            observation.time_us=nowUs;observation.row0=firstRow;
            observation.row_count=NVT_THP_PARTIAL_BLOCK_ROWS;
            observation.values_le=values;observation.value_bytes=2 * PartialSamples;
            observation.baseline=core->reference;observation.fresh=1;
            observation.epoch_trusted=facts.epoch_trusted;
            observation.baseline_trusted=facts.baseline_trusted;
            observation.mode_trusted=facts.mode_trusted;
            nvt_coverage_result result={};
            if(nvt_coverage_negative(&facts.policy,&facts.contact,&observation,&result)==
               NVT_COVERAGE_COVERED_NEGATIVE) {
                const int n=nvt_touch_covered_negative(core,slot.tracking_id,slot.generation,nowUs,out,max);
                if(n>=0)changed=n;
            }
        }
        return changed;
    }
public:
    void clock(uint64_t nowUs) {
        if(previousUs && nowUs<previousUs) {
            resetPartial();
            streamUs=0;
            haveStreamCounter=false;
            haveRecoveryCounter=false;
        }
        previousUs=nowUs;
    }
    bool acceptCounter(uint16_t counter,uint64_t nowUs,bool &rebased) {
        if(!haveStreamCounter || nowUs<previousUs)return true;
        const uint16_t step=static_cast<uint16_t>(counter-streamCounter);
        if(step && step<0x8000) {
            haveRecoveryCounter=false;
            return true;
        }
        if(!step || nowUs-streamUs<NVT_OBSERVATION_MAX_US) {
            haveRecoveryCounter=false;
            return false;
        }

        // A long silence can hide a controller restart. Require a second
        // advancing frame before reopening the counter at the new position.
        if(haveRecoveryCounter) {
            const uint16_t recoveryStep=static_cast<uint16_t>(counter-recoveryCounter);
            if(!recoveryStep)return false;
            if(recoveryStep<0x8000) {
                haveRecoveryCounter=false;
                rebased=true;
                return true;
            }
        }
        recoveryCounter=counter;
        haveRecoveryCounter=true;
        return false;
    }
    int finishFrame(uint64_t nowUs,nvt_contact *out,int max,int observed,int before=-1) {
        const int expired=nvt_touch_expire(core,nowUs,out,max);
        if(expired>=0)return expired;
        return observed>=0 || before<0 ? observed : before;
    }
    int frame(const uint8_t *raw,size_t rawLength,uint64_t nowUs,int16_t *matrix,nvt_contact *out,int max) {
        isPartial=false;
        const auto kind=nvt_classify_frame(raw,rawLength);
        if(!core || !matrix || !out || kind==NVT_FRAME_INVALID)return Rejected;
        const uint16_t counter=static_cast<uint16_t>(nvt_frame_counter(raw));
        bool rebased=false;
        if(!acceptCounter(counter,nowUs,rebased))return Stale;
        core->cancelled_ids=0;
        // Retire old contacts before allowing a restarted counter to match.
        int before=-1;
        if(rebased || (core->clock_valid && nowUs<core->now_us))
            before=nvt_touch_expire(core,nowUs,out,max);
        clock(nowUs);
        const uint64_t previousFrameUs=streamUs;
        const bool freshStripe=!rebased && haveStreamCounter && nowUs>previousFrameUs;
        if(rebased)resetPartial();
        streamCounter=counter;haveStreamCounter=true;
        streamUs=nowUs;++sampleToken;
        if(kind==NVT_FRAME_PARTIAL) {
            isPartial=true;++partialFrames;
            const int overdue=nvt_touch_begin_observation(core,nowUs,out,max);
            if(overdue>=0)before=overdue;
            if(!partial)return finishFrame(nowUs,out,max,SpecialP,before);
            const unsigned firstRow=(raw[NVT_THP_PARTIAL_BLOCK_OFFSET]-1)*NVT_THP_PARTIAL_BLOCK_ROWS;
            const uint8_t *values=raw+NVT_THP_PARTIAL_VALUES_OFFSET;
            // Complete supported peaks use the exact ordinary-M candidate path.
            // A clipped peak is excluded here, and may be localized by K below.
            int16_t rows[PartialSamples];
            for(unsigned j=0;j<PartialSamples;++j) {
                const unsigned v=(unsigned)values[2*j]|((unsigned)values[2*j+1]<<8);
                rows[j]=static_cast<int16_t>((v&0x8000u)?(int)v-65536:(int)v);
            }
            nvt_region_candidates region={};
            int result=-1;
            if(nvt_touch_extract_region_at(core,rows,firstRow,NVT_THP_PARTIAL_BLOCK_ROWS,nowUs,&region)>=0)
                result=nvt_touch_process_region(core,&region,nowUs,out,max);
            nvt_contact measured[NVT_SLOTS];
            const int count=nvt_partial_measure(partial,core,raw,rawLength,nowUs,measured,NVT_SLOTS);
            if(count>=0)for(int i=0;i<NVT_SLOTS;++i) {
                const nvt_slot &slot=core->slots[i];
                if(slot.active && slot.age>=2 && slot.tracking_id>=0 && slot.tracking_id<32 &&
                   (partial->presence_ids&(1u<<slot.tracking_id)))
                    nvt_touch_note_presence(core,slot.tracking_id,slot.generation,nowUs);
            }
            if(count>0) {
                const int fitted=nvt_touch_process_partial(core,measured,count,nowUs,out,max);
                if(fitted>=0){result=fitted;partialPositions+=static_cast<unsigned>(count);}
            }
            if (freshStripe) {
                uint32_t fallbackIds=UINT32_MAX;
                const int covered=coveredNegative(values,firstRow,nowUs,fallbackIds,out,max);
                const int quiet=nvt_touch_quiet_stripe_for_ids(core,rows,firstRow,NVT_THP_PARTIAL_BLOCK_ROWS,
                                                               previousFrameUs,nowUs,fallbackIds,out,max);
                if(quiet>=0)result=quiet;
                else if(covered>=0)result=covered;
            }
            if(result<0)++partialRejected;
            return finishFrame(nowUs,out,max,result>=0?result:SpecialP,before);
        }
        nvt_frame_matrix(raw,matrix);
        const int result=nvt_touch_process_timed(core,matrix,nvt_frame_type(raw),nowUs,out,max);
        if(partial)nvt_partial_seed(partial,core,matrix,nowUs);
        return finishFrame(nowUs,out,max,result,before);
    }
    int expire(uint64_t nowUs,nvt_contact *out,int max) {
        clock(nowUs);
        if(core)core->cancelled_ids=0;
        const unsigned long before=core?core->observation_cancelled:0;
        const int result=nvt_touch_expire(core,nowUs,out,max);
        if(core && streamUs && nowUs>=streamUs && nowUs-streamUs>=NVT_OBSERVATION_MAX_US)
            transportCancelled+=static_cast<unsigned>(core->observation_cancelled-before);
        return result;
    }
    uint64_t waitUs(uint64_t nowUs)const {return nvt_touch_wait_us(core,nowUs);}
    unsigned waitMs(uint64_t nowUs,bool scriptedDown)const {
        const uint64_t us=waitUs(nowUs);
        if(!us)return 0;
        if(scriptedDown && us==1)return 100;
        return static_cast<unsigned>((us+999)/1000);
    }
};
#endif
