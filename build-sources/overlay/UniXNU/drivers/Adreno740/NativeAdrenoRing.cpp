/* SPDX-License-Identifier: MIT */
#include "NativeAdrenoRing.h"
#include "NativeAdrenoRingLog.h"
#include "AdrenoPM4.h"
#include "NativeAdrenoPreempt.h"
#include <IOKit/IOLib.h>

/* Values are taken from the pinned Linux source, not guessed:
 * drivers/gpu/drm/msm/registers/adreno/a6xx.xml (register offsets), and
 * registers/adreno/adreno_pm4.xml (opcode 0x46 CP_EVENT_WRITE7, event 0x31
 * CACHE_CLEAN, WRITE_SRC bits 20..22, WRITE_DST bit 24, WRITE_ENABLED bit 27,
 * IRQ bit 31). CP_RB_CNTL BUFSZ bits 0..5 and BLKSZ bits 8..13 are the same
 * fields MSM_GPU_RB_CNTL_DEFAULT programs. All are dword register indices. */
enum : uint32_t {
    RingBase = 0x0800,        /* 64-bit CP_RB_BASE */
    RingControl = 0x0802,     /* CP_RB_CNTL */
    RingRptrAddress = 0x0804, /* 64-bit CP_RB_RPTR_ADDR */
    RingRptr = 0x0806,
    RingWptr = 0x0807,
    RingScratch = 0x0885,     /* CP_SCRATCH(2) for A6XX..A7XX */
    RingFenceOpcode = 0x46,   /* CP_EVENT_WRITE7 on A7XX */
    RingFenceEvent = 0x31,    /* CACHE_CLEAN */
    RingFenceEnable = 1u << 27,
    RingFenceIrq = 1u << 31,
    RingBlockBytes = 32,
    /* A7XX framing, as a7xx_submit() in the pinned a6xx_gpu.c emits it: CP_THREAD_CONTROL (0x17; THREAD bits 0..1,
     * BR 1, BV 2, BOTH 3; SYNC_THREADS bit 31), CP_SET_MARKER (0x65; 0x101 IFPC disable, 0x100 enable), CCU_INVALIDATE_DEPTH
     * 0x18 / CCU_INVALIDATE_COLOR 0x19 and CP_WAIT_TIMESTAMP (0x14) from adreno_pm4.xml. User command streams synchronize
     * the BR and BV threads themselves (Turnip waits on BV timestamps), so the IBs must run with both threads enabled. */
    RingThreadControl = 0x17,
    RingThreadBr = 1,
    RingThreadBv = 2,
    RingThreadBoth = 3,
    RingSyncThreads = 1u << 31,
    RingSetMarker = 0x65,
    RingWaitTimestamp = 0x14,
    RingCcuInvalidateDepth = 0x18,
    RingCcuInvalidateColor = 0x19,
    /* What the SQE's CP_SMMU_TABLE_UPDATE handler does before it switches tables (a740_sqe.fw, qrisc-disasm): CACHE_FLUSH7 (0x32)
     * and CACHE_INVALIDATE7 (0x33) with a wait for idle (CP_WAIT_FOR_IDLE 0x26). Linux gets these from that packet on every context
     * switch; this ring never sends it, and event 0x31 is only CACHE_CLEAN on A7XX, so nothing else ever invalidated UCHE. */
    RingCacheFlush7 = 0x32,
    RingCacheInvalidate7 = 0x33,
    RingWaitForIdle = 0x26,
    RingPrologueWords = 11,
    /* a6xx_set_pagetable()'s A7XX context entry: CP_RESET_CONTEXT_STATE (0x1f; CLEAR_ON_CHIP_TS, CLEAR_RESOURCE_TABLE,
     * CLEAR_BV_BR_COUNTER, RESET_GLOBAL_LOCAL_TS = bits 0..3) between thread syncs, LRZ_FLUSH_INVALIDATE (0x26), the
     * RBBM_PERFCTR_SRAM_INIT_CMD (0x050e) write and CACHE_INVALIDATE (0x31), after a CP_WAIT_TIMESTAMP on the last fence, then the
     * CP_WAIT_REG_MEM (0x3c, WRITE_EQ) on RBBM_PERFCTR_SRAM_INIT_STATUS (0x050f) bit 0 that Linux always emits after the SRAM init
     * write. Without that wait the client's first IB started while the SRAM was still clearing; on sheng (int-064..067) the CP then
     * stopped on the IB's first CP_THREAD_CONTROL, while the same IB retired under the Linux driver. */
    RingResetContextState = 0x1f,
    RingResetAll = 0xf,
    RingLrzFlushInvalidate = 0x26,
    RingCacheInvalidate = 0x31,
    RingPerfctrSramInit = 0x050e,
    RingPerfctrSramInitStatus = 0x050f,
    RingWaitRegMem = 0x3c,
    RingWaitWriteEq = 3,
    RingResetWords = 26,
    /* a6xx_set_pagetable()'s table switch inside the context entry, after the SRAM init write: CP_SMMU_TABLE_UPDATE (0x53; TTBR0 low,
     * TTBR0 high with the ASID in bits 16..31, CONTEXTIDR 0, CONTEXTBANK 0: the GPU's bank is 0, as arm-smmu-qcom assigns it so the CP can
     * switch tables), then CP_THREAD_CONTROL SYNC_THREADS|BR so BV cannot run ahead while BR switches. The CACHE_INVALIDATE that follows
     * in the entry is the UCHE flush Linux sends after the switch. */
    RingSmmuTableUpdate = 0x53,
    RingTableWords = 7,
    RingMemWrite = 0x3d,
    RingTableSnapshotWords = 6,
    RingTableSnapshotOffset = 128, /* separate cache line from fence/rptr and the postamble */
    RingPostambleOffset = 64,
    RingPostambleWords = 11,
    RingSetAmble = 0x55,
    RingRegRmw = 0x21,
    RingRegToMem = 0x3e,      /* CP_REG_TO_MEM */
    RingAlwaysOnCounter = 0x0980,   /* CP_ALWAYS_ON_COUNTER, 64 bit */
    RingStampWords = 4,
    RingStampOffset = 512,
    RingStampSlots = 64,
    RingStampStride = 16,
    RingStampBytes = RingStampOffset+RingStampSlots*RingStampStride,
    RingFenceWords = 7,       /* kernel bring-up submissions: TYPE4 scratch + TYPE7 event */
    RingFramedFenceWords = 31,/* user submissions: scratch, BV timestamp, BR wait, ring timestamp */
    /* CP preemption between rings (a6xx_preempt.c, a6xx_gpu.c a6xx_emit_set_pseudo_reg): CP_SET_PSEUDO_REG (0x56) with the
     * SMMU info (written by the CPU on each switch), the non-secure save address (this ring's preemption record), the
     * non-privileged save address and the counters (both unused: 0). CP_CONTEXT_SWITCH_YIELD (0x6b, four words) ends a
     * submission by yielding the floor so the next ring can be switched in, as a6xx_flush_yield emits it; a zero address in
     * its third word skips the write the CP would make on completion. The SET_PSEUDO_REG payload and the yield's shape are
     * a6xx_gpu.c's: 12 dwords of (pseudo register, address low, address high) triples, then 4 dwords (0, 0, 1, 0). */
    RingSetPseudoReg = 0x56,
    RingPseudoSmmuInfo = 0,
    RingPseudoNonSecureSave = 1,
    RingPseudoNonPrivSave = 3,
    RingPseudoCounter = 4,
    RingPseudoWords = 17, /* pseudo registers plus the KMD postamble address */
    RingContextSwitchYield = 0x6b,
    RingYieldWords = 5,
    RingBvFenceOffset = 12    /* msm_rbmemptrs.bv_fence; +8 is bv_rptr, the BV RPTR shadow hw init programs */
};
static uint32_t ilog2(size_t value)
{
    uint32_t result=0;while(value>1) {value>>=1;++result;}return result;
}
/* The board compares a missing fence with the packets that should have written it. */
extern "C" {volatile NativeAdrenoRingLogRow native_adreno_ring_log[NativeAdrenoRingLogRows];volatile uint32_t native_adreno_ring_log_count;}
static IOLock *ring_log_lock;
static uint64_t ring_log_total;
static IOLock *ringLogLock()
{
    IOLock *lock=__atomic_load_n(&ring_log_lock,__ATOMIC_ACQUIRE);
    if(lock)return lock;
    IOLock *candidate=IOLockAlloc();
    if(!candidate)return nullptr;
    if(__atomic_compare_exchange_n(&ring_log_lock,&lock,candidate,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))
        return candidate;
    IOLockFree(candidate);
    return lock;
}
extern "C" uint32_t native_adreno_ring_log_copy(NativeAdrenoRingLogRow *out,uint32_t capacity)
{
    IOLock *lock=__atomic_load_n(&ring_log_lock,__ATOMIC_ACQUIRE);
    if(!out || !capacity || !lock)return 0;
    IOLockLock(lock);
    const uint32_t kept=ring_log_total<NativeAdrenoRingLogRows?uint32_t(ring_log_total):uint32_t(NativeAdrenoRingLogRows);
    const uint32_t rows=capacity<kept?capacity:kept;
    for(uint32_t i=0;i<rows;++i) {
        const volatile uint32_t *const src=native_adreno_ring_log[(ring_log_total-1-i)%NativeAdrenoRingLogRows];
        for(uint32_t k=0;k<NativeAdrenoRingLogRowWords;++k)out[i][k]=src[k];
    }
    IOLockUnlock(lock);
    return rows;
}
/* GPU page table switches: [0] by the CP (CP_SMMU_TABLE_UPDATE in a framed prologue), [1] by the CPU while the GPU was idle, [2] process
 * tables made, [3] process tables freed, [4] live process tables, [5] their peak, [6] the last failure (IOReturn), [7] its step. */
extern "C" {volatile uint32_t native_adreno_table_switches[8];}
IOReturn NativeAdrenoRing::poison()
{
    poisoned_=true;return kIOReturnIOError;
}
bool NativeAdrenoRing::publishBeforeMmio(const volatile uint32_t *words,size_t bytes)
{
    return !cache_.publish(cache_.cookie,words,bytes) &&
        (cache_.publish_orders_mmio || !io_.barrier(io_.cookie));
}
/* WPTR as a fenced register write: the GMU drops a write to CP_RB_WPTR that arrives while the GPU domain is not ready and flags it in
 * its AHB fence status; the write is then given again, at most 200 times. i491: a fence stalled on almost every submission until the
 * stall handler wrote WPTR again (5361 of 5397). [0] writes, [1] writes that were dropped at least once, [2] repeated writes, [3] writes
 * still dropped after the last try. */
extern "C" {volatile uint32_t native_adreno_wptr_fence[4];}
IOReturn NativeAdrenoRing::writeWptr(uint32_t value)
{
    __atomic_fetch_add(&native_adreno_wptr_fence[0],1u,__ATOMIC_RELAXED);
    if(io_.write32(io_.cookie,RingWptr,value))return poison();
    if(!io_.fence_status)return kIOReturnSuccess;
    uint32_t status=0;
    for(unsigned tries=0;;++tries) {
        if(io_.barrier(io_.cookie) || io_.fence_status(io_.fence_cookie,0,&status))return poison();
        if(!(status&1u))return kIOReturnSuccess;
        if(!tries)__atomic_fetch_add(&native_adreno_wptr_fence[1],1u,__ATOMIC_RELAXED);
        if(tries==200) {
            __atomic_fetch_add(&native_adreno_wptr_fence[3],1u,__ATOMIC_RELAXED);
            if(io_.fence_status(io_.fence_cookie,1,&status))return poison();
            return kIOReturnSuccess;   /* the stall handler gives WPTR again later */
        }
        __atomic_fetch_add(&native_adreno_wptr_fence[2],1u,__ATOMIC_RELAXED);
        if(io_.fence_status(io_.fence_cookie,1,&status))return poison();
        // Match msm fenced_write's delay to let the GPU power domain come up.
        // A successful first write pays no delay. The 200 retries budget 2 ms.
        IODelay(10);
        if(io_.write32(io_.cookie,RingWptr,value))return poison();
    }
}
IOReturn NativeAdrenoRing::readRptr(uint32_t *value)
{
    uint32_t rptr=0;
    /* The board enables APRIV read-pointer writeback to memptrs, also while the ring runs. Inactive rings have no dedicated
     * CP_RB_RPTR register. Keep reading their own shadows and avoid direct register reads when CP preemption/IFPC may be active. */
    if(secondary_ || preempt_) {
        if(cache_.acquire(cache_.cookie,fence_,fence_bytes_))return poison();
        rptr=fence_[0];
    } else if(io_.read32(io_.cookie,RingRptr,&rptr))return poison();
    if(rptr>=ring_bytes_/4)return poison(); /* unusable read pointer */
    *value=rptr;rptr_shadow_=rptr;return kIOReturnSuccess;
}
IOReturn NativeAdrenoRing::ibBefore(uint32_t rptr,uint64_t *iova,uint32_t *dwords) const
{
    if(!ring_ || !iova || !dwords)return kIOReturnBadArgument;
    const uint32_t mask=(uint32_t)(ring_bytes_/4-1);
    const uint32_t header=ring_[(rptr-4)&mask];
    if((header>>28)!=7u || ((header>>16)&0x7fu)!=0x3fu || (header&0x3fffu)!=3)return kIOReturnUnsupported;
    *iova=(uint64_t)ring_[(rptr-2)&mask]<<32|ring_[(rptr-3)&mask];
    *dwords=ring_[(rptr-1)&mask]&0xfffffu;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoRing::stallState(uint32_t *out)
{
    static constexpr uint32_t regs[]={
        0x0210,0x0211,0x0212,0x0213,0x0201,0x0821,0x0825,0x0824,RingRptr,RingWptr,
        0x0928,0x0929,0x092a,0x092b,0x092c,0x092d,0x0a66,0x0a6d,0x0a70,0x0a74,0x0a71,0x0a72};
    static_assert(sizeof(regs)/sizeof(regs[0])==NativeAdrenoRingStallWords,"stall register output layout");
    if(!out)return kIOReturnBadArgument;
    if(!attached_)return kIOReturnNotReady;
    for(uint32_t i=0;i<NativeAdrenoRingStallWords;++i)
        if(io_.read32(io_.cookie,regs[i],&out[i]))return kIOReturnIOError;
    return kIOReturnSuccess;
}
void NativeAdrenoRing::capturePublishedTail(NativeAdrenoPublishedTail *out) const
{
    if(!out)return;
    *out={};
    out->ring_words=uint32_t(ring_bytes_/4);out->software_next=next_;
    out->hw_rptr=out->hw_wptr=out->first=UINT32_MAX;
    out->read_status[0]=out->read_status[1]=out->barrier_status=-1;
    for(unsigned i=0;i<NativeAdrenoPublishedTailWords;++i)out->words[i]=UINT32_MAX;
    if(!attached_ || !ring_ || !io_.read32 || !io_.barrier || !out->ring_words)return;
    const uint32_t regs[2]={RingRptr,RingWptr};
    for(unsigned i=0;i<2;++i) {
        uint32_t value=UINT32_MAX;
        out->read_status[i]=io_.read32(io_.cookie,regs[i],&value);
        if(!out->read_status[i]) {
            (i?out->hw_wptr:out->hw_rptr)=value;out->valid_mask|=1u<<i;
        }
    }
    if(!(out->valid_mask&2u) || out->hw_wptr>=out->ring_words || next_>=out->ring_words)return;
    out->barrier_status=io_.barrier(io_.cookie);
    if(out->barrier_status)return;
    const uint32_t mask=out->ring_words-1;
    /* Pending commands can wrap almost a whole ring. Do not mistake their
     * overwritten bytes immediately before WPTR for the older published tail. */
    const uint32_t unpublished=(next_-out->hw_wptr)&mask;
    const uint32_t available=out->ring_words-unpublished;
    out->count=available<NativeAdrenoPublishedTailWords?available:NativeAdrenoPublishedTailWords;
    out->first=(out->hw_wptr-out->count)&mask;
    for(uint32_t i=0;i<out->count;++i)out->words[i]=ring_[(out->first+i)&mask];
    out->valid_mask|=4u;
}
IOReturn NativeAdrenoRing::kick()
{
    if(!attached_ || poisoned_)return kIOReturnNotReady;
    /* A ring the CP does not run (or while a switch is in flight) gets its write pointer from the switch's arrival, never from a register write here. */
    if(!current_) {restore_wptr_=true;return kIOReturnSuccess;}
    if(io_.barrier(io_.cookie))return poison();
    return writeWptr(next_);
}
IOReturn NativeAdrenoRing::drainBus(uint32_t *ack)
{
    enum : uint32_t {GbifHalt = 0x3c45, GbifHaltAck = 0x3c46, ClientHalt = 1u, ArbHalt = 2u};
    constexpr unsigned max_polls=20000;
    if(!attached_ || poisoned_ || !ack)return kIOReturnNotReady;
    ack[0]=ack[1]=0;
    const auto requestHalt=[&](uint32_t request,uint32_t mask,uint32_t &last_ack)->IOReturn {
        if(io_.write32(io_.cookie,GbifHalt,request))return kIOReturnIOError;
        for(unsigned i=0;i<max_polls;++i) {
            uint32_t value=0;
            if(io_.read32(io_.cookie,GbifHaltAck,&value))return kIOReturnIOError;
            last_ack=value;
            if(value&mask)return kIOReturnSuccess;
        }
        return kIOReturnTimeout;
    };
    IOReturn status=requestHalt(ClientHalt,ClientHalt,ack[0]);
    if(!status)status=requestHalt(ClientHalt|ArbHalt,ArbHalt,ack[1]);
    /* This is a temporary stall intervention. Hangcheck runs the board's own halt sequence before a reset. */
    if(io_.write32(io_.cookie,GbifHalt,0))return poison();
    return status;
}
/* Packs one TYPE7 packet into words; returns the next free index or 0 when the header cannot be built. */
static size_t packet(uint32_t *words,size_t at,uint32_t opcode,const uint32_t *payload,uint32_t count)
{
    if(AdrenoPM4Type7(opcode,count,&words[at])!=AdrenoPM4Success)return 0;
    for(uint32_t i=0;i<count;++i)words[at+1+i]=payload[i];
    return at+1+count;
}
/* CP_REG_TO_MEM of the always-on counter into the timeline slot of this fence (second: the end half). */
static size_t stamp(uint32_t *words,size_t at,uint64_t fence_iova,uint32_t seqno,bool end)
{
    const uint64_t to=fence_iova+RingStampOffset+(uint64_t)(seqno%RingStampSlots)*RingStampStride+(end?8:0);
    const uint32_t payload[3]={RingAlwaysOnCounter|(2u<<18)|(1u<<30),(uint32_t)to,(uint32_t)(to>>32)};
    return packet(words,at,RingRegToMem,payload,3);
}
/* Capture the diagnostic prefix while emitting it. Reading this prefix back
 * through Sheng's non-cacheable ring mapping serialized uncached loads
 * under both the submit gate and the log lock. */
struct NativeAdrenoRing::SubmitWriter {
    volatile uint32_t *ring;
    uint32_t mask,at;
    uint32_t prefix[NativeAdrenoRingLogPrefixWords];
    size_t captured=0;

    SubmitWriter(volatile uint32_t *ring,uint32_t mask,uint32_t start):ring(ring),mask(mask),at(start) {}

    void append(const uint32_t *words,size_t count)
    {
        const size_t space=sizeof(prefix)/sizeof(prefix[0])-captured;
        const size_t saved=count<space?count:space;
        size_t i=0;
        for(;i<saved;++i) {
            const uint32_t word=words[i];
            ring[(at+i)&mask]=word;
            prefix[captured+i]=word;
        }
        for(;i<count;++i)ring[(at+i)&mask]=words[i];
        at=(at+uint32_t(count))&mask;
        captured+=saved;
    }
};
/* Thread selection and IFPC disable before the user buffers, as a7xx_submit() does; the first submission after
 * requestContextReset() also carries the context entry a6xx_set_pagetable() emits when a new context first submits.
 * Every prologue ends its BR-only part with the GPU cache flush and invalidate described at RingCacheFlush7. */
/* `number` is the fence this submission ends with: the start stamp goes to that fence's slot, which is not seqno_+1 once the two rings share one numbering. */
IOReturn NativeAdrenoRing::emitPrologue(SubmitWriter &writer,bool reset,bool table,uint32_t number)
{
    uint32_t words[RingPrologueWords+RingResetWords+RingTableWords+RingTableSnapshotWords+RingStampWords+RingPseudoWords]={0};
    const uint32_t br=RingSyncThreads|RingThreadBr,syncBoth=RingSyncThreads|RingThreadBoth,both=RingThreadBoth,marker=0x101;
    if(table && !reset)return poison();
    const uint32_t count=RingPrologueWords+(reset?RingResetWords:0)+(table?RingTableWords+(preempt_?RingTableSnapshotWords:0):0)+(stamps_?RingStampWords:0)+(preempt_?RingPseudoWords:0);
    size_t n=packet(words,0,RingThreadControl,&br,1);
    if(reset) {
        const uint64_t at4=fence_iova_+4;
        const uint32_t wait[4]={0,(uint32_t)at4,(uint32_t)(at4>>32),seqno_},all=RingResetAll,lrz=RingLrzFlushInvalidate,
            invalidate=RingCacheInvalidate;
        if(n)n=packet(words,n,RingWaitTimestamp,wait,4);
        if(n)n=packet(words,n,RingThreadControl,&syncBoth,1);
        if(n)n=packet(words,n,RingResetContextState,&all,1);
        if(n)n=packet(words,n,RingThreadControl,&syncBoth,1);
        if(n)n=packet(words,n,RingFenceOpcode,&lrz,1);
        if(n)n=packet(words,n,RingThreadControl,&br,1);
        if(n && AdrenoPM4Type4(RingPerfctrSramInit,1,&words[n])==AdrenoPM4Success) {words[n+1]=1;n+=2;} else n=0;
        if(table) {
            const uint32_t update[4]={(uint32_t)table_,(uint32_t)(table_>>32),0,0};
            if(n)n=packet(words,n,RingSmmuTableUpdate,update,4);
            if(n && preempt_) {
                const uint64_t address=fence_iova_+RingTableSnapshotOffset;
                const uint32_t snapshot[5]={(uint32_t)address,(uint32_t)(address>>32),(uint32_t)table_,(uint32_t)(table_>>32),0};
                n=packet(words,n,RingMemWrite,snapshot,5);
            }
            if(n)n=packet(words,n,RingThreadControl,&br,1);
        }
        if(n)n=packet(words,n,RingFenceOpcode,&invalidate,1);
        const uint32_t sram[6]={RingWaitWriteEq,RingPerfctrSramInitStatus,0,1,1,0};
        if(n)n=packet(words,n,RingWaitRegMem,sram,6);
    }
    const uint32_t flush=RingCacheFlush7,invalidate7=RingCacheInvalidate7;
    if(n)n=packet(words,n,RingFenceOpcode,&flush,1);
    if(n)n=packet(words,n,RingFenceOpcode,&invalidate7,1);
    if(n)n=packet(words,n,RingWaitForIdle,nullptr,0);
    if(n && stamps_)n=stamp(words,n,fence_iova_,number,false);
    if(n && preempt_) {
        const uint32_t pseudo[12]={RingPseudoSmmuInfo,0,0,RingPseudoNonSecureSave,(uint32_t)record_iova_,(uint32_t)(record_iova_>>32),
            RingPseudoNonPrivSave,0,0,RingPseudoCounter,0,0};
        if(n)n=packet(words,n,RingSetPseudoReg,pseudo,12);
        const uint64_t address=fence_iova_+RingPostambleOffset;
        const uint32_t amble[3]={(uint32_t)address,(uint32_t)(address>>32),RingPostambleWords|(3u<<20)};
        if(n)n=packet(words,n,RingSetAmble,amble,3);
    }
    if(n)n=packet(words,n,RingThreadControl,&both,1);
    if(n)n=packet(words,n,RingSetMarker,&marker,1);
    if(n!=count)return poison();
    writer.append(words,count);
    return kIOReturnSuccess;
}
/* Writes the timestamp fence that makes a submission observable. */
IOReturn NativeAdrenoRing::emitFence(SubmitWriter &writer,uint32_t seqno)
{
    uint32_t words[RingFenceWords]={0};
    if(AdrenoPM4Type4(RingScratch,1,&words[0])!=AdrenoPM4Success ||
        AdrenoPM4Type7(RingFenceOpcode,4,&words[2])!=AdrenoPM4Success)return poison();
    words[1]=seqno;
    words[3]=RingFenceEvent|RingFenceEnable|RingFenceIrq;
    /* The timestamp goes to +4 (msm_rbmemptrs.fence), where wait() reads it; +0 is the CP's RPTR shadow. */
    const uint64_t at4=fence_iova_+4;
    words[4]=(uint32_t)at4;words[5]=(uint32_t)(at4>>32);words[6]=seqno;
    writer.append(words,RingFenceWords);
    return kIOReturnSuccess;
}
/* The fence of a framed submission: CP_SCRATCH(2), the BV thread's timestamp, a BR wait for it (so BR cannot report the
 * submission while BV still works on it), then the ring timestamp with an interrupt, both threads and IFPC back on. */
IOReturn NativeAdrenoRing::emitFramedFence(SubmitWriter &writer,uint32_t seqno,bool yield)
{
    uint32_t words[RingFramedFenceWords+RingStampWords+RingYieldWords]={0};
    const uint32_t total=RingFramedFenceWords+(stamps_?RingStampWords:0)+(yield?RingYieldWords:0);
    if(AdrenoPM4Type4(RingScratch,1,&words[0])!=AdrenoPM4Success)return poison();
    words[1]=seqno;
    /* The timestamp goes to +4 (msm_rbmemptrs.fence), where wait() reads it; +0 is the CP's RPTR shadow. */
    const uint64_t at4=fence_iova_+4,bv=fence_iova_+RingBvFenceOffset;
    const uint32_t br=RingThreadBr,bvThread=RingThreadBv,both=RingThreadBoth,depth=RingCcuInvalidateDepth,
        color=RingCcuInvalidateColor,marker=0x100;
    const uint32_t bvWrite[4]={RingFenceEvent|RingFenceEnable,(uint32_t)bv,(uint32_t)(bv>>32),seqno};
    const uint32_t bvWait[4]={0,(uint32_t)bv,(uint32_t)(bv>>32),seqno};
    const uint32_t fence[4]={RingFenceEvent|RingFenceEnable|RingFenceIrq,(uint32_t)at4,(uint32_t)(at4>>32),seqno};
    size_t n=packet(words,2,RingThreadControl,&br,1);
    if(n && stamps_)n=stamp(words,n,fence_iova_,seqno,true);
    if(n)n=packet(words,n,RingFenceOpcode,&depth,1);
    if(n)n=packet(words,n,RingFenceOpcode,&color,1);
    if(n)n=packet(words,n,RingThreadControl,&bvThread,1);
    if(n)n=packet(words,n,RingFenceOpcode,bvWrite,4);
    if(n)n=packet(words,n,RingThreadControl,&br,1);
    if(n)n=packet(words,n,RingWaitTimestamp,bvWait,4);
    if(n)n=packet(words,n,RingFenceOpcode,fence,4);
    if(n)n=packet(words,n,RingThreadControl,&both,1);
    if(n)n=packet(words,n,RingSetMarker,&marker,1);
    if(n && yield) {
        const uint32_t floor[4]={0,0,1,0};
        if(n)n=packet(words,n,RingContextSwitchYield,floor,4);
    }
    if(n!=total)return poison();
    writer.append(words,total);
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoRing::attach(const NativeAdrenoRingIo *io,const NativeAdrenoRingCache *cache,
    void *ring,size_t ring_bytes,uint64_t ring_iova,void *fence,size_t fence_bytes,uint64_t fence_iova)
{
    if(attached_ || poisoned_ || ring_ || fence_)return kIOReturnBusy;
    if(!io || !io->write32 || !io->read32 || !io->barrier || !cache ||
        !cache->publish || !cache->acquire || !ring || !fence ||
        ring_bytes<8192 || ring_bytes>(1u<<20) || (ring_bytes&(ring_bytes-1)) ||
        ((ring_iova|ring_bytes)&(ring_bytes-1)) || (ring_iova&4095) ||
        fence_bytes<64 || (fence_iova&7) || fence_bytes>=(1u<<24))return kIOReturnBadArgument;
    /* No register may be touched before every argument is acceptable. */
    io_=*io;cache_=*cache;ring_=static_cast<volatile uint32_t *>(ring);
    ring_bytes_=ring_bytes;ring_iova_=ring_iova;
    fence_=static_cast<volatile uint32_t *>(fence);fence_bytes_=fence_bytes;fence_iova_=fence_iova;
    for(size_t i=0;i<fence_bytes_/4;++i)fence_[i]=0;
    if(!publishBeforeMmio(fence_,fence_bytes_))return poison();
    control_=(uint32_t)(ilog2(ring_bytes_/8)&0x3f)|(ilog2(RingBlockBytes/8)<<8);
    IOReturn status=program();
    if(status)return status;
    seqno_=0;completed_=0;submissions_=0;
    attached_=true;return kIOReturnSuccess;
}
/* A second ring of the same GPU: the CP has one ring register set, so this one programs none of it. Its base, read-pointer shadow
 * and control word reach the CP through its preemption record on each switch (a6xx_preempt_trigger writes the record before the
 * control word). The read pointer is the record's to report, so the ring starts at its own index 0. */
IOReturn NativeAdrenoRing::attachSecondary(const NativeAdrenoRingIo *io,const NativeAdrenoRingCache *cache,
    void *ring,size_t ring_bytes,uint64_t ring_iova,void *fence,size_t fence_bytes,uint64_t fence_iova)
{
    if(attached_ || poisoned_ || ring_ || fence_)return kIOReturnBusy;
    if(!io || !io->write32 || !io->read32 || !io->barrier || !cache || !cache->publish || !cache->acquire || !ring || !fence || ring_bytes<8192 || ring_bytes>(1u<<20) || (ring_bytes&(ring_bytes-1)) ||
        ((ring_iova|ring_bytes)&(ring_bytes-1)) || (ring_iova&4095) ||
        fence_bytes<64 || (fence_iova&7) || fence_bytes>=(1u<<24))return kIOReturnBadArgument;
    io_=*io;cache_=*cache;ring_=static_cast<volatile uint32_t *>(ring);
    ring_bytes_=ring_bytes;ring_iova_=ring_iova;
    fence_=static_cast<volatile uint32_t *>(fence);fence_bytes_=fence_bytes;fence_iova_=fence_iova;
    for(size_t i=0;i<ring_bytes_/4;++i)ring_[i]=0;
    for(size_t i=0;i<fence_bytes_/4;++i)fence_[i]=0;
    if(!publishBeforeMmio(fence_,fence_bytes_))return poison();
    control_=(uint32_t)(ilog2(ring_bytes_/8)&0x3f)|(ilog2(RingBlockBytes/8)<<8);
    next_=0;rptr_shadow_=0;seqno_=0;completed_=0;submissions_=0;
    secondary_=true;current_=false;attached_=true;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoRing::setPreempt(uint64_t record,uint64_t smmu_info,uint64_t bv_rptr_addr)
{
    if(!attached_ || poisoned_)return kIOReturnNotReady;
    if(!record || !smmu_info || fence_bytes_<RingTableSnapshotOffset+12)return kIOReturnBadArgument;
    record_iova_=record;smmu_iova_=smmu_info;bv_rptr_iova_=bv_rptr_addr;
    preempt_=true;
    initPreemptPage();
    if(cache_.publish(cache_.cookie,fence_,fence_bytes_))return poison();
    return kIOReturnSuccess;
}
/* The KMD postamble initializes performance-counter SRAM after saving a context.
 * Hardware packet fields: adreno_pm4.xml CP_REG_RMW, CP_WAIT_REG_MEM and CP_SET_AMBLE. */
void NativeAdrenoRing::initPreemptPage()
{
    uint32_t words[RingPostambleWords]={};
    const uint32_t init[3]={RingPerfctrSramInit,0,1};
    const uint32_t wait[6]={RingWaitWriteEq,RingPerfctrSramInitStatus,0,1,1,0};
    size_t n=packet(words,0,RingRegRmw,init,3);
    n=packet(words,n,RingWaitRegMem,wait,6);
    for(size_t i=0;i<n;++i)fence_[RingPostambleOffset/4+i]=words[i];
}
IOReturn NativeAdrenoRing::resetExecutedTable(uint64_t ttbr0)
{
    if(!attached_ || poisoned_ || !preempt_)return kIOReturnNotReady;
    if(!ttbr0)return kIOReturnBadArgument;
    if(cache_.acquire(cache_.cookie,fence_,fence_bytes_))return poison();
    fence_[RingTableSnapshotOffset/4]=(uint32_t)ttbr0;
    fence_[RingTableSnapshotOffset/4+1]=(uint32_t)(ttbr0>>32);
    fence_[RingTableSnapshotOffset/4+2]=0;
    return cache_.publish(cache_.cookie,fence_,fence_bytes_)?poison():kIOReturnSuccess;
}
IOReturn NativeAdrenoRing::executedTable(uint64_t *ttbr0)
{
    if(!ttbr0)return kIOReturnBadArgument;
    if(!attached_ || poisoned_ || !preempt_)return kIOReturnNotReady;
    if(cache_.acquire(cache_.cookie,fence_,fence_bytes_))return poison();
    *ttbr0=(uint64_t)fence_[RingTableSnapshotOffset/4]|(uint64_t)fence_[RingTableSnapshotOffset/4+1]<<32;
    return *ttbr0?kIOReturnSuccess:kIOReturnNotReady;
}
IOReturn NativeAdrenoRing::writePreemptRecord(uint8_t *record) const
{
    if(!record)return kIOReturnBadArgument;
    if(!NativeAdrenoPreempt::writeRecord(record,ring_iova_,fence_iova_,bv_rptr_iova_,control_,next_))return kIOReturnBadArgument;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoRing::writePreemptWptr(uint8_t *record)
{
    if(!NativeAdrenoPreempt::setRecordWptr(record,next_))return kIOReturnBadArgument;
    /* The CP restores this WPTR from the record. The submission lock also
     * covers this flag: only work appended after this snapshot needs another
     * WPTR write when the switch completes (a6xx_preempt_trigger). */
    restore_wptr_=false;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoRing::restoreWptr()
{
    if(!attached_ || poisoned_ || !current_)return kIOReturnNotReady;
    if(!restore_wptr_)return kIOReturnSuccess;
    if(writeWptr(next_))return kIOReturnIOError;
    restore_wptr_=false;
    return kIOReturnSuccess;
}

uint32_t NativeAdrenoRing::readHardwarePointers(uint32_t &rptr,uint32_t &wptr) const
{
    rptr=wptr=UINT32_MAX;
    if(!attached_)return 0;
    uint32_t valid=0,value=0;
    if(!io_.read32(io_.cookie,RingRptr,&value)){rptr=value;valid|=1u;}
    if(!io_.read32(io_.cookie,RingWptr,&value)){wptr=value;valid|=2u;}
    return valid;
}
/* Program an empty ring while the CP is halted or reset. */
IOReturn NativeAdrenoRing::program()
{
    uint32_t rptr=0;
    const IOReturn status=readRptr(&rptr);
    if(status)return status;
    // Reject a nonzero RPTR before publishing WPTR zero, which would make an empty ring appear to contain work.
    if(rptr)return poison();
    const uint32_t control=control_?control_:(uint32_t)(ilog2(ring_bytes_/8)&0x3f)|(ilog2(RingBlockBytes/8)<<8);
    const uint32_t base_lo=(uint32_t)ring_iova_,base_hi=(uint32_t)(ring_iova_>>32);
    const uint32_t rptr_lo=(uint32_t)fence_iova_,rptr_hi=(uint32_t)(fence_iova_>>32);
    const uint32_t writes[]={RingBase,base_lo,RingBase+1,base_hi,RingControl,control,
        RingRptrAddress,rptr_lo,RingRptrAddress+1,rptr_hi,RingWptr,0,RingScratch,0};
    for(size_t i=0;i<sizeof(writes)/sizeof(writes[0]);i+=2)
        if(io_.write32(io_.cookie,writes[i],writes[i+1]))return poison();
    next_=0;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoRing::restart(uint32_t completed)
{
    if(!attached_ || secondary_)return kIOReturnNotReady;
    /* Not checked against this ring's own last number: the two rings share one number space, so the hung or completed fence the caller
     * restarts at may be one the other ring issued (untagged-cbea3217c: the hung job 71 sat on ring 1, ring 0 had issued only 70). */
    poisoned_=false;
    /* msm_rbmemptrs: +0 rptr, +4 fence, +8 bv_rptr, +12 bv_fence. */
    for(size_t i=0;i<fence_bytes_/4;++i)fence_[i]=0;
    fence_[1]=completed;fence_[RingBvFenceOffset/4]=completed;
    if(preempt_)initPreemptPage();
    for(size_t i=0;i<ring_bytes_/4;++i)ring_[i]=0;
    if(cache_.publish(cache_.cookie,fence_,fence_bytes_) || !publishBeforeMmio(ring_,ring_bytes_))return poison();
    IOReturn status=program();
    if(status)return status;
    seqno_=completed;completed_=completed;
    current_=true;restore_wptr_=false;
    context_=0;reset_pending_=true;table_=0;table_pending_=false;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoRing::restartSecondary(uint32_t completed)
{
    if(!attached_ || !secondary_)return kIOReturnNotReady;
    poisoned_=false;
    for(size_t i=0;i<fence_bytes_/4;++i)fence_[i]=0;
    fence_[1]=completed;fence_[RingBvFenceOffset/4]=completed;
    if(preempt_)initPreemptPage();
    for(size_t i=0;i<ring_bytes_/4;++i)ring_[i]=0;
    if(cache_.publish(cache_.cookie,fence_,fence_bytes_) || cache_.publish(cache_.cookie,ring_,ring_bytes_))return poison();
    next_=0;rptr_shadow_=0;seqno_=completed;completed_=completed;
    current_=false;restore_wptr_=false;reset_pending_=true;table_pending_=false;
    return kIOReturnSuccess;
}
void NativeAdrenoRing::setTimeline(bool on)
{
    stamps_=on && attached_ && fence_bytes_>=RingStampBytes;
}
uint32_t NativeAdrenoRing::timeline(uint32_t fence,uint64_t *start,uint64_t *end)
{
    /* status bits: 1 attached, 2 stamps on, 4 page big enough, 8 page acquired, 16 slot read */
    if(!start || !end)return 0;
    *start=*end=0;
    uint32_t status=(attached_?1u:0u)|(stamps_?2u:0u)|(fence_bytes_>=RingStampBytes?4u:0u);
    if(status!=7)return status;
    if(cache_.acquire(cache_.cookie,fence_,fence_bytes_))return status;
    status|=8;
    const volatile uint32_t *slot=fence_+(RingStampOffset+(fence%RingStampSlots)*RingStampStride)/4;
    *start=(uint64_t)slot[1]<<32|slot[0];*end=(uint64_t)slot[3]<<32|slot[2];
    return status|16;   /* the raw words: the caller judges them */
}
IOReturn NativeAdrenoRing::fenceValue(uint32_t *value)
{
    if(!value)return kIOReturnBadArgument;*value=0;
    if(!attached_)return kIOReturnNotReady;
    if(cache_.acquire(cache_.cookie,fence_+1,sizeof(*fence_)))return kIOReturnIOError;
    *value=fence_[1];
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoRing::submitUnfenced(const uint32_t *words,size_t word_count)
{
    if(!attached_ || poisoned_ || !current_)return kIOReturnNotReady;
    if(!words || !word_count)return kIOReturnBadArgument;
    const uint32_t size_words=(uint32_t)(ring_bytes_/4),mask=size_words-1;
    if(word_count>size_words-8)return kIOReturnNoResources;
    uint32_t rptr=0;IOReturn status=readRptr(&rptr);
    if(status)return status;
    if(word_count+8>size_words-((next_-rptr)&mask))return kIOReturnNoResources;
    for(size_t i=0;i<word_count;++i)ring_[(next_+i)&mask]=words[i];
    const uint32_t end=(next_+(uint32_t)word_count)&mask;
    if(!publishBeforeMmio(ring_,ring_bytes_))return poison();
    if(writeWptr(end))return kIOReturnIOError;
    next_=end;
    return kIOReturnSuccess;
}
bool NativeAdrenoRingClock::waitForPoll(uint64_t start_us,uint32_t timeout_us) const
{
    if(!now_us || !timeout_us)return false;
    const uint64_t elapsed=now_us(cookie)-start_us;
    if(elapsed>=timeout_us)return false;
    if(idle)return !idle(cookie);
    // Early bring-up cannot sleep. Poll quickly for short waits, then back off
    // without extending the timeout merely to reach the next interval.
    const uint32_t interval=elapsed<100?10:elapsed<1000?40:100;
    const uint32_t remaining=timeout_us-uint32_t(elapsed);
    IODelay(interval<remaining?interval:remaining);
    return true;
}
IOReturn NativeAdrenoRing::idle(const NativeAdrenoRingClock *clock,uint32_t timeout_us)
{
    enum : uint32_t {RbbmStatus=0x0210,GpuBusyIgnAhb=1u<<23};
    if(!attached_ || poisoned_ || !current_)return kIOReturnNotReady;
    if(!clock || !clock->now_us)return kIOReturnBadArgument;
    const uint64_t start=clock->now_us(clock->cookie);
    for(;;) {
        uint32_t rptr=0,status=0;
        IOReturn read=readRptr(&rptr);
        if(read)return read;
        if(rptr==next_) {
            if(io_.read32(io_.cookie,RbbmStatus,&status))return poison();
            if(!(status&GpuBusyIgnAhb))return kIOReturnSuccess;
        }
        if(!clock->waitForPoll(start,timeout_us))return kIOReturnTimeout;
    }
}
IOReturn NativeAdrenoRing::progressState(uint32_t out[6])
{
    enum : uint32_t {Ib1Base=0x0928,Ib1Rem=0x092a,Ib2Base=0x092b,Ib2Rem=0x092d,RoqAvailIb1=0x0949,RoqAvailIb2=0x094a};
    if(!out)return kIOReturnBadArgument;
    if(!attached_)return kIOReturnNotReady;
    uint32_t roq1=0,roq2=0;
    if(io_.read32(io_.cookie,Ib1Base,&out[0]) || io_.read32(io_.cookie,Ib1Base+1,&out[1]) ||
        io_.read32(io_.cookie,Ib2Base,&out[2]) || io_.read32(io_.cookie,Ib2Base+1,&out[3]) ||
        io_.read32(io_.cookie,Ib1Rem,&out[4]) || io_.read32(io_.cookie,Ib2Rem,&out[5]) ||
        io_.read32(io_.cookie,RoqAvailIb1,&roq1) || io_.read32(io_.cookie,RoqAvailIb2,&roq2))return kIOReturnIOError;
    out[4]+=roq1>>16;out[5]+=roq2>>16;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoRing::busHalt(bool gx_off,unsigned spins)
{
    enum : uint32_t {RbbmGbifHalt=0x0016,RbbmGbifHaltAck=0x0017,GbifHalt=0x3c45,GbifHaltAck=0x3c46,ClientHalt=1u,ArbHalt=2u};
    if(!attached_)return kIOReturnNotReady;
    if(!spins)return kIOReturnBadArgument;
    const auto request=[&](uint32_t reg,uint32_t ack_reg,uint32_t mask)->IOReturn {
        if(io_.write32(io_.cookie,reg,mask))return kIOReturnIOError;
        for(unsigned i=0;i<spins;++i) {
            uint32_t ack=0;
            if(io_.read32(io_.cookie,ack_reg,&ack))return kIOReturnIOError;
            if(ack&mask)return kIOReturnSuccess;
        }
        return kIOReturnTimeout;
    };
    IOReturn status=gx_off?request(RbbmGbifHalt,RbbmGbifHaltAck,1):kIOReturnSuccess;
    const bool gbif_requested=!status;
    if(gbif_requested) {
        status=request(GbifHalt,GbifHaltAck,ClientHalt);
        if(!status)status=request(GbifHalt,GbifHaltAck,ArbHalt);
        // A confirmed GX halt remains in place until the reset and board hw_init.
        if(!status && !io_.write32(io_.cookie,GbifHalt,0))return kIOReturnSuccess;
        if(!status)status=kIOReturnIOError;
    }
    // A failed MMIO write can have reached hardware. Clear every halt this call attempted.
    const bool gbif_uncertain=gbif_requested && io_.write32(io_.cookie,GbifHalt,0);
    const bool gx_uncertain=gx_off && io_.write32(io_.cookie,RbbmGbifHalt,0);
    return (gbif_uncertain || gx_uncertain)?poison():status;
}
IOReturn NativeAdrenoRing::detach()
{
    if(poisoned_)return kIOReturnBusy;
    if(!attached_)return kIOReturnNotReady;
    if(outstanding())return kIOReturnBusy;
    attached_=false;stamps_=false;ring_=nullptr;fence_=nullptr;ring_bytes_=fence_bytes_=0;
    ring_iova_=fence_iova_=0;next_=rptr_shadow_=seqno_=completed_=submissions_=0;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoRing::submit(const uint32_t *words,size_t word_count,uint32_t *fence)
{
    if(!fence)return kIOReturnBadArgument;
    return submitWords(words,word_count,seqno_+1,false,current_,fence);
}
IOReturn NativeAdrenoRing::submitFramed(const uint32_t *words,size_t word_count,uint32_t *fence)
{
    if(!fence)return kIOReturnBadArgument;
    return submitWords(words,word_count,seqno_+1,true,current_,fence);
}
IOReturn NativeAdrenoRing::submitFramed(const uint32_t *words,size_t word_count,uint32_t number,uint32_t *issued)
{
    if(!issued)return kIOReturnBadArgument;*issued=0;
    // The ring's CP wait-timestamp uses the previous BV value until the new job
    // reaches its end. Keep a numbered jump within the forward serial half-range.
    if(number<=seqno_ || number-seqno_>INT32_MAX)return kIOReturnBadArgument;
    return submitWords(words,word_count,number,true,current_,issued);
}
IOReturn NativeAdrenoRing::submitWords(const uint32_t *words,size_t word_count,uint32_t number,bool framed,bool give_wptr,uint32_t *fence)
{
    if(!fence)return kIOReturnBadArgument;*fence=0;
    if(!attached_ || poisoned_)return kIOReturnNotReady;
    if(!number)return kIOReturnNoResources;  /* Fence zero is reserved. */
    if(!words || !word_count || word_count>SIZE_MAX/sizeof(uint32_t))return kIOReturnBadArgument;
    const uint32_t size_words=(uint32_t)(ring_bytes_/4),mask=size_words-1;
    const bool reset=framed && reset_pending_,table=reset && table_pending_;
    /* A plain (kernel) submission cannot carry the switch: a pending table waits for the next framed one. */
    const uint32_t extra=framed?RingPrologueWords+(reset?RingResetWords:0)+(table?RingTableWords+(preempt_?RingTableSnapshotWords:0):0)+RingFramedFenceWords+
        (stamps_?2*RingStampWords:0)+(preempt_?RingPseudoWords+RingYieldWords:0):RingFenceWords;
    if(word_count>size_words-extra-8)return kIOReturnNoResources;
    uint32_t rptr=0;IOReturn status=readRptr(&rptr);
    if(status)return status;
    const uint32_t used=(next_-rptr)&mask;
    if(word_count+extra+8>size_words-used)return kIOReturnNoResources;
    SubmitWriter writer(ring_,mask,next_);
    if(framed) {
        status=emitPrologue(writer,reset,table,number);
        if(status)return status;
    }
    writer.append(words,word_count);
    /* Every framed submission of a ring with a preemption record ends with the yield: the second ring's too, or the CP never
     * leaves it (the switch back stays pending with STOP set). */
    status=framed?emitFramedFence(writer,number,preempt_):emitFence(writer,number);
    if(status)return status;
    const uint32_t end=writer.at;
    /* A submission may wrap. One publication covers both pieces. Sheng's
     * non-cacheable ring mapping needs only an ordering barrier here. */
    if(IOLock *const lock=ringLogLock()) {
        IOLockLock(lock);
        const uint32_t total=(end-next_)&mask;
        volatile uint32_t *const record=native_adreno_ring_log[ring_log_total%NativeAdrenoRingLogRows];
        record[0]=number;record[1]=next_;record[2]=end;record[3]=total;
        for(size_t i=0;i<writer.captured;++i)record[NativeAdrenoRingLogHeaderWords+i]=writer.prefix[i];
        for(size_t i=writer.captured;i<sizeof(writer.prefix)/sizeof(writer.prefix[0]);++i)record[NativeAdrenoRingLogHeaderWords+i]=0;
        ++ring_log_total;
        __atomic_store_n(&native_adreno_ring_log_count,uint32_t(ring_log_total),__ATOMIC_RELEASE);
        IOLockUnlock(lock);
    }
    if(!publishBeforeMmio(ring_,ring_bytes_))return poison();
    /* A secondary ring appends its words but leaves CP_RB_WPTR alone: the CP reads them only after it is switched in, and the
     * switch's completion gives the pointer (restoreWptr), as msm's ring->restore_wptr does for a ring that is not current. */
    if(give_wptr) {if(writeWptr(end))return kIOReturnIOError;}
    else restore_wptr_=true;
    next_=end;seqno_=number;*fence=number;++submissions_;
    if(reset)reset_pending_=false;
    if(table) {table_pending_=false;native_adreno_table_switches[0]=native_adreno_table_switches[0]+1;}
    return kIOReturnSuccess;
}
NativeAdrenoRingFence NativeAdrenoRing::observeFence(uint32_t fence,
    const NativeAdrenoRingClock *clock,uint32_t timeout_us,uint32_t *observed)
{
    if(observed)*observed=0;
    if(!attached_ || poisoned_)return NativeAdrenoRingFencePoisoned;
    if(!fence || fence>seqno_)return NativeAdrenoRingFenceArgument;
    const uint64_t start=timeout_us && clock && clock->now_us?clock->now_us(clock->cookie):0;
    for(;;) {
        uint32_t value=0;
        if(fenceValue(&value)) {poison();return NativeAdrenoRingFencePoisoned;}
        if(observed)*observed=value;
        if(value>seqno_) {poison();return NativeAdrenoRingFencePoisoned;}
        if(value>completed_)completed_=value;
        if(value>=fence)return NativeAdrenoRingFenceComplete;
        if(!clock || !clock->waitForPoll(start,timeout_us))return NativeAdrenoRingFenceTimeout;
    }
}
NativeAdrenoRingFence NativeAdrenoRing::wait(uint32_t fence,const NativeAdrenoRingClock *clock,
    uint32_t timeout_us,uint32_t *observed)
{
    return observeFence(fence,clock,timeout_us,observed);
}
