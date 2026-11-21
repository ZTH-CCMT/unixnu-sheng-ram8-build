/* SPDX-License-Identifier: MIT */
/* Real VM, ring and execbuf code over two software register peers. */
#include "XnuMemoryFixture.h"
#include "NativeAdrenoSubmit.h"
#include "NativeAdrenoRingLog.h"
#include "AdrenoPM4.h"
#include <cstdio>
#include <cstring>
#include <vector>
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)
#define SMMU_BYTES 0x40000
#define CB 0x22000
static bool gpu_trace_enabled;
extern "C" int native_gpu_trace_enabled() {return gpu_trace_enabled;}
extern "C" volatile uint64_t native_adreno_wptr_handoff[3][11];
extern "C" volatile uint32_t native_adreno_unstick[15];
extern "C" volatile uint32_t native_adreno_vm_tail_resume[2];
extern "C" volatile uint64_t native_adreno_stall_log[32][8];
extern "C" volatile uint32_t native_adreno_stall_log_count;
extern "C" bool native_adreno_stall_pointers_copy(uint32_t index,uint32_t out[6]);
extern "C" bool native_adreno_timeline_range(uint64_t *generation,uint64_t *first,uint64_t *end);
extern "C" bool native_adreno_timeline_copy(uint64_t generation,uint64_t index,uint64_t out[8]);

IOReturn NativeAdrenoQueryCpu4K(NativeAdrenoCpuState *out)
{CHECK(out);*out={};return kIOReturnSuccess;}
IOReturn NativeAdrenoMaintainOwnedPages(void *mapping,size_t bytes,NativeAdrenoCacheOperation op)
{
    CHECK(mapping && bytes && !(((uintptr_t)mapping|bytes)&4095));
    CHECK(op==NativeAdrenoClean || op==NativeAdrenoCleanInvalidate || op==NativeAdrenoInvalidate);
    fixture.cache_operations.push_back(op);
    ++fixture.cache_calls;
    return fixture.cache_calls==fixture.cache_fail_call?kIOReturnIOError:kIOReturnSuccess;
}
/* SMMUv2 register peer. */
static uint32_t mmio[SMMU_BYTES/4];
static struct {unsigned calls,fail_call,after;} mf;
static ArmSmmu4KIo smmuio;
static ArmSmmu4KStreamState streams;
static ArmSmmu4KStream routes[2]={{2,0},{4,1}};
static ArmSmmu4KStreamResult route_result;
static ArmSmmu4KContextConfig context;
static ArmSmmu4KContextImage context_image;
static int mevent(void){return ++mf.calls==mf.fail_call;}
static int mread32(void *c,size_t at,uint32_t *v)
{CHECK(c==&mf && at%4==0 && at<=SMMU_BYTES-4);if(mevent())return 1;*v=mmio[at/4];return 0;}
static int mread64(void *c,size_t at,uint64_t *v)
{CHECK(c==&mf && at%8==0 && at<=SMMU_BYTES-8);if(mevent())return 1;std::memcpy(v,mmio+at/4,8);return 0;}
static int mwrite32(void *c,size_t at,uint32_t v)
{CHECK(c==&mf && at%4==0 && at<=SMMU_BYTES-4);int f=mevent();if(f && !mf.after)return 1;mmio[at/4]=v;return f;}
static int mwrite64(void *c,size_t at,uint64_t v)
{CHECK(c==&mf && at%8==0 && at<=SMMU_BYTES-8);int f=mevent();if(f && !mf.after)return 1;std::memcpy(mmio+at/4,&v,8);return f;}
static int mbarrier(void *c){CHECK(c==&mf);return mevent();}
/* CP register peer. */
struct CpPeer {
    std::vector<uint32_t> regs;
    std::vector<uint32_t> writes;
    unsigned publish=0,acquire=0,barriers=0,fail_publish=0,fail_acquire=0,fail_barrier=0;
    unsigned sqe_calls=0;bool diagnostic_callbacks=true,clear_stop_on_write=false,fail_halt_clear=false;
    uint32_t fail_read_reg=~0u,fail_write_reg=~0u;
    CpPeer():regs(0x10000,0){}
};
static CpPeer cp;
static uint64_t clock_us=0;
static int cwrite(void *c,uint32_t reg,uint32_t v)
{
    CHECK(c==&cp && reg<cp.regs.size());
    if(reg==cp.fail_write_reg || (reg==0x3c45 && !v && cp.fail_halt_clear))return 1;
    cp.regs[reg]=reg==0x08a0 && cp.clear_stop_on_write?v&~1u:v;
    cp.writes.push_back(reg);
    return 0;
}
static int cgmuvote(void *,uint32_t reg,uint32_t value)
{CHECK(reg==0x1f8c4 && value<=1);return 0;}
static int cread(void *c,uint32_t reg,uint32_t *v)
{CHECK(c==&cp && v && reg<cp.regs.size());if(reg==cp.fail_read_reg)return 1;*v=cp.regs[reg];return 0;}
static int cbarrier(void *c){CHECK(c==&cp);++cp.barriers;return cp.barriers==cp.fail_barrier;}
static int cpublish(void *c,const volatile uint32_t *w,size_t bytes)
{CHECK(c==&cp && w && (bytes==32768 || bytes==4096));++cp.publish;return cp.publish==cp.fail_publish;}
static int cacquire(void *c,volatile uint32_t *w,size_t bytes)
{CHECK(c==&cp && w && (bytes==4096 || bytes==sizeof(*w)));++cp.acquire;return cp.acquire==cp.fail_acquire;}
static uint64_t cnow(void *c){CHECK(c==&cp);return clock_us;}
static int cidle(void *c){CHECK(c==&cp);clock_us+=1000;return 0;}
struct Path {
    NativeAdrenoTablePages tables;
    NativeAdrenoMemory object;
    ArmSmmu4KLiveArena arena;
    ArmSmmu4KLiveState state{};
    ArmSmmu4KLiveVisibility visibility;
    NativeAdrenoVm vm;
    NativeAdrenoRing ring;
    NativeAdrenoSubmit submit;
    uint64_t handle=0;
};
static uint32_t ringmem[8192],fencemem[1024];
static std::vector<uint64_t> dram_reads;
static NativeAdrenoRingIo ringio{&cp,cwrite,cread,cbarrier,nullptr,nullptr};
static NativeAdrenoRingCache ringcache{&cp,cpublish,cacquire};
static NativeAdrenoRingClock ringclock{&cp,cnow,cidle};
static void smmuReset(void)
{
    std::memset(mmio,0,sizeof(mmio));mf={};streams=(ArmSmmu4KStreamState){};
    smmuio=(ArmSmmu4KIo){&mf,SMMU_BYTES,mread32,mread64,mwrite32,mwrite64,mbarrier};
    mmio[0x20/4]=(1u<<30)|(1u<<27)|(4u<<9)|8;
    mmio[0x24/4]=0x40000010;mmio[0x28/4]=0x1433;mmio[0]=0x436;
    for(unsigned i=0;i<8;++i)mmio[(0xc00+4*i)/4]=0x20000;
    mmio[0x800/4]=1u<<31|9;mmio[0xc00/4]=3;
}
static int exposed(Path &p,bool write_combined=false)
{
    smmuReset();cp=CpPeer{};clock_us=0;
    std::memset(ringmem,0xef,sizeof(ringmem));std::memset(fencemem,0,sizeof(fencemem));
    if(NativeAdrenoPrepareSmmuMemory(&p.tables,32,40,0x800000,65536,0,16,&p.arena,&p.state,&p.visibility))
        return kIOReturnBadArgument;
    context=(ArmSmmu4KContextConfig){.lower=&p.arena.config,.lower_tables=&p.state.tables,
        .id0=mmio[0x20/4],.id2=mmio[0x28/4],.asid=1,.coherent_walk=1,
        .stall_on_fault=0,.continue_after_fault=1};
    ArmSmmu4KContextConfig actual=context;
    CHECK(ArmSmmu4KEncodeContext(&actual,&context_image)==ArmSmmu4KSuccess);
    mmio[0x1008/4]=context_image.cbar;mmio[0x1808/4]=context_image.cba2r;
    mmio[CB/4]=context_image.sctlr;mmio[(CB+0x10)/4]=context_image.tcr2;mmio[(CB+0x30)/4]=context_image.tcr;
    std::memcpy(mmio+(CB+0x20)/4,&context_image.ttbr0,8);
    std::memcpy(mmio+(CB+0x28)/4,&context_image.ttbr1,8);
    mmio[(CB+0x38)/4]=context_image.mair0;
    if(ArmSmmu4KAttachStreams(&smmuio,&streams,routes,2,2,&context,&route_result))return kIOReturnBadArgument;
    NativeAdrenoVmRegion lower={&p.tables,&p.arena,&p.state};
    if(p.vm.attach(&smmuio,&context,&streams,&lower,nullptr,32,1000))return kIOReturnBadArgument;
    if(p.object.allocate(8192,40,false,8192))return kIOReturnNoMemory;
    if(write_combined && p.object.makeWriteCombined())return kIOReturnNoMemory;
    if(p.vm.map(&p.object,0,0x800000,3,0,2,&p.handle))return kIOReturnBadArgument;
    if(p.ring.attach(&ringio,&ringcache,ringmem,sizeof(ringmem),0x100000,fencemem,sizeof(fencemem),0x108000))
        return kIOReturnBadArgument;
    return p.submit.attach(&p.vm,&p.ring);
}
static void attachProcess(Path &process,ArmSmmu4KContextConfig &config)
{
    CHECK(NativeAdrenoPrepareSmmuMemory(&process.tables,32,40,0x800000,65536,0,16,
        &process.arena,&process.state,&process.visibility)==kIOReturnSuccess);
    config=context;
    config.lower=&process.arena.config;
    config.lower_tables=&process.state.tables;
    config.asid=16;
    config.ttbr0_switched=1;
    process.state.detached=1;
    process.state.context_offset=CB;
    const NativeAdrenoVmRegion lower{&process.tables,&process.arena,&process.state};
    process.vm.keepStreamsOnFailure();
    CHECK(process.vm.attach(&smmuio,&config,&streams,&lower,nullptr,32,1000)==kIOReturnSuccess);
    CHECK(process.object.allocate(8192,40,false,8192)==kIOReturnSuccess);
    CHECK(process.vm.map(&process.object,0,0x800000,3,0,2,&process.handle)==kIOReturnSuccess);
}
void NativeAdrenoNotePhase(unsigned,size_t) {}
extern "C" {extern volatile uint32_t native_adreno_submit_ibs[4],native_adreno_diag,native_adreno_ib_log[4][4+28],native_adreno_ib_log_count;
    extern volatile uint64_t native_adreno_submit_us[8];
    extern volatile uint32_t native_adreno_switch_timeouts;}

struct DualRingPath : Path {
    NativeAdrenoRing low;
    uint32_t low_words[8192]{};
    uint32_t low_fences[1024]{};
    alignas(4) uint8_t records[2][64]{};
    alignas(4) uint8_t smmu_info[2][32]{};
    bool keepalive=false;
    unsigned restart_count=0;
    uint32_t restart_fence=~0u;

    void setup()
    {
        CHECK(exposed(*this)==kIOReturnSuccess);
        CHECK(low.attachSecondary(&ringio,&ringcache,low_words,sizeof(low_words),0x200000,
            low_fences,sizeof(low_fences),0x208000)==kIOReturnSuccess);
        CHECK(ring.setPreempt(0x300000,0x400000,0x108008)==kIOReturnSuccess);
        CHECK(low.setPreempt(0x500000,0x600000,0x208008)==kIOReturnSuccess);
        NativeAdrenoSubmit::PreemptSetup config{};
        config.io={&cp,cwrite,cread,cbarrier,this,[](void *cookie,uint32_t reg,uint32_t value)->int {
            CHECK(reg==0x1f8c4 && value<=1);
            static_cast<DualRingPath *>(cookie)->keepalive=value!=0;
            return 0;
        }};
        config.io.sqe_snapshot=[](void *cookie,NativeAdrenoSqeSnapshot *out)->int {
            CHECK(cookie==&cp);++cp.sqe_calls;
            // A failed/unsupported callback can leave no valid data.
            if(!cp.diagnostic_callbacks)return -55;
            CHECK(cp.regs[0x08a0]&1); // Hardware reset has not yet cleared STOP.
            out->words[0][0]=0;out->valid_mask[0][0]=1;
            out->words[1][0]=0x741;out->valid_mask[1][0]=1;
            out->read_status[0]=out->read_status[1]=0;
            return 0;
        };
        config.pages={{0x300000,0x500000},{0x400000,0x600000},{records[0],records[1]},{smmu_info[0],smmu_info[1]}};
        config.now=cnow;
        config.clock=&cp;
        config.mode=2;
        CHECK(submit.setPreemption(&ring,&low,config,true)==kIOReturnSuccess);
        CHECK(submit.setTableSwitching(2,[](void *,uint64_t)->IOReturn {return kIOReturnSuccess;},
            nullptr)==kIOReturnSuccess);
        clock_us=1;
    }

    uint32_t enqueue(int priority,uint64_t queue)
    {
        const NativeAdrenoSubmitBuffer buffer{handle,0x800000,4096};
        uint32_t fence=0;
        submit.nextPriority(priority);
        CHECK(submit.submit(&vm,&buffer,1,queue,&fence,&ringclock)==kIOReturnSuccess);
        return fence;
    }

    void signalSwitch()
    {
        CHECK(cp.regs[0x08a0]&1);
        cp.regs[0x08a0]&=~1u;
        CHECK(submit.preemptIrq());
    }

    void beginReverseSwitch(uint32_t &high_pending,uint32_t &low_done)
    {
        const uint32_t high=enqueue(0,8);
        const uint32_t low_first=enqueue(1,7);
        CHECK(low.lastSubmitted()==low_first && !(cp.regs[0x08a0]&1));
        // Aging grants ring 1 a slice even though ring 0 has a pending fence.
        clock_us+=300000;
        CHECK(submit.retireCompleted()==kIOReturnSuccess && keepalive);
        fencemem[1]=high;
        signalSwitch();
        CHECK(low.current() && !keepalive);
        high_pending=enqueue(0,8);
        low_done=enqueue(1,7);
        CHECK(low_done>high_pending && low.lastSubmitted()==low_done);
        CHECK(!(cp.regs[0x08a0]&1));
        // The newest global fence retires on ring 1 while older ring 0 work is queued.
        low_fences[1]=low_done;
        CHECK(submit.retireCompleted()==kIOReturnSuccess && keepalive);
        CHECK(cp.regs[0x08a0]&1);
        CHECK(!ring.current() && !low.current());
        CHECK(submit.fencePending(high_pending) && !submit.fencePending(low_done));
    }

    IOReturn recover(NativeAdrenoRecovery &out)
    {
        return submit.recover([](void *cookie,NativeAdrenoRing *ring,uint32_t completed)->IOReturn {
            auto &path=*static_cast<DualRingPath *>(cookie);
            ++path.restart_count;
            path.restart_fence=completed;
            cp.regs[0x08a0]=0;
            return ring->restart(completed);
        },this,&ringclock,&out);
    }

    IOReturn recoverBoard(NativeAdrenoRecovery &out)
    {
        return submit.recover([](void *cookie,NativeAdrenoRing *ring,uint32_t completed)->IOReturn {
            auto &path=*static_cast<DualRingPath *>(cookie);
            ++path.restart_count;path.restart_fence=completed;
            for(auto &reg:cp.regs)reg=0; // Full reset loses CP state, including the pseudo save address.
            IOReturn status=ring->restart(completed);
            if(status)return status;
            // gpuRestart's real post-reset path: ten CP init words, then two
            // secure-mode words. Neither command installs a preemption record
            // in the CP, emits a yield, or consumes a user fence number.
            uint32_t init[10]={};
            CHECK(AdrenoPM4Type7(0x17,1,&init[0])==AdrenoPM4Success);
            CHECK(AdrenoPM4Type7(0x48,7,&init[2])==AdrenoPM4Success);
            init[1]=1u<<27;init[3]=0x14b;init[4]=3;init[5]=0x20000000;
            init[6]=2;init[7]=0x700000;init[9]=1u<<31;
            status=ring->submitUnfenced(init,10);if(status)return status;
            fencemem[0]=cp.regs[0x0806]=ring->writePointer();
            uint32_t secure[2]={};
            CHECK(AdrenoPM4Type7(0x66,1,&secure[0])==AdrenoPM4Success);
            status=ring->submitUnfenced(secure,2);if(status)return status;
            fencemem[0]=cp.regs[0x0806]=ring->writePointer();
            CHECK(cp.regs[0x0806]==12 && cp.regs[0x0807]==12);
            CHECK(fencemem[1]==completed && ring->lastSubmitted()==completed);
            return ring->idle(&ringclock,0);
        },this,&ringclock,&out);
    }
};

static void testHiddenActiveFence()
{
    DualRingPath p;
    p.setup();
    uint32_t blocked=0,completed=0;
    p.beginReverseSwitch(blocked,completed);
    p.signalSwitch();
    CHECK(p.ring.current() && p.submit.fencePending(blocked));
    // The current ring's command processor stays stationary while only its older fence is outstanding.
    bool hung=false;
    for(unsigned tick=0;tick<5 && !hung;++tick) {
        clock_us+=500000;
        CHECK(p.submit.hangCheck(&hung)==kIOReturnSuccess);
    }
    CHECK(hung);
    CHECK(p.submit.fencePending(blocked));
    CHECK(p.submit.observeFence(completed,nullptr,0)==kIOReturnSuccess);
    std::puts("PASS completed newer fence on inactive ring cannot hide a stopped active ring");
}

static void testSwitchCompletesWithoutInterrupt()
{
    DualRingPath p;
    p.setup();
    uint32_t blocked=0,completed=0;
    p.beginReverseSwitch(blocked,completed);
    const NativeAdrenoPreemptStats before=p.submit.preemptStats();
    // STOP is clear by the watchdog sample, but CP_SW never reached the driver.
    clock_us+=1000001;
    cp.regs[0x08a0]&=~1u;
    bool hung=true;
    CHECK(p.submit.hangCheck(&hung)==kIOReturnSuccess && !hung);
    const NativeAdrenoPreemptStats after=p.submit.preemptStats();
    CHECK(after.triggers==before.triggers && after.completed==before.completed+1);
    CHECK(after.faulted==before.faulted && p.submit.deadReason()==0 && p.submit.stats().restarts==0);
    CHECK(p.ring.current() && !p.low.current() && !p.keepalive);
    CHECK(cp.regs[0x0807]==p.ring.writePointer() && !p.ring.restorePending());
    CHECK(p.submit.fencePending(blocked) && !p.submit.fencePending(completed));
    fencemem[1]=blocked;
    CHECK(p.submit.retireCompleted()==kIOReturnSuccess && p.submit.outstanding()==0);
    CHECK(p.submit.wait(blocked,&ringclock,0)==kIOReturnSuccess);
    std::puts("PASS watchdog collects a completed switch with no interrupt or reset");
}


static void testImmediateSwitchCompletion()
{
    for(bool fail_control_read:{false,true}) {
        DualRingPath p;
        p.setup();
        cp.clear_stop_on_write=true;
        if(fail_control_read)cp.fail_read_reg=0x08a0;
        const uint32_t fence=p.enqueue(1,5);
        CHECK(p.submit.preemptStats().triggers==1);
        if(fail_control_read) {
            CHECK(!p.low.current() && p.keepalive && p.submit.preemptStats().completed==0);
            cp.fail_read_reg=~0u;
            CHECK(p.submit.preemptIrq());
        }
        CHECK(p.low.current() && !p.ring.current() && !p.keepalive);
        CHECK(p.submit.preemptStats().completed==1 && p.submit.preemptStats().faulted==0);
        CHECK(!p.submit.preemptIrq());  // A delayed worker must not complete the same switch twice.
        p.low_fences[1]=fence;
        CHECK(p.submit.retireCompleted()==kIOReturnSuccess && !p.submit.outstanding());
    }
    std::puts("PASS immediate STOP-clear claims completion under the submission lock");
}

static void testFenceSequenceExhaustion()
{
    DualRingPath p;
    p.setup();
    CHECK(p.ring.restart(UINT32_C(0x7ffffffe))==kIOReturnSuccess);
    uint32_t fence=p.enqueue(0,9);
    CHECK(fence==UINT32_C(0x7fffffff));
    // A far-ahead unknown ID must not alias the retired end of the sequence.
    CHECK(p.submit.wait(UINT32_MAX,&ringclock,0)==kIOReturnBadArgument);
    CHECK(p.submit.observeFence(UINT32_MAX,&ringclock,0)==kIOReturnBadArgument);
    fencemem[1]=fence;
    CHECK(p.submit.retireCompleted()==kIOReturnSuccess);
    fence=p.enqueue(0,9);
    CHECK(fence==UINT32_C(0x80000000));
    uint32_t submitted=0,retired=0;
    CHECK(p.submit.fenceProgress(&submitted,&retired,nullptr));
    CHECK(submitted==fence && retired==fence-1);
    fencemem[1]=fence;
    CHECK(p.submit.retireCompleted()==kIOReturnSuccess);
    CHECK(p.ring.restart(UINT32_MAX-1)==kIOReturnSuccess);
    fence=p.enqueue(0,9);
    CHECK(fence==UINT32_MAX);
    const unsigned cacheBefore=fixture.cache_calls;
    const size_t writesBefore=cp.writes.size(),refsBefore=p.object.deviceReferences();
    const NativeAdrenoSubmitBuffer buffer{p.handle,0x800000,4096};
    uint32_t exhausted=1;
    CHECK(p.submit.submit(&buffer,1,9,&exhausted)==kIOReturnNoResources && !exhausted);
    CHECK(p.submit.stats().fence_exhaustions==1 && fixture.cache_calls==cacheBefore &&
        cp.writes.size()==writesBefore && p.object.deviceReferences()==refsBefore);
    CHECK(p.submit.wait(fence,&ringclock,0)==kIOReturnTimeout);
    fencemem[1]=fence;
    CHECK(p.submit.wait(fence,&ringclock,0)==kIOReturnSuccess);
    CHECK(p.submit.detach()==kIOReturnSuccess);
    std::puts("PASS monotonic GPU fences cross the sign boundary and reject reuse after UINT32_MAX");
}

static void testTimelineDetach()
{
    Path p;
    native_adreno_diag=1;
    CHECK(exposed(p)==kIOReturnSuccess);
    NativeAdrenoSubmit second;
    CHECK(second.attach(&p.vm,&p.ring)==kIOReturnSuccess);
    native_adreno_diag=0;
    uint64_t generation=0,first=1,end=1,row[8]{};
    CHECK(native_adreno_timeline_range(&generation,&first,&end) && first==0 && end==0);
    CHECK(!native_adreno_timeline_copy(generation,0,row));
    const NativeAdrenoSubmitBuffer buffer{p.handle,0x800000,4096};
    uint32_t fence=0;
    CHECK(p.submit.submit(&buffer,1,0,&fence)==kIOReturnSuccess);
    CHECK(p.submit.detach()==kIOReturnBusy && native_adreno_timeline_range(nullptr,nullptr,nullptr));
    fencemem[1]=fence;
    CHECK(p.submit.wait(fence,&ringclock,0)==kIOReturnSuccess);
    CHECK(p.submit.detach()==kIOReturnSuccess && native_adreno_timeline_copy(generation,0,row));
    CHECK(uint32_t(row[0])==fence);
    CHECK(second.detach()==kIOReturnSuccess && !native_adreno_timeline_range(nullptr,nullptr,nullptr));
    CHECK(!native_adreno_timeline_copy(generation,0,row));
    native_adreno_diag=1;
    CHECK(second.attach(&p.vm,&p.ring)==kIOReturnSuccess);
    native_adreno_diag=0;
    uint64_t newGeneration=0;
    CHECK(native_adreno_timeline_range(&newGeneration,&first,&end) && newGeneration!=generation && first==0 && end==0);
    CHECK(second.submit(&buffer,1,0,&fence)==kIOReturnSuccess);
    fencemem[1]=fence;
    CHECK(second.wait(fence,&ringclock,0)==kIOReturnSuccess);
    CHECK(!native_adreno_timeline_copy(generation,0,row));
    CHECK(native_adreno_timeline_copy(newGeneration,0,row) && uint32_t(row[0])==fence);
    CHECK(second.detach()==kIOReturnSuccess && !native_adreno_timeline_range(nullptr,nullptr,nullptr));
    std::puts("PASS job timeline is shared by active diagnostic submitters and retires with its last owner");
}

static void testDisablePreemptionRestoresPrimary()
{
    for(bool immediate:{false,true}) {
        DualRingPath p;
        p.setup();
        const auto disable=[&] {return p.submit.setPreemption(nullptr,nullptr,{},false);};
        const uint32_t lowFence=p.enqueue(1,5);
        p.signalSwitch();
        CHECK(p.low.current() && !p.ring.current() && disable()==kIOReturnBusy);
        p.low_fences[1]=lowFence;
        CHECK(p.submit.retireCompleted()==kIOReturnSuccess && !p.submit.outstanding());
        const uint32_t target[2]={0,lowFence};
        uint32_t retired[2]{};
        // The fence is complete but CP has not yet consumed the final ring words.
        CHECK(disable()==kIOReturnBusy && p.submit.preemptStats().triggers==1 && p.low.current());
        p.low_fences[0]=p.low.writePointer();
        cp.regs[0x0210]=1u<<23;
        CHECK(disable()==kIOReturnBusy && p.submit.preemptStats().triggers==1 && p.low.current());
        cp.regs[0x0210]=0;
        cp.clear_stop_on_write=immediate;
        const IOReturn result=disable();
        if(!immediate) {
            CHECK(result==kIOReturnBusy && p.submit.preemption() && p.keepalive);
            CHECK(!p.low.current() && !p.ring.current());
            CHECK(disable()==kIOReturnBusy && p.submit.preemptStats().triggers==2);
            // A control-read failure cannot make the unfinished primary current.
            cp.fail_read_reg=0x08a0;
            CHECK(disable()==kIOReturnIOError && p.keepalive && p.submit.preemption());
            cp.fail_read_reg=~0u;
            cp.regs[0x08a0]&=~1u;
            CHECK(disable()==kIOReturnSuccess); // The IRQ worker need not have run yet.
        } else {
            CHECK(result==kIOReturnSuccess);
        }
        CHECK(!p.submit.preemption() && p.ring.current() && !p.low.current() && !p.keepalive);
        CHECK(p.submit.wait(lowFence,&ringclock,0)==kIOReturnSuccess);
        CHECK(p.submit.observeFence(lowFence,&ringclock,0)==kIOReturnSuccess);
        CHECK(p.submit.scanoutProgress(target,retired)==kIOReturnSuccess && retired[1]>=lowFence);
        CHECK(p.enqueue(1,5)==lowFence+1 && p.ring.lastSubmitted()==lowFence+1);
        fencemem[1]=lowFence+1;
        CHECK(p.submit.retireCompleted()==kIOReturnSuccess && p.submit.detach()==kIOReturnSuccess);
    }
    std::puts("PASS disabling preemption drains secondary, confirms primary, and preserves fence sequence");
}

static void testSwitchCompletesBeforeRecovery()
{
    DualRingPath p;
    p.setup();
    uint32_t blocked=0,completed=0;
    p.beginReverseSwitch(blocked,completed);
    clock_us+=1000001;
    bool hung=false;
    CHECK(p.submit.hangCheck(&hung)==kIOReturnSuccess && hung);

    // STOP can clear between the watchdog's last poll and the restart decision.
    cp.regs[0x08a0]&=~1u;
    NativeAdrenoRecovery result{};
    CHECK(p.recover(result)==kIOReturnSuccess);
    CHECK(result.completed_late && result.replayed==0 && !result.guilty);
    CHECK(p.restart_count==0 && p.submit.stats().restarts==0);
    CHECK(p.ring.current() && !p.low.current() && !p.keepalive);
    CHECK(p.submit.deadReason()==0 && p.submit.preemptStats().completed==2 && p.submit.preemptStats().faulted==0);
    NativeAdrenoPreemptStats late_stats{};
    uint32_t late_loaded=~0u,late_cntl=~0u,late_dead=~0u;
    bool late_flight=true;
    CHECK(p.submit.preemptStats(&late_stats,&late_loaded,&late_flight,&late_cntl,&late_dead));
    CHECK(late_dead==0 && late_loaded==0 && !late_flight);
    CHECK(p.submit.wait(blocked,&ringclock,0)==kIOReturnTimeout);
    fencemem[1]=blocked;
    CHECK(p.submit.wait(blocked,&ringclock,0)==kIOReturnSuccess);
    CHECK(p.submit.wait(completed,&ringclock,0)==kIOReturnSuccess);
    // A late completion did not reset the hardware. It must not permanently
    // disable scheduling on ring 1 merely because the watchdog had timed out.
    const uint32_t before_triggers=p.submit.preemptStats().triggers;
    const uint32_t low_after_late=p.enqueue(1,19);
    CHECK(p.low.lastSubmitted()==low_after_late && (cp.regs[0x08a0]&1));
    CHECK(p.submit.preemptStats().triggers==before_triggers+1);
    p.signalSwitch();
    p.low_fences[1]=low_after_late;
    CHECK(p.submit.wait(low_after_late,&ringclock,0)==kIOReturnSuccess);
    std::puts("PASS switch completion between watchdog and recovery avoids a restart");
}

static void testSwitchTimeoutRecovery(bool reverse)
{
    DualRingPath p;
    p.setup();
    uint32_t uncertain=0,completed=0;
    if(reverse) {
        p.beginReverseSwitch(uncertain,completed);
    } else {
        uncertain=p.enqueue(1,7);
        CHECK((cp.regs[0x08a0]&1) && p.keepalive);
    }

    // This task shares the context of the potentially executed task and could depend on its output.
    const int priority=reverse?0:1;
    const uint32_t same_context=p.enqueue(priority,12);
    CHECK(same_context>uncertain && p.submit.fencePending(same_context));

    // A separate VM gets a new command after STOP was armed. It has not been exposed in either ring's write pointer.
    ArmSmmu4KContextConfig independent_config{};
    Path independent;
    attachProcess(independent,independent_config);
    const NativeAdrenoSubmitBuffer buffer{independent.handle,0x800000,4096};
    uint32_t pending=0;
    p.submit.nextPriority(priority);
    CHECK(p.submit.submit(&independent.vm,&buffer,1,13,&pending,&ringclock)==kIOReturnSuccess);
    CHECK(pending>same_context && p.submit.fencePending(pending));
    CHECK(independent.object.deviceReferences()==2);

    clock_us+=999999;
    bool hung=true;
    CHECK(p.submit.hangCheck(&hung)==kIOReturnSuccess && !hung);
    clock_us+=2;
    CHECK(p.submit.hangCheck(&hung)==kIOReturnSuccess && hung);
    CHECK(p.submit.preemptStats().faulted==1);

    // The CP wrote these fields before reset. Clearing the CPU ring alone
    // would leave the next hardware switch pointed into the old command stream.
    for(auto &record:p.records) {
        record[4]=0x12;
        record[8]=0x34;
        record[20]=0x56;
    }
    NativeAdrenoRecovery result{};
    CHECK(p.recover(result)==kIOReturnSuccess);
    CHECK(p.restart_count==1 && p.restart_fence==pending);
    for(const auto &record:p.records) {
        CHECK(NativeAdrenoPreempt::recordWord(record,0)==0xae399d6eu);
        CHECK(NativeAdrenoPreempt::recordWord(record,4)==0);
        CHECK(NativeAdrenoPreempt::recordWord(record,8)==0);
        CHECK(NativeAdrenoPreempt::recordWord(record,20)==0);
        CHECK(NativeAdrenoPreempt::recordWptr(record)==0);
    }
    CHECK(result.replayed==0 && !result.guilty && p.vm.hangs()==0);
    CHECK(p.submit.outstanding()==0 && p.object.deviceReferences()==1);
    CHECK(independent.object.deviceReferences()==1);
    CHECK(p.submit.wait(uncertain,&ringclock,0)==kIOReturnAborted);
    CHECK(p.submit.observeFence(uncertain,nullptr,0)==kIOReturnAborted);
    CHECK(p.submit.wait(same_context,&ringclock,0)==kIOReturnAborted);
    CHECK(p.submit.wait(pending,&ringclock,0)==kIOReturnAborted);
    CHECK(p.submit.observeFence(pending,nullptr,0)==kIOReturnAborted);
    if(completed)CHECK(p.submit.wait(completed,&ringclock,0)==kIOReturnSuccess);

    const uint32_t fresh=p.enqueue(0,8);
    CHECK(fresh==pending+1 && p.submit.fencePending(fresh));
    fencemem[1]=fresh;
    CHECK(p.submit.wait(fresh,&ringclock,0)==kIOReturnSuccess);
    CHECK(p.submit.wait(uncertain,&ringclock,0)==kIOReturnAborted);
    CHECK(p.submit.wait(pending,&ringclock,0)==kIOReturnAborted);
    CHECK(independent.vm.unmap(independent.handle)==kIOReturnSuccess);
    CHECK(independent.vm.detach()==kIOReturnSuccess);
    CHECK(independent.tables.release()==kIOReturnSuccess);
    std::puts(reverse?"PASS 1->0 timeout aborts outstanding work across VMs and clears saved ring records":
        "PASS 0->1 timeout aborts outstanding work across VMs and clears saved ring records");
}

static void testRecoveryReplaysOnlyUnexposedWork()
{
    DualRingPath p;
    p.setup();
    const uint32_t high_first=p.enqueue(0,8);
    const uint32_t replay_first=p.enqueue(1,7);
    const uint32_t high_second=p.enqueue(0,8);
    const uint32_t replay_second=p.enqueue(1,7);
    CHECK(high_first<replay_first && replay_first<high_second && high_second<replay_second);
    CHECK(!(cp.regs[0x08a0]&1) && p.submit.preemptStats().triggers==0 && !p.keepalive);
    // The current ring completes before the inactive jobs age out. No switch
    // gave either inactive job's write pointer to the CP.
    fencemem[1]=high_second;
    clock_us+=2000001;
    bool hung=false;
    CHECK(p.submit.hangCheck(&hung)==kIOReturnSuccess && hung);
    NativeAdrenoRecovery result{};
    CHECK(p.recover(result)==kIOReturnSuccess);
    CHECK(p.restart_count==1 && p.restart_fence==replay_first-1);
    CHECK(result.replayed==2 && !result.guilty);
    CHECK(p.submit.outstanding()==2 && p.object.deviceReferences()==3);
    CHECK(p.submit.wait(high_second,&ringclock,0)==kIOReturnSuccess);
    CHECK(p.submit.wait(replay_first,&ringclock,0)==kIOReturnTimeout);
    CHECK(p.submit.observeFence(replay_second,nullptr,0)==kIOReturnTimeout);

    fencemem[1]=replay_first;
    CHECK(p.submit.wait(replay_first,&ringclock,0)==kIOReturnSuccess);
    CHECK(p.submit.fencePending(replay_second));
    fencemem[1]=replay_second;
    CHECK(p.submit.wait(replay_second,&ringclock,0)==kIOReturnSuccess);
    CHECK(p.submit.outstanding()==0 && p.object.deviceReferences()==1);
    std::puts("PASS dual-ring recovery preserves completion and replays only never-exposed work");
}

static void testSwitchRestartFailureRetainsReferences()
{
    DualRingPath p;
    p.setup();
    const uint32_t fence=p.enqueue(1,7);
    CHECK(p.submit.fencePending(fence) && p.object.deviceReferences()==2);
    clock_us+=1000001;
    bool hung=false;
    CHECK(p.submit.hangCheck(&hung)==kIOReturnSuccess && hung);

    NativeAdrenoRecovery result{};
    const auto fail_restart=[](void *cookie,NativeAdrenoRing *,uint32_t)->IOReturn {
        ++*static_cast<unsigned *>(cookie);
        return kIOReturnIOError;
    };
    CHECK(p.submit.recover(fail_restart,&p.restart_count,&ringclock,&result)==kIOReturnIOError);
    CHECK(p.restart_count==1 && p.submit.poisoned());
    CHECK(p.submit.outstanding()==1 && p.object.deviceReferences()==2);
    CHECK(p.submit.wait(fence,&ringclock,0)==kIOReturnNotReady);
    std::puts("PASS failed GPU restart retains buffer references and cannot report fence success");
}

static void testStalledWaitDoesNotTouchInactiveRing()
{
    DualRingPath p;
    p.setup();
    const uint32_t high=p.enqueue(0,8);
    const uint32_t low=p.enqueue(1,7);
    CHECK(p.ring.current() && !p.low.current());
    const auto waitWithoutUnstick=[&](uint32_t fence) {
        const size_t writes=cp.writes.size();
        const unsigned smmu_calls=mf.calls;
        CHECK(p.submit.wait(fence,&ringclock,0)==kIOReturnTimeout);
        clock_us+=50000;
        CHECK(p.submit.wait(fence,&ringclock,0)==kIOReturnTimeout);
        clock_us+=150000;
        CHECK(p.submit.wait(fence,&ringclock,0)==kIOReturnTimeout);
        for(size_t i=writes;i<cp.writes.size();++i) {
            CHECK(cp.writes[i]!=0x0807);   // CP_RB_WPTR
            CHECK(cp.writes[i]!=0x3c45);   // GBIF_HALT
        }
        CHECK(mf.calls==smmu_calls);      // No global TLB invalidation.
    };

    // Waiting for the queued low fence cannot disturb current high work,
    // even when the numerically adjacent fence belongs to the high ring.
    CHECK(low==high+1);
    waitWithoutUnstick(low);
    CHECK(p.ring.current() && !p.keepalive);

    clock_us+=100000;
    CHECK(p.submit.retireCompleted()==kIOReturnSuccess && p.keepalive);
    CHECK((cp.regs[0x08a0]&1) && !p.ring.current() && !p.low.current());
    waitWithoutUnstick(low);
    CHECK(p.keepalive && (cp.regs[0x08a0]&1));
    std::puts("PASS waiting on an inactive ring cannot kick or drain the GPU");
}

static void completePrimary(DualRingPath &p,uint32_t fence)
{
    CHECK(p.ring.current() && !p.low.current() && !(cp.regs[0x08a0]&1));
    CHECK(cp.regs[0x0807]==p.ring.writePointer());
    fencemem[0]=cp.regs[0x0806]=p.ring.writePointer();
    fencemem[1]=fence;
    CHECK(p.submit.wait(fence,&ringclock,0)==kIOReturnSuccess);
    CHECK(p.submit.retireCompleted()==kIOReturnSuccess);
}

static void checkFallbackReported(DualRingPath &p)
{
    NativeAdrenoPreemptStats stats{};
    uint32_t loaded=~0u,cntl=~0u,dead=~0u;
    bool flight=true;
    CHECK(p.submit.deadReason()==9);
    CHECK(p.submit.preemptStats(&stats,&loaded,&flight,&cntl,&dead));
    CHECK(dead==9 && loaded==0 && !flight && !(cntl&1));
}

static void checkIdleAfterReset(DualRingPath &p,uint32_t triggers)
{
    CHECK(p.submit.outstanding()==0);
    checkFallbackReported(p);
    for(unsigned tick=0;tick<6;++tick) {
        clock_us+=500000;
        bool hung=true;
        CHECK(p.submit.hangCheck(&hung)==kIOReturnSuccess && !hung);
        CHECK(p.submit.retireCompleted()==kIOReturnSuccess);
        CHECK(p.submit.preemptStats().triggers==triggers);
        CHECK(!(cp.regs[0x08a0]&1) && !p.keepalive);
    }
}

static void testBoardResetKeepsFirstLowWorkOnPrimary(bool reverse)
{
    DualRingPath p;p.setup();
    uint32_t failed=0,finished=0;
    if(reverse)p.beginReverseSwitch(failed,finished);
    else failed=p.enqueue(1,7);
    clock_us+=1000001;
    bool hung=false;
    CHECK(p.submit.hangCheck(&hung)==kIOReturnSuccess && hung);
    NativeAdrenoRecovery result{};
    CHECK(p.recoverBoard(result)==kIOReturnSuccess && !result.completed_late);
    CHECK(result.replayed==0 && p.restart_count==1);
    CHECK(p.ring.writePointer()==12 && cp.regs[0x0807]==12);
    const uint32_t triggers=p.submit.preemptStats().triggers;
    checkIdleAfterReset(p,triggers);
    // A normal-priority queue in mode 2 formerly selected ring 1 here. With
    // the CP parked at 0xc there is no yield to honor a new STOP request.
    const uint32_t fresh=p.enqueue(1,17);
    CHECK(p.ring.lastSubmitted()==fresh && p.low.lastSubmitted()<fresh);
    CHECK(p.ring.current() && !p.low.current() && !(cp.regs[0x08a0]&1));
    CHECK(p.submit.preemptStats().triggers==triggers);
    CHECK(p.ring.writePointer()>12 && cp.regs[0x0807]==p.ring.writePointer());
    CHECK(p.submit.fencePending(fresh));
    completePrimary(p,fresh);
    CHECK(p.submit.wait(failed,&ringclock,0)==kIOReturnAborted);
    CHECK(p.submit.observeFence(failed,nullptr,0)==kIOReturnAborted);
    if(finished)CHECK(p.submit.wait(finished,&ringclock,0)==kIOReturnSuccess);
    checkIdleAfterReset(p,triggers);
    std::puts(reverse?"PASS board 1->0 reset runs first new low work on ring 0 without another STOP":
        "PASS board 0->1 reset runs first new low work on ring 0 without another STOP");
}

static void testBoardResetReplayAndLowQueueContinuation()
{
    DualRingPath p;p.setup();
    const uint32_t high_first=p.enqueue(0,8);
    const uint32_t replay_first=p.enqueue(1,7);
    const uint32_t high_second=p.enqueue(0,8);
    const uint32_t replay_second=p.enqueue(1,7);
    CHECK(high_first<replay_first && replay_first<high_second && high_second<replay_second);
    fencemem[1]=high_second;
    clock_us+=2000001;
    bool hung=false;
    CHECK(p.submit.hangCheck(&hung)==kIOReturnSuccess && hung);
    NativeAdrenoRecovery result{};
    CHECK(p.recoverBoard(result)==kIOReturnSuccess && result.replayed==2);
    checkFallbackReported(p);
    const uint32_t triggers=p.submit.preemptStats().triggers;
    CHECK(p.ring.lastSubmitted()==replay_second && p.submit.fencePending(replay_first));
    CHECK(p.submit.wait(high_second,&ringclock,0)==kIOReturnSuccess);
    const uint32_t same_queue=p.enqueue(1,7);
    CHECK(p.ring.lastSubmitted()==same_queue && p.low.lastSubmitted()<replay_first);
    CHECK(p.submit.preemptStats().triggers==triggers && !(cp.regs[0x08a0]&1));
    completePrimary(p,same_queue);
    CHECK(p.submit.wait(replay_first,&ringclock,0)==kIOReturnSuccess);
    CHECK(p.submit.wait(replay_second,&ringclock,0)==kIOReturnSuccess);
    // Once that queue has drained, neither it nor a different low queue may
    // re-enter ring 1. Moving only the replayed jobs is not sufficient.
    const uint32_t after_drain=p.enqueue(1,7);
    const uint32_t another_queue=p.enqueue(2,31);
    CHECK(p.ring.lastSubmitted()==another_queue && p.low.lastSubmitted()<after_drain);
    CHECK(p.submit.preemptStats().triggers==triggers && !(cp.regs[0x08a0]&1));
    completePrimary(p,another_queue);
    checkIdleAfterReset(p,triggers);
    std::puts("PASS unexposed low work replays with original fences and both same/new low queues stay on ring 0");
}

static void testBoardExecutionResetAndSecondHangKeepFenceHistory()
{
    DualRingPath p;p.setup();
    const uint32_t first_failed=p.enqueue(0,8);
    // This is an execution hang with no switch fault, so fallback must apply
    // to every full reset, not only dead_reason 2 or an in-flight switch.
    bool hung=false;
    for(unsigned tick=0;tick<6 && !hung;++tick) {
        clock_us+=500000;CHECK(p.submit.hangCheck(&hung)==kIOReturnSuccess);
    }
    CHECK(hung && p.submit.preemptStats().triggers==0);
    NativeAdrenoRecovery first{};
    CHECK(p.recoverBoard(first)==kIOReturnSuccess && first.replayed==0);
    checkFallbackReported(p);
    const uint32_t successful=p.enqueue(1,17);
    CHECK(p.ring.lastSubmitted()==successful && !(cp.regs[0x08a0]&1));
    completePrimary(p,successful);
    const uint32_t second_failed=p.enqueue(2,18);
    CHECK(p.ring.lastSubmitted()==second_failed && !(cp.regs[0x08a0]&1));
    hung=false;
    for(unsigned tick=0;tick<6 && !hung;++tick) {
        clock_us+=500000;CHECK(p.submit.hangCheck(&hung)==kIOReturnSuccess);
    }
    CHECK(hung); // Fallback must not turn off execution hang detection.
    NativeAdrenoRecovery second{};
    CHECK(p.recoverBoard(second)==kIOReturnSuccess && second.replayed==0);
    checkFallbackReported(p);
    CHECK(p.restart_count==2 && p.submit.stats().restarts==2);
    CHECK(p.submit.wait(first_failed,&ringclock,0)==kIOReturnAborted);
    CHECK(p.submit.observeFence(second_failed,nullptr,0)==kIOReturnAborted);
    CHECK(p.submit.wait(successful,&ringclock,0)==kIOReturnSuccess);
    const uint32_t fresh=p.enqueue(3,19);
    CHECK(fresh==second_failed+1 && p.ring.lastSubmitted()==fresh);
    completePrimary(p,fresh);
    CHECK(p.submit.wait(first_failed,&ringclock,0)==kIOReturnAborted);
    CHECK(p.submit.wait(second_failed,&ringclock,0)==kIOReturnAborted);
    checkIdleAfterReset(p,p.submit.preemptStats().triggers);
    std::puts("PASS execution reset falls back, second real hang recovers, and failed/successful fence history survives");
}


// These run first: timeout evidence keeps the first two events for the boot lifetime.
static uint32_t expected_timeout[2][NativeAdrenoSwitchTimeoutWords];
static uint32_t expected_evidence[2][NativeAdrenoSwitchEvidenceWords];
static void testInterleavedDiagnosticFences()
{
    DualRingPath p;p.setup();
    uint32_t event_header=0;
    CHECK(AdrenoPM4Type7(0x46,4,&event_header)==AdrenoPM4Success);
    for(unsigned i=0;i<4;++i) {
        const unsigned index=i&1;
        const uint32_t fence=p.enqueue(index,index?7:8);
        CHECK(fence==i+1);
        const volatile uint32_t *ib=native_adreno_ib_log[(native_adreno_ib_log_count-1)&3];
        NativeAdrenoRingLogRow snapshot[1]={};
        CHECK(native_adreno_ring_log_copy(snapshot,1)==1);
        const uint32_t *log=snapshot[0];
        CHECK(ib[0]==fence && log[0]==fence);
        const uint32_t *words=index?p.low_words:ringmem;
        const uint32_t address=index?0x208004:0x108004;
        unsigned found=0;
        for(uint32_t at=log[1],n=0;n<log[3];++n,at=(at+1)&8191) {
            if(words[at]==event_header && words[(at+2)&8191]==address && !words[(at+3)&8191]) {
                CHECK(words[(at+4)&8191]==fence);++found;
            }
        }
        CHECK(found==1); // Check the actual CP fence packet, not just the two diagnostic labels.
    }
    std::puts("PASS interleaved rings: IB/ring labels equal the actual global fence packet");
}
static void testFrozenTimeoutEvidence()
{
    uint32_t copy[NativeAdrenoSwitchTimeoutWords],canary[NativeAdrenoSwitchTimeoutWords];
    for(auto &word:copy)word=0xdeadbeef;
    std::memcpy(canary,copy,sizeof(copy));
    CHECK(!native_adreno_switch_timeout_copy(0,copy,NativeAdrenoSwitchTimeoutWords));
    CHECK(!std::memcmp(copy,canary,sizeof(copy)));
    CHECK(!native_adreno_switch_timeouts);
    uint32_t raw[NativeAdrenoSwitchEvidenceWords];for(auto &word:raw)word=0xaabbccdd;
    CHECK(!native_adreno_switch_evidence_copy(0,raw,NativeAdrenoSwitchEvidenceWords));
    for(auto word:raw)CHECK(word==0xaabbccdd);
    for(unsigned slot=0;slot<2;++slot) {
        DualRingPath p;p.setup();
        uint32_t blocked=0,completed=0;
        if(!slot)p.beginReverseSwitch(blocked,completed);
        else blocked=p.enqueue(1,7);
        CHECK(cp.regs[0x08a0]&1);
        const uint32_t software[6]={p.ring.writePointer(),p.ring.current()?1u:0u,p.ring.restorePending()?1u:0u,
            p.low.writePointer(),p.low.current()?1u:0u,p.low.restorePending()?1u:0u};
        // Hardware samples are distinctive, and do not rely on an all-zero CP peer.
        cp.regs[0x0210]=0x11110000+slot;cp.regs[0x0201]=0x22220000+slot;cp.regs[0x0821]=0x33330000+slot;
        // Both pointers equal: the diagnostic must still run on idle-ring STOP.
        cp.regs[0x0806]=cp.regs[0x0807];
        const uint32_t rptr=cp.regs[0x0806],wptr=cp.regs[0x0807];
        const uint32_t progress[6]={0x3a000+slot,1,0x44000+slot,1,0,0};
        cp.regs[0x0928]=progress[0];cp.regs[0x0929]=progress[1];
        cp.regs[0x092b]=progress[2];cp.regs[0x092c]=progress[3];
        cp.regs[0x092a]=cp.regs[0x092d]=0;
        clock_us+=1000001;bool hung=false;
        CHECK(p.submit.hangCheck(&hung)==kIOReturnSuccess && hung);
        CHECK(native_adreno_switch_timeouts==slot+1);
        CHECK(cp.sqe_calls==1 && p.restart_count==0);
        CHECK(native_adreno_switch_evidence_copy(slot,raw,NativeAdrenoSwitchEvidenceWords)==NativeAdrenoSwitchEvidenceWords);
        NativeAdrenoSwitchEvidence evidence{};std::memcpy(&evidence,raw,sizeof evidence);
        CHECK(evidence.version==1 && evidence.word_count==NativeAdrenoSwitchEvidenceWords && evidence.reason==2);
        CHECK(evidence.sequence==native_adreno_switch_count && evidence.from_ring==(slot?0u:1u) && evidence.to_ring==(slot?1u:0u));
        CHECK(evidence.registers[6]==evidence.registers[7] && (evidence.register_valid_mask&0xc0)==0xc0);
        CHECK(evidence.sqe_status==0 && evidence.sqe.words[0][0]==0 && evidence.sqe.valid_mask[0][0]==1);
        CHECK(evidence.before.valid_mask==15 && evidence.after.valid_mask==15);
        CHECK(evidence.published_tail.hw_wptr==wptr && evidence.published_tail.count==64);
        std::memcpy(expected_evidence[slot],raw,sizeof raw);
        CHECK(native_adreno_switch_timeout_copy(slot,copy,NativeAdrenoSwitchTimeoutWords));
        CHECK(copy[0]==2 && copy[1]==1 && !copy[2] && !copy[3]);
        const uint32_t *e=copy+4;
        CHECK(e[5]==((slot?0x100u:1u)|2u<<16));
        CHECK((e[6]&1) && (e[7]&1));
        CHECK(e[8]==0x11110000+slot && e[9]==0x22220000+slot && e[10]==0x33330000+slot);
        CHECK(e[11]==((rptr&0xffff)|wptr<<16));
        CHECK(!std::memcmp(e+28,progress,sizeof(progress)));
        CHECK(!std::memcmp(copy+38,software,sizeof(software)));
        for(unsigned ring=0;ring<2;++ring) {
            const uint32_t offsets[4]={20,24,8,4};
            for(unsigned k=0;k<4;++k)
                CHECK(e[20+4*ring+k]==NativeAdrenoPreempt::recordWord(p.records[ring],offsets[k]));
        }
        std::memcpy(expected_timeout[slot],copy,sizeof(copy));
        NativeAdrenoRecovery result{};
        if(!slot) {
            cp.regs[0x08a0]&=~1u;
            CHECK(p.recover(result)==kIOReturnSuccess && result.completed_late && !p.restart_count);
        } else {
            CHECK(p.recoverBoard(result)==kIOReturnSuccess && !result.completed_late && p.restart_count==1);
        }
        CHECK(native_adreno_switch_timeout_copy(slot,copy,NativeAdrenoSwitchTimeoutWords));
        CHECK(!std::memcmp(copy,expected_timeout[slot],sizeof(copy)));
        CHECK(native_adreno_switch_evidence_copy(slot,raw,NativeAdrenoSwitchEvidenceWords)==NativeAdrenoSwitchEvidenceWords);
        CHECK(!std::memcmp(raw,expected_evidence[slot],sizeof raw));
    }
    std::puts("PASS V5.3 real Submit timeout captures equal-pointer SQE before reset and preserves immutable evidence");
    std::puts("PASS frozen timeout direction, reason, read status, hardware/record/software state survive late completion and reset");
}
static void testTimeoutEvidenceReadFailure()
{
    DualRingPath p;p.setup();p.enqueue(1,7);
    cp.fail_read_reg=0x0928; // The first progress IB word; also read by stallState.
    cp.diagnostic_callbacks=false;
    clock_us+=1000001;bool hung=false;
    CHECK(p.submit.hangCheck(&hung)==kIOReturnSuccess && hung);
    uint32_t copy[NativeAdrenoSwitchTimeoutWords];
    CHECK(native_adreno_switch_timeout_copy(0,copy,NativeAdrenoSwitchTimeoutWords));
    CHECK(copy[0]==2 && copy[1]==1);
    CHECK(copy[2]==uint32_t(kIOReturnIOError) && copy[3]!=uint32_t(kIOReturnSuccess));
    CHECK(copy[9]==(0x100u|2u<<16));
    uint32_t raw[NativeAdrenoSwitchEvidenceWords];
    CHECK(native_adreno_switch_evidence_copy(0,raw,NativeAdrenoSwitchEvidenceWords)==NativeAdrenoSwitchEvidenceWords);
    NativeAdrenoSwitchEvidence evidence{};std::memcpy(&evidence,raw,sizeof evidence);
    CHECK(evidence.sqe_status==-55 && !evidence.sqe.valid_mask[0][0] && evidence.sqe.words[0][0]==UINT32_MAX);
    CHECK(evidence.sqe.selector_read_status[0]==-1 && !p.restart_count);
    CHECK(!p.ring.poisoned() && !p.low.poisoned());
    std::puts("PASS failed/busy SQE callback is not valid zero and does not suppress timeout handling");
    std::puts("PASS failed progress/stall reads remain explicitly invalid in the captured event");
}
static void checkFrozenTimeoutEvidenceAfterLaterEvents()
{
    uint32_t copy[NativeAdrenoSwitchTimeoutWords],canary[NativeAdrenoSwitchTimeoutWords];
    CHECK(native_adreno_switch_timeouts>2);
    for(unsigned slot=0;slot<2;++slot) {
        CHECK(native_adreno_switch_timeout_copy(slot,copy,NativeAdrenoSwitchTimeoutWords));
        CHECK(!std::memcmp(copy,expected_timeout[slot],sizeof(copy)));
    }
    std::memcpy(canary,copy,sizeof(copy));
    CHECK(!native_adreno_switch_timeout_copy(2,copy,NativeAdrenoSwitchTimeoutWords));
    CHECK(!native_adreno_switch_timeout_copy(0,copy,NativeAdrenoSwitchTimeoutWords-1));
    CHECK(!native_adreno_switch_timeout_copy(0,nullptr,NativeAdrenoSwitchTimeoutWords));
    CHECK(!std::memcmp(copy,canary,sizeof(copy)));
    uint32_t raw[NativeAdrenoSwitchEvidenceWords],before[NativeAdrenoSwitchEvidenceWords];
    for(unsigned slot=0;slot<2;++slot) {
        CHECK(native_adreno_switch_evidence_copy(slot,raw,NativeAdrenoSwitchEvidenceWords)==NativeAdrenoSwitchEvidenceWords);
        CHECK(!std::memcmp(raw,expected_evidence[slot],sizeof raw));
    }
    std::memcpy(before,raw,sizeof raw);
    CHECK(!native_adreno_switch_evidence_copy(2,raw,NativeAdrenoSwitchEvidenceWords));
    CHECK(!native_adreno_switch_evidence_copy(0,raw,NativeAdrenoSwitchEvidenceWords-1));
    CHECK(!native_adreno_switch_evidence_copy(0,nullptr,NativeAdrenoSwitchEvidenceWords));
    CHECK(!std::memcmp(before,raw,sizeof raw));
    std::puts("PASS later timeouts and recovery leave both frozen slots unchanged; rejected reads leave output untouched");
}


/* Software CP completion restores the snapshot WPTR itself. Only later host
 * submissions need a register write. These checks count real driver MMIO calls;
 * they do not model whether duplicate writes make A740 hardware hang. */
static unsigned wptrWrites()
{
    unsigned n=0;for(uint32_t reg:cp.writes)if(reg==0x0807)++n;return n;
}
static void testWptrHandoffWithoutAppend()
{
    DualRingPath p;p.setup();
    const uint32_t job=p.enqueue(1,7);
    CHECK(cp.regs[0x08a0]&1);
    const uint32_t saved=NativeAdrenoPreempt::recordWptr(p.records[1]);
    CHECK(saved==p.low.writePointer());
    const unsigned before=wptrWrites();
    cp.regs[0x0807]=saved; // The hardware restore, not a host MMIO write.
    p.signalSwitch();
    CHECK(p.low.current() && !p.keepalive);
    CHECK(wptrWrites()==before);
    CHECK(!p.low.restorePending());
    CHECK(p.submit.fencePending(job));
    p.low_fences[1]=job;
    CHECK(p.submit.retireCompleted()==kIOReturnSuccess);
    CHECK(p.submit.observeFence(job,nullptr,0)==kIOReturnSuccess);
    std::puts("PASS WPTR handoff: no new submission means no duplicate software WPTR write");
}
static void testWptrHandoffWithAppend()
{
    DualRingPath p;p.setup();
    const uint32_t first=p.enqueue(1,7);
    const uint32_t saved=NativeAdrenoPreempt::recordWptr(p.records[1]);
    const unsigned before=wptrWrites();
    CHECK(!p.low.restorePending());
    const uint32_t second=p.enqueue(1,7),third=p.enqueue(1,7);
    CHECK(third>second && second>first);
    CHECK(saved==NativeAdrenoPreempt::recordWptr(p.records[1]));
    CHECK(p.low.restorePending() && p.low.writePointer()!=saved);
    CHECK(wptrWrites()==before);
    CHECK(p.low.writePreemptWptr(nullptr)==kIOReturnBadArgument);
    CHECK(p.low.restorePending()); // A failed snapshot must not consume new work.
    cp.regs[0x0807]=saved;
    p.signalSwitch();
    CHECK(wptrWrites()==before+1);
    CHECK(cp.regs[0x0807]==p.low.writePointer());
    CHECK(!p.low.restorePending());
    CHECK(!p.submit.preemptIrq() && wptrWrites()==before+1);
    p.low_fences[1]=third;
    CHECK(p.submit.retireCompleted()==kIOReturnSuccess);
    CHECK(p.submit.observeFence(first,nullptr,0)==kIOReturnSuccess);
    CHECK(p.submit.observeFence(second,nullptr,0)==kIOReturnSuccess);
    CHECK(p.submit.observeFence(third,nullptr,0)==kIOReturnSuccess);
    std::puts("PASS WPTR handoff: two in-flight appends publish latest WPTR once; null snapshot preserves pending work");
}

static void testWptrHandoffReverse(bool append)
{
    DualRingPath p;p.setup();
    uint32_t pending=0,done=0;p.beginReverseSwitch(pending,done);
    const uint32_t saved=NativeAdrenoPreempt::recordWptr(p.records[0]);
    const unsigned before=wptrWrites();
    CHECK(!p.ring.restorePending());
    const uint32_t final=append?p.enqueue(0,8):pending;
    CHECK(NativeAdrenoPreempt::recordWptr(p.records[0])==saved);
    CHECK(p.ring.restorePending()==append);
    cp.regs[0x0807]=saved;
    const uint64_t priorSample=native_adreno_wptr_handoff[0][0];
    const uint64_t sampleTime=++clock_us;
    gpu_trace_enabled=true;
    p.signalSwitch();
    gpu_trace_enabled=false;
    CHECK(p.ring.current() && !p.keepalive);
    CHECK(wptrWrites()==before+(append?1u:0u));
    CHECK(cp.regs[0x0807]==p.ring.writePointer());
    CHECK(!p.ring.restorePending());
    if(append) {
        const uint64_t expected[]={sampleTime,pending,final,saved,p.ring.writePointer(),1,kIOReturnSuccess,
            cp.regs[0x0806],cp.regs[0x0807],3,0};
        for(unsigned i=0;i<sizeof(expected)/sizeof(expected[0]);++i)
            CHECK(native_adreno_wptr_handoff[0][i]==expected[i]);
    } else {
        CHECK(native_adreno_wptr_handoff[0][0]==priorSample);
    }
    fencemem[1]=final;
    CHECK(p.submit.retireCompleted()==kIOReturnSuccess);
    CHECK(p.submit.observeFence(pending,nullptr,0)==kIOReturnSuccess);
    CHECK(p.submit.observeFence(final,nullptr,0)==kIOReturnSuccess);
    std::puts(append?"PASS reverse 1->0 WPTR handoff publishes in-flight append once":
        "PASS reverse 1->0 WPTR handoff does not repeat restored pointer");
}

static void testWptrHandoffTriggerFailure()
{
    DualRingPath p;p.setup();
    cp.fail_write_reg=0x08a0;
    const uint32_t failed=p.enqueue(1,7);
    CHECK(p.submit.deadReason()==1 && !p.keepalive);
    CHECK(!(cp.regs[0x08a0]&1));
    CHECK(NativeAdrenoPreempt::recordWptr(p.records[1])==p.low.writePointer());
    CHECK(!p.low.restorePending());
    CHECK(p.submit.fencePending(failed) && p.submit.outstanding()==1);
    bool hung=false;CHECK(p.submit.hangCheck(&hung)==kIOReturnSuccess && hung);
    cp.fail_write_reg=~0u;
    NativeAdrenoRecovery result{};
    CHECK(p.recoverBoard(result)==kIOReturnSuccess);
    CHECK(p.submit.observeFence(failed,nullptr,0)==kIOReturnAborted);
    CHECK(p.submit.outstanding()==0 && !p.vm.unusable());
    const uint32_t resumed=p.enqueue(1,7);
    CHECK(resumed>failed && p.ring.lastSubmitted()==resumed);
    fencemem[1]=resumed;
    CHECK(p.submit.retireCompleted()==kIOReturnSuccess);
    CHECK(p.submit.observeFence(resumed,nullptr,0)==kIOReturnSuccess);
    CHECK(p.submit.observeFence(failed,nullptr,0)==kIOReturnAborted);
    std::puts("PASS failed handoff trigger: pending fence is explicitly aborted by recovery, later work completes");
}

static void testStallDrainStatus()
{
    Path p;
    CHECK(exposed(p)==kIOReturnSuccess);
    const NativeAdrenoSubmitBuffer buffer{p.handle,0x800000,4096};
    struct DrainCase {uint32_t ack;bool read_error,clear_error;IOReturn result;};
    const DrainCase cases[]={
        {0,false,false,kIOReturnTimeout},
        {1,false,false,kIOReturnTimeout},
        {3,true,false,kIOReturnIOError},
        {3,false,false,kIOReturnSuccess},
        {3,false,true,kIOReturnIOError}
    };
    // A failed first CP sample must not masquerade as forward motion.
    uint32_t sampled_fence=0;
    CHECK(p.submit.submit(&p.vm,&buffer,1,7,&sampled_fence,&ringclock)==kIOReturnSuccess);
    cp.fail_read_reg=0x0928;
    clock_us+=1;
    CHECK(p.submit.wait(sampled_fence,&ringclock,0)==kIOReturnTimeout && !p.ring.poisoned());
    cp.fail_read_reg=UINT32_MAX;
    cp.regs[0x0806]=0x12345u;cp.regs[0x0807]=0x23456u;
    clock_us+=50000;
    CHECK(p.submit.wait(sampled_fence,&ringclock,0)==kIOReturnTimeout);
    CHECK(native_adreno_stall_kick[20]==(1u<<8));
    const uint32_t last=native_adreno_stall_log_count;
    uint32_t pointers[6]={};
    CHECK(native_adreno_stall_pointers_copy(last-2,pointers));
    CHECK(pointers[0]==sampled_fence && pointers[1]==4 && pointers[2]==0x12345u && pointers[3]==0x23456u);
    CHECK((native_adreno_stall_log[(last-2)&31][4]&0xffffffffu)==0x34562345u);
    CHECK(!native_adreno_stall_pointers_copy(last,pointers) && pointers[2]==0x12345u);
    cp.regs[0x0806]=0;
    fencemem[1]=sampled_fence;
    CHECK(p.submit.wait(sampled_fence,&ringclock,0)==kIOReturnSuccess);
    for(const auto &test:cases) {
        uint32_t fence=0;
        CHECK(p.submit.submit(&p.vm,&buffer,1,7,&fence,&ringclock)==kIOReturnSuccess);
        clock_us+=1;
        CHECK(p.submit.wait(fence,&ringclock,0)==kIOReturnTimeout);
        ++cp.regs[0x0928];
        clock_us+=50000;
        CHECK(p.submit.wait(fence,&ringclock,0)==kIOReturnTimeout);
        CHECK(native_adreno_stall_kick[20]==1);
        const uint32_t attempts=native_adreno_unstick[4],drained=native_adreno_unstick[11];
        cp.regs[0x3c46]=test.ack;
        cp.fail_read_reg=test.read_error?0x3c46:~0u;
        cp.fail_halt_clear=test.clear_error;
        clock_us+=150000;
        const IOReturn status=p.submit.wait(fence,&ringclock,0);
        CHECK((native_adreno_stall_log[(native_adreno_stall_log_count-1)&31][2]&0xff)==(test.result?5u:3u));
        CHECK(native_adreno_unstick[14]==static_cast<uint32_t>(test.result));
        CHECK(status==(test.result==kIOReturnIOError?kIOReturnIOError:kIOReturnTimeout));
        CHECK(native_adreno_unstick[11]==drained);
        CHECK(p.object.deviceReferences()==2 && native_adreno_unstick[4]==attempts+1);
        if(test.clear_error) {
            CHECK(cp.regs[0x3c45]==3 && p.ring.poisoned() && p.submit.poisoned());
            const size_t writes=cp.writes.size();
            fencemem[1]=fence;
            CHECK(p.submit.wait(fence,&ringclock,0)==kIOReturnNotReady);
            CHECK(p.submit.observeFence(fence,&ringclock,0)==kIOReturnNotReady);
            uint32_t refused=~0u;
            CHECK(p.submit.submit(&buffer,1,7,&refused)==kIOReturnNotReady && !refused);
            CHECK(p.object.deviceReferences()==2 && cp.writes.size()==writes);
            bool hung=false;
            CHECK(p.submit.hangCheck(&hung)==kIOReturnSuccess && hung);
            CHECK(cp.writes.size()==writes);
            fencemem[1]=fence-1;  // Keep the stalled fence unresolved at reset.
            NativeAdrenoRecovery recovered{};
            CHECK(p.submit.recover([](void *,NativeAdrenoRing *target,uint32_t completed) {
                CHECK(cp.regs[0x3c45]==3);
                cp.fail_halt_clear=false;
                cp.regs[0x3c45]=0;
                return target->restart(completed);
            },nullptr,&ringclock,&recovered)==kIOReturnSuccess);
            CHECK(!recovered.guilty && !recovered.replayed && !p.submit.poisoned() && !p.ring.poisoned());
            CHECK(p.submit.wait(fence,&ringclock,0)==kIOReturnAborted && p.object.deviceReferences()==1);
            continue;
        }
        CHECK(p.submit.fencePending(fence) && cp.regs[0x3c45]==0);
        clock_us+=1000;
        CHECK(p.submit.wait(fence,&ringclock,0)==kIOReturnTimeout);
        CHECK(native_adreno_unstick[4]==attempts+1);
        fencemem[1]=fence;
        CHECK(p.submit.wait(fence,&ringclock,0)==kIOReturnSuccess && p.object.deviceReferences()==1);
    }
    std::puts("PASS stall drain preserves progress after an ack failure and stops submissions if halt release fails");
}

static void testDualRingHaltRecovery(bool reset_ok)
{
    DualRingPath p;p.setup();
    // Both fixture instances use the same fence numbers on a fresh attach.
    const uint32_t done=p.enqueue(0,8);
    fencemem[1]=done;
    CHECK(p.submit.retireCompleted()==kIOReturnSuccess);
    const uint32_t high=p.enqueue(0,8);
    const uint32_t low=p.enqueue(1,7);
    CHECK(p.submit.oldestFenceInRange(7,1)==low);
    CHECK(p.submit.oldestFenceInRange(8,1)==high);
    CHECK(p.submit.oldestFenceInRange(7,2)==high);
    CHECK(!p.submit.oldestFenceInRange(7,0) && !p.submit.oldestFenceInRange(9,1));
    CHECK(p.ring.current() && !p.low.current());
    CHECK(p.submit.wait(high,&ringclock,0)==kIOReturnTimeout);
    ++cp.regs[0x0928];clock_us+=50000;
    CHECK(p.submit.wait(high,&ringclock,0)==kIOReturnTimeout);
    cp.regs[0x3c46]=3;cp.fail_halt_clear=true;clock_us+=150000;
    CHECK(p.submit.wait(high,&ringclock,0)==kIOReturnIOError);
    CHECK(p.ring.poisoned() && !p.low.poisoned() && p.submit.poisoned());
    CHECK(p.submit.outstanding()==2 && p.object.deviceReferences()==3);

    const auto writes=cp.writes.size();
    const NativeAdrenoSubmitBuffer buffer{p.handle,0x800000,4096};
    uint32_t refused=~0u;
    p.submit.nextPriority(1);
    CHECK(p.submit.submit(&p.vm,&buffer,1,7,&refused,&ringclock)==kIOReturnNotReady && !refused);
    CHECK(p.submit.wait(low,&ringclock,0)==kIOReturnNotReady);
    CHECK(p.submit.retireCompleted()==kIOReturnNotReady);
    CHECK(!p.submit.preemptIrq());
    bool hung=false;
    CHECK(p.submit.hangCheck(&hung)==kIOReturnSuccess && hung);
    CHECK(cp.writes.size()==writes && p.object.deviceReferences()==3);

    NativeAdrenoRecovery outcome{};
    if(reset_ok) {
        cp.fail_halt_clear=false;
        CHECK(p.recoverBoard(outcome)==kIOReturnSuccess && p.restart_count==1);
        CHECK(!outcome.completed_late && !outcome.replayed && !p.submit.poisoned());
        CHECK(p.submit.wait(high,&ringclock,0)==kIOReturnAborted);
        CHECK(p.submit.wait(low,&ringclock,0)==kIOReturnAborted);
        CHECK(p.object.deviceReferences()==1);
        const uint32_t resumed=p.enqueue(1,7);
        CHECK(resumed>low && p.ring.lastSubmitted()==resumed);
        fencemem[1]=resumed;
        CHECK(p.submit.retireCompleted()==kIOReturnSuccess);
        CHECK(p.submit.wait(resumed,&ringclock,0)==kIOReturnSuccess && p.object.deviceReferences()==1);
    } else {
        const auto failed=[](void *cookie,NativeAdrenoRing *,uint32_t)->IOReturn {
            ++static_cast<DualRingPath *>(cookie)->restart_count;
            return kIOReturnTimeout;
        };
        CHECK(p.submit.recover(failed,&p,&ringclock,&outcome)==kIOReturnIOError);
        CHECK(outcome.status==kIOReturnTimeout && p.restart_count==1);
        CHECK(p.submit.hangCheck(&hung)==kIOReturnNotReady && !hung);
        CHECK(p.submit.recover(failed,&p,&ringclock,&outcome)==kIOReturnNotReady && p.restart_count==1);
        CHECK(p.submit.outstanding()==2 && p.object.deviceReferences()==3 && cp.writes.size()==writes);
    }
    std::puts(reset_ok?"PASS shared halt failure stops both rings until reset, aborts old fences and resumes":
        "PASS failed shared halt reset retains both jobs and cannot loop");
}

static void testCpuTableRecovery(bool complete)
{
    ArmSmmu4KContextConfig process_config{};
    Path primary,process;
    CHECK(exposed(primary)==kIOReturnSuccess);
    attachProcess(process,process_config);
    const uint64_t kernel_table=primary.vm.ttbr0(),process_table=process.vm.ttbr0();
    CHECK(kernel_table!=process_table);
    struct Replay {
        NativeAdrenoRing &ring;
        bool complete;
        unsigned pauses=0;
        uint64_t us=0;
        std::vector<std::pair<uint64_t,uint32_t>> tables;
    } replay{primary.ring,complete};
    const auto write=[](void *cookie,uint64_t table)->IOReturn {
        auto &model=*static_cast<Replay *>(cookie);
        model.tables.emplace_back(table,fencemem[1]);
        return kIOReturnSuccess;
    };
    CHECK(primary.submit.setTableSwitching(1,write,&replay)==kIOReturnSuccess);
    const NativeAdrenoSubmitBuffer buffer{process.handle,0x800000,4096};
    uint32_t fence=0;
    CHECK(primary.submit.submit(&process.vm,&buffer,1,7,&fence,&ringclock)==0 && fence==1);
    CHECK(primary.submit.submit(&process.vm,&buffer,1,7,&fence,&ringclock)==0 && fence==2);
    // A ring-owned packet has no submit Job. Recovery fills its fence with a kernel-table NOP.
    uint32_t nop=0;
    CHECK(AdrenoPM4Type7(0x10,0,&nop)==AdrenoPM4Success);
    CHECK(primary.ring.submit(&nop,1,&fence)==0 && fence==3);
    CHECK(primary.submit.submit(&process.vm,&buffer,1,7,&fence,&ringclock)==0 && fence==4);
    CHECK(process.object.deviceReferences()==4);
    replay.tables.clear();

    const NativeAdrenoRingClock clock{&replay,
        [](void *cookie) {return static_cast<Replay *>(cookie)->us;},
        [](void *cookie)->int {
            auto &model=*static_cast<Replay *>(cookie);
            CHECK(model.ring.lastSubmitted()==2 || model.ring.lastSubmitted()==3);
            ++model.pauses;
            model.us+=100000;
            if(model.complete && model.pauses%3==0)fencemem[1]=model.ring.lastSubmitted();
            return 0;
        }};
    const auto restart=[](void *,NativeAdrenoRing *ring,uint32_t completed)->IOReturn {
        CHECK(completed==1);return ring->restart(completed);
    };
    NativeAdrenoRecovery result{};
    const IOReturn status=primary.submit.recover(restart,nullptr,&clock,&result);
    if(complete) {
        CHECK(status==kIOReturnSuccess && result.replayed==3);
        CHECK(primary.submit.wait(1,&clock,0)==kIOReturnAborted);
        CHECK(primary.submit.observeFence(1,&clock,0)==kIOReturnAborted);
        const std::vector<std::pair<uint64_t,uint32_t>> expected{
            {kernel_table,1},{process_table,1},{kernel_table,2},{process_table,3}};
        CHECK(replay.tables==expected && replay.pauses==6);
        CHECK(primary.ring.lastSubmitted()==4 && primary.submit.fencePending(4));
        CHECK(process.object.deviceReferences()==3 && primary.submit.stats().references_retired==1);
        CHECK(primary.submit.retireCompleted()==0 && process.object.deviceReferences()==2);
        fencemem[1]=4;
        CHECK(primary.submit.wait(4,&clock,0)==0 && process.object.deviceReferences()==1);
        CHECK(primary.submit.wait(4,&clock,0)==0 && primary.submit.stats().references_retired==3);
        CHECK(primary.submit.outstanding()==0 && primary.submit.stats().restarts==1);
        std::puts("PASS CPU-table replay waits only for already-issued fences and preserves IDs and references");
    } else {
        CHECK(status==kIOReturnIOError && result.status==kIOReturnTimeout && result.replayed==1);
        CHECK(replay.tables.size()==2 && replay.tables.back().first==process_table && replay.pauses==20);
        CHECK(primary.ring.lastSubmitted()==2 && primary.submit.poisoned());
        CHECK(process.object.deviceReferences()==3 && primary.submit.stats().references_retired==1);
        CHECK(primary.submit.stats().restarts==0);
        std::puts("PASS CPU-table replay timeout preserves the live table and unresolved references");
    }
}

static void testForgetUnusedVm()
{
    Path primary,process;
    ArmSmmu4KContextConfig process_config{};
    CHECK(exposed(primary)==kIOReturnSuccess);
    attachProcess(process,process_config);
    uint64_t installed=0;
    const auto write=[](void *cookie,uint64_t ttbr0)->IOReturn {
        *static_cast<uint64_t *>(cookie)=ttbr0;
        return kIOReturnSuccess;
    };
    CHECK(primary.submit.setTableSwitching(2,write,&installed)==kIOReturnSuccess);
    const NativeAdrenoSubmitBuffer kernelJob{primary.handle,0x800000,4096};
    uint32_t pending=0;
    CHECK(primary.submit.submit(&primary.vm,&kernelJob,1,8,&pending,&ringclock)==kIOReturnSuccess);
    const size_t writes=cp.writes.size();
    const uint64_t before=clock_us;
    CHECK(primary.submit.forgetVm(&process.vm,&ringclock,10000)==kIOReturnSuccess);
    CHECK(clock_us==before && cp.writes.size()==writes && !installed);
    CHECK(primary.submit.fencePending(pending) && primary.object.deviceReferences()==2);

    fencemem[1]=pending;
    CHECK(primary.submit.retireCompleted()==kIOReturnSuccess);
    const NativeAdrenoSubmitBuffer processJob{process.handle,0x800000,4096};
    CHECK(primary.submit.submit(&process.vm,&processJob,1,7,&pending,&ringclock)==kIOReturnSuccess);
    CHECK(primary.submit.forgetVm(&process.vm,&ringclock,1000)==kIOReturnTimeout && !installed);
    CHECK(primary.submit.fencePending(pending) && process.object.deviceReferences()==2);
    fencemem[1]=pending;
    cp.regs[0x0806]=primary.ring.writePointer();
    CHECK(primary.submit.forgetVm(&process.vm,&ringclock,1000)==kIOReturnSuccess);
    CHECK(installed==primary.vm.ttbr0() && process.object.deviceReferences()==1);
    std::puts("PASS unused VM skips unrelated GPU work; used VM still waits and restores the kernel table");
}

struct TableWriteProbe {
    uint64_t ttbr0=0,hardware=0;
    unsigned calls=0,reads=0;
};

// The ring has written its fence but has not executed the final yield packet.
static constexpr uint32_t ContextSwitchYieldDwords=5;

static void installAndRetireProcessTable(DualRingPath &path,Path &process,
    ArmSmmu4KContextConfig &config,TableWriteProbe &write,bool leaveTail=false)
{
    path.setup();
    const auto record=[](void *cookie,uint64_t ttbr0)->IOReturn {
        auto &probe=*static_cast<TableWriteProbe *>(cookie);
        ++probe.calls;
        probe.hardware=probe.ttbr0=ttbr0;
        return kIOReturnSuccess;
    };
    const auto read=[](void *cookie,uint64_t *ttbr0)->IOReturn {
        auto &probe=*static_cast<TableWriteProbe *>(cookie);
        ++probe.reads;
        *ttbr0=probe.hardware;
        return kIOReturnSuccess;
    };
    CHECK(path.submit.setTableSwitching(2,record,&write,read)==kIOReturnSuccess);
    attachProcess(process,config);
    const NativeAdrenoSubmitBuffer buffer{process.handle,0x800000,4096};
    uint32_t fence=0;
    path.submit.nextPriority(0);
    CHECK(path.submit.submit(&process.vm,&buffer,1,8,&fence,&ringclock)==kIOReturnSuccess);
    fencemem[0]=path.ring.writePointer()-(leaveTail?ContextSwitchYieldDwords:0);
    fencemem[1]=fence;
    // The host peer models the CP's TTBR0 writeback before its fence.
    write.hardware=process.vm.ttbr0();
    CHECK(path.ring.resetExecutedTable(process.vm.ttbr0())==kIOReturnSuccess);
    CHECK(path.submit.retireCompleted()==kIOReturnSuccess && !path.submit.outstanding());
    CHECK(process.object.deviceReferences()==1 && !write.calls);
}

static void testForgetDisplacedVm()
{
    DualRingPath path;
    Path process;
    ArmSmmu4KContextConfig config{};
    TableWriteProbe write;
    installAndRetireProcessTable(path,process,config,write);

    const uint32_t other=path.enqueue(0,9);
    // The new command's prologue switches to the kernel table. Its fence
    // is still pending, so draining all jobs would time out.
    CHECK(path.ring.resetExecutedTable(path.vm.ttbr0())==kIOReturnSuccess);
    const uint64_t began=clock_us;
    CHECK(path.submit.fencePending(other) && path.object.deviceReferences()==2);
    // The software shadows alone are insufficient while the real bank
    // still names the retiring VM.
    CHECK(path.submit.forgetVm(&process.vm,&ringclock,0)==kIOReturnTimeout);
    CHECK(clock_us==began && !write.calls && path.submit.fencePending(other));
    write.hardware=path.vm.ttbr0();
    const uint32_t low=path.enqueue(1,10);
    clock_us+=300000;   // Force a switch while both unrelated fences remain pending.
    CHECK(path.submit.retireCompleted()==kIOReturnSuccess && path.keepalive);
    CHECK(cp.regs[0x08a0]&1);
    const unsigned readsBeforeSwitch=write.reads;
    CHECK(path.submit.forgetVm(&process.vm,&ringclock,0)==kIOReturnTimeout);
    CHECK(!write.calls && write.reads==readsBeforeSwitch);
    path.signalSwitch();

    const uint64_t resumed=clock_us;
    const unsigned reads=write.reads;
    CHECK(path.submit.forgetVm(&process.vm,&ringclock,3000)==kIOReturnSuccess);
    CHECK(clock_us==resumed && !write.calls && write.reads>reads);
    CHECK(path.submit.fencePending(other) && path.submit.fencePending(low));
    CHECK(path.object.deviceReferences()==3 && process.object.deviceReferences()==1);
    std::puts("PASS mode-2 teardown leaves unrelated work pending and refuses an in-flight ring switch");
}

static void testForgetUnreplacedVm()
{
    DualRingPath path;
    Path process;
    ArmSmmu4KContextConfig config{};
    TableWriteProbe write;
    installAndRetireProcessTable(path,process,config,write);

    // Fill the ring with raw NOPs. The replacement command is refused
    // before the CP can execute its page-table switch.
    uint32_t nop=0;
    CHECK(AdrenoPM4Type7(0x10,0,&nop)==AdrenoPM4Success);
    const std::vector<uint32_t> raw(8181,nop);
    CHECK(path.ring.submitUnfenced(raw.data(),raw.size())==kIOReturnSuccess);
    const NativeAdrenoSubmitBuffer kernel{path.handle,0x800000,4096};
    uint32_t refused=0;
    path.submit.nextPriority(0);
    CHECK(path.submit.submit(&path.vm,&kernel,1,9,&refused,&ringclock)==kIOReturnBusy && !refused);
    CHECK(!path.submit.outstanding() && path.object.deviceReferences()==1);
    CHECK(path.ring.tablePending() && path.ring.table()==path.vm.ttbr0());
    uint64_t executed=0;
    CHECK(path.ring.executedTable(&executed)==kIOReturnSuccess && executed==process.vm.ttbr0());
    CHECK(fencemem[0]!=path.ring.writePointer());
    for(auto &saved:path.smmu_info)
        CHECK(NativeAdrenoPreempt::writeSmmuInfo(saved,process.vm.ttbr0(),uint32_t(process.vm.ttbr0()>>48),0));
    const auto checkStillInUse=[&] {
        CHECK(path.submit.forgetVm(&process.vm,&ringclock,0)==kIOReturnTimeout);
        CHECK(!write.calls && !path.submit.poisoned() && process.object.deviceReferences()==1);
        CHECK(path.ring.executedTable(&executed)==kIOReturnSuccess && executed==process.vm.ttbr0());
    };
    checkStillInUse();   // No tracked jobs, but the hardware RPTR has not caught up.
    fencemem[0]=path.ring.writePointer();
    cp.regs[0x0210]=1u<<23;   // RPTR caught up, but RBBM_STATUS still reports GPU busy.
    checkStillInUse();
    cp.regs[0x0210]=0;

    CHECK(path.submit.forgetVm(&process.vm,&ringclock,1000)==kIOReturnSuccess);
    CHECK(write.calls==1 && write.ttbr0==path.vm.ttbr0() && write.hardware==path.vm.ttbr0());
    CHECK(!path.ring.tablePending() && path.ring.table()==path.vm.ttbr0());
    CHECK(path.ring.executedTable(&executed)==kIOReturnSuccess && executed==path.vm.ttbr0());
    CHECK(path.low.executedTable(&executed)==kIOReturnSuccess && executed==path.vm.ttbr0());
    for(const auto &saved:path.smmu_info) {
        uint64_t ttbr0=0;
        std::memcpy(&ttbr0,saved+8,sizeof(ttbr0));
        CHECK(ttbr0==path.vm.ttbr0());
    }
    std::puts("PASS mode-2 teardown restores the executed page table only after the ring and GPU are idle");
}

static void testForgetInactiveTail(bool highTail)
{
    DualRingPath path;
    Path process;
    ArmSmmu4KContextConfig config{};
    TableWriteProbe write;
    installAndRetireProcessTable(path,process,config,write,highTail);

    const uint32_t low=path.enqueue(1,9);
    path.signalSwitch();
    path.low_fences[0]=path.low.writePointer()-(highTail?0:ContextSwitchYieldDwords);
    path.low_fences[1]=low;
    if(!highTail) {
        const NativeAdrenoSubmitBuffer buffer{process.handle,0x800000,4096};
        uint32_t high=0;
        path.submit.nextPriority(0);
        CHECK(path.submit.submit(&process.vm,&buffer,1,8,&high,&ringclock)==kIOReturnSuccess);
        path.signalSwitch();
        fencemem[0]=path.ring.writePointer();
        fencemem[1]=high;
    }
    CHECK(path.submit.retireCompleted()==kIOReturnSuccess && !path.submit.outstanding());
    auto &target=highTail?path.ring:path.low;
    auto &rptr=highTail?fencemem[0]:path.low_fences[0];
    CHECK(!target.current() && target.commandsConsumed()==kIOReturnTimeout);
    CHECK(!(cp.regs[0x08a0]&1) && !write.calls);

    const unsigned targetIndex=highTail?0u:1u;
    const uint32_t resumes=native_adreno_vm_tail_resume[targetIndex];
    const uint32_t otherResumes=native_adreno_vm_tail_resume[1-targetIndex];
    // A close must leave the current ring alone until the CP is idle.
    cp.regs[0x0210]=1u<<23;
    CHECK(path.submit.forgetVm(&process.vm,&ringclock,0)==kIOReturnTimeout);
    CHECK(!(cp.regs[0x08a0]&1) && !write.calls);
    cp.regs[0x0210]=0;

    // There are no jobs left to trigger the normal scheduler.
    CHECK(path.submit.forgetVm(&process.vm,&ringclock,0)==kIOReturnTimeout);
    CHECK((cp.regs[0x08a0]&1) && path.keepalive && !write.calls);
    CHECK(cp.regs[0x08a1]==(highTail?0x400000u:0x600000u));
    const size_t switchWrites=cp.writes.size();
    CHECK(path.submit.forgetVm(&process.vm,&ringclock,0)==kIOReturnTimeout);
    CHECK(cp.writes.size()==switchWrites && !write.calls);

    path.signalSwitch();
    CHECK(target.current() && !path.keepalive);
    CHECK(path.submit.forgetVm(&process.vm,&ringclock,0)==kIOReturnTimeout);
    CHECK(!(cp.regs[0x08a0]&1) && !write.calls);
    rptr=target.writePointer();
    CHECK(path.submit.forgetVm(&process.vm,&ringclock,0)==kIOReturnSuccess);
    CHECK(write.calls==1 && write.ttbr0==path.vm.ttbr0());
    CHECK(native_adreno_vm_tail_resume[targetIndex]==resumes+1);
    CHECK(native_adreno_vm_tail_resume[1-targetIndex]==otherResumes);
    std::printf("PASS VM close resumes fenced %s ring tail before restoring TTBR0\n",highTail?"high":"low");
}

int main(int argc,char **argv)
{
    if(argc==3 && !std::strcmp(argv[1],"--forget-inactive-tail")) {
        testForgetInactiveTail(!std::strcmp(argv[2],"high"));return 0;
    }
    if(argc==2 && !std::strcmp(argv[1],"--forget-displaced-vm")) {
        testForgetDisplacedVm();return 0;
    }
    if(argc==2 && !std::strcmp(argv[1],"--forget-unreplaced-vm")) {
        testForgetUnreplacedVm();return 0;
    }
    if(argc==2 && !std::strcmp(argv[1],"--forget-vm")) {
        testForgetUnusedVm();return 0;
    }
    if(argc==2 && !std::strcmp(argv[1],"--replay-cpu-table")) {
        testCpuTableRecovery(true);testCpuTableRecovery(false);return 0;
    }
    if(argc==2 && !std::strcmp(argv[1],"--stall-drain")) {
        testStallDrainStatus();testDualRingHaltRecovery(true);testDualRingHaltRecovery(false);return 0;
    }
    if(argc==2 && !std::strcmp(argv[1],"--wptr-handoff")) {
        testWptrHandoffWithoutAppend();
        testWptrHandoffWithAppend();
        testWptrHandoffReverse(false);testWptrHandoffReverse(true);
        testWptrHandoffTriggerFailure();return 0;
    }
    if(argc==2 && !std::strcmp(argv[1],"--timeout-read-error")) {
        testTimeoutEvidenceReadFailure();return 0;
    }
    if(argc==2 && !std::strcmp(argv[1],"--timeout-evidence")) {
        testInterleavedDiagnosticFences();
        testFrozenTimeoutEvidence();
        // Wrap the 16-entry live event ring, then cause additional timeouts.
        for(unsigned i=0;i<20;++i)testSwitchCompletesWithoutInterrupt();
        testSwitchTimeoutRecovery(true);
        testSwitchTimeoutRecovery(false);
        checkFrozenTimeoutEvidenceAfterLaterEvents();
        return 0;
    }
    /* One validated indirect-buffer submission with exact ring bytes. */
    {
        Path p;CHECK(exposed(p)==kIOReturnSuccess);
        NativeAdrenoSubmitBuffer buffer{p.handle,0x800000,4096};
        uint32_t fence=0;
        /* Before the ring: clean+invalidate of the one mapped BO and a coherence sample of the IB's first word; no TLBIALL (a map fills only
         * invalid leaves and an unmap drops its own pages). */
        static_cast<volatile uint32_t *>(p.object.mapping())[0]=0x70970001u;
        const unsigned cache_calls=fixture.cache_calls;
        mmio[(CB+0x618)/4]=0xffffffffu;
        CHECK(p.submit.submit(&buffer,1,0,&fence)==kIOReturnSuccess && fence==1);
        CHECK(fixture.cache_calls==cache_calls+1 && mmio[(CB+0x618)/4]==0xffffffffu);
        CHECK(fixture.cache_operations.back()==NativeAdrenoCleanInvalidate);
        CHECK(native_adreno_coherence[0]==1 && native_adreno_coherence[1]==0x70970001u &&
            native_adreno_coherence[2]==~0u && native_adreno_coherence[3]==~0u);   /* no board DRAM reader installed */
        CHECK(cp.writes.back()==0x0807 && cp.regs[0x0807]==11+4+31);
        CHECK((ringmem[11]>>16&0x7fu)==0x3f && (ringmem[11]&0x3fffu)==3);
        CHECK(ringmem[12]==0x800000 && ringmem[13]==0 && ringmem[14]==1024);
        CHECK(p.object.deviceReferences()==2); /* mapping + job */
        CHECK(p.submit.outstanding()==1 && p.submit.stats().references_held==1);
        CHECK(p.submit.detach()==kIOReturnBusy);
        uint32_t observed=0;
        CHECK(p.ring.wait(1,&ringclock,1000,&observed)==NativeAdrenoRingFenceTimeout);
        CHECK(p.submit.wait(1,&ringclock,1000)==kIOReturnTimeout && p.submit.stats().timeouts==1);
        fencemem[1]=1;
        CHECK(p.submit.wait(1,&ringclock,1000)==kIOReturnSuccess);
        CHECK(p.submit.outstanding()==0 && p.submit.stats().completed==1);
        CHECK(p.object.deviceReferences()==1 && p.submit.stats().references_retired==1);
        CHECK(p.submit.wait(1,&ringclock,1000)==kIOReturnSuccess); /* retired job: the ring says the CP passed it */
        CHECK(p.submit.wait(2,&ringclock,1000)==kIOReturnBadArgument); /* never submitted */
        /* The coherence sample runs on every 64th submission only (the first was sampled above). */
        CHECK(p.submit.submit(&buffer,1,0,&fence)==kIOReturnSuccess && fence==2 && native_adreno_coherence[0]==1);
        fencemem[1]=2;
        CHECK(p.submit.wait(2,&ringclock,1000)==kIOReturnSuccess);
        CHECK(p.submit.detach()==kIOReturnSuccess);
    }
    {
        const unsigned imports=fixture.imports_live;
        Path p;CHECK(exposed(p)==kIOReturnSuccess);
        NativeAdrenoMemory imported;
        fixture.write_protected_imports=true;
        CHECK(imported.importTask(reinterpret_cast<void *>(0x77),0x20000000,4096,40,4096)==kIOReturnSuccess);
        fixture.write_protected_imports=false;
        CHECK(imported.readOnly());
        const unsigned mmioCalls=mf.calls;
        uint64_t handle=0;
        CHECK(p.vm.map(&imported,0,0x808000,ArmSmmu4KRead|ArmSmmu4KWrite,0,2,&handle)==kIOReturnBadArgument && !handle);
        CHECK(mf.calls==mmioCalls && imported.deviceReferences()==0);
        CHECK(p.vm.map(&p.object,0,0x808000,ArmSmmu4KRead|ArmSmmu4KWrite,1,3,&handle)==kIOReturnBadArgument && !handle);
        CHECK(p.vm.map(&imported,0,0x808000,ArmSmmu4KRead,0,2,&handle)==kIOReturnSuccess && handle);
        CHECK(p.vm.unmap(handle)==kIOReturnSuccess);
        CHECK(imported.release()==kIOReturnSuccess && fixture.imports_live==imports);
        CHECK(p.submit.detach()==kIOReturnSuccess);
        CHECK(p.vm.unmap(p.handle)==kIOReturnSuccess);
        CHECK(p.object.release()==kIOReturnSuccess);
        CHECK(p.vm.detach()==kIOReturnSuccess);
        CHECK(p.tables.release()==kIOReturnSuccess);
        std::puts("PASS read-only GPU mappings and rejection of incompatible aliases");
    }
    {
        Path p;CHECK(exposed(p)==kIOReturnSuccess);
        NativeAdrenoMemory buffer;
        CHECK(buffer.allocate(4096,40,false,4096)==kIOReturnSuccess);
        uint64_t handle=0;
        fixture.cache_fail_call=fixture.cache_calls+1;
        CHECK(p.vm.map(&buffer,0,0x808000,ArmSmmu4KRead|ArmSmmu4KWrite,0,2,&handle)==kIOReturnIOError);
        CHECK(!handle && !buffer.deviceReferences() && !buffer.quarantined() && !p.vm.poisoned());
        fixture.cache_fail_call=0;
        CHECK(p.vm.map(&buffer,0,0x808000,ArmSmmu4KRead|ArmSmmu4KWrite,0,2,&handle)==0 && handle);
        CHECK(p.vm.unmap(handle)==0 && buffer.release()==0);
        CHECK(p.submit.detach()==0 && p.vm.unmap(p.handle)==0 && p.object.release()==0);
        CHECK(p.vm.detach()==0 && p.tables.release()==0);
        std::puts("PASS cache failure before GPU mapping drops its reference and allows retry");
    }
    {
        for(unsigned fault=0;fault<2;++fault) {
            Path p;CHECK(exposed(p)==kIOReturnSuccess);
            NativeAdrenoMemory buffer;
            const auto live=fixture.live.size(),metadata=fixture.metadata.size();
            if(fault==0)fixture.allocation_fail=true;else fixture.prepare_fail=true;
            CHECK(buffer.allocate(4096,40,false,4096)!=kIOReturnSuccess);
            fixture.allocation_fail=fixture.prepare_fail=false;
            CHECK(!buffer.backed() && fixture.live.size()==live && fixture.metadata.size()==metadata);
            CHECK(p.submit.detach()==0 && p.vm.unmap(p.handle)==0 && p.object.release()==0);
            CHECK(p.vm.detach()==0 && p.tables.release()==0);
        }
        std::puts("PASS allocation and prepare failures do not retain a BO");
    }
    {
        NativeAdrenoMemory imported;
        CHECK(imported.importTask(reinterpret_cast<void *>(0x77),0x20000000,5*4096,40,5*4096)==0);
        CHECK(imported.spanCount()==5);
        fixture.physical_operations.clear();
        CHECK(imported.publishForDevice(4096,2*4096,false,false)==0);
        CHECK(fixture.physical_operations.size()==2);
        CHECK(fixture.physical_operations[0].physical==imported.spans()[1].physical);
        CHECK(fixture.physical_operations[1].physical==imported.spans()[2].physical);
        for(const auto &operation:fixture.physical_operations)
            CHECK(operation.bytes==4096 && operation.op==NativeAdrenoClean);
        fixture.physical_operations.clear();
        CHECK(imported.acquireDeviceWrites(4*4096,4096)==0);
        CHECK(fixture.physical_operations.size()==1 &&
            fixture.physical_operations[0].physical==imported.spans()[4].physical &&
            fixture.physical_operations[0].op==NativeAdrenoCleanInvalidate);
        CHECK(imported.release()==0);
        std::puts("PASS imported cache maintenance covers the requested span range and operation only");
    }
    /* Every retired job is timed by the driver's clock without the CP's stamps: from its submission, or from the retirement before it
     * on its ring, to its own; a job over 8 ms is named in the slow table and the longest stay in the long table. */
    {
        Path p;CHECK(exposed(p,true)==kIOReturnSuccess);
        NativeAdrenoSubmitBuffer buffer{p.handle,0x800000,4096};
        const uint32_t timed=native_adreno_job_time[0],slow=native_adreno_job_time[7],over=native_adreno_job_time[2];
        uint32_t fence=0;
        clock_us=1000;p.submit.nextOwner(77);
        CHECK(p.submit.submit(&p.vm,&buffer,1,5,&fence,&ringclock)==kIOReturnSuccess && fence==1);
        uint32_t second=0;p.submit.nextOwner(78);
        CHECK(p.submit.submit(&p.vm,&buffer,1,5,&second,&ringclock)==kIOReturnSuccess && second==2);
        clock_us=11000;fencemem[1]=1;CHECK(p.submit.retireCompleted()==kIOReturnSuccess);   /* 10 ms from its submission */
        clock_us=12000;fencemem[1]=2;CHECK(p.submit.retireCompleted()==kIOReturnSuccess);   /* 1 ms after the first retired */
        CHECK(native_adreno_job_time[0]==timed+2 && native_adreno_job_time[2]==over+1 && native_adreno_job_time[7]==slow+1);
        const volatile uint64_t *row=native_adreno_slow_jobs[slow&15];
        CHECK((uint32_t)row[0]==1 && (row[0]>>32&0xff)==1 && (row[0]>>52&15)==2 && row[1]==10000 && row[2]==4096 && (uint32_t)row[3]==77 &&
            row[4]==5 && row[5]==1000 && row[6]==10000);
        bool found=false;for(unsigned i=0;i<8;++i)found=found || (native_adreno_long_jobs[i][1]==10000 && (uint32_t)native_adreno_long_jobs[i][3]==77);
        CHECK(found && native_adreno_job_time[6]>=10000);
        CHECK(p.submit.detach()==kIOReturnSuccess);
    }
    /* Without gpu-audit the submissions compare no line: neither the write-combined IBs nor the synchronized driver buffers. */
    {
        Path p;CHECK(exposed(p,true)==kIOReturnSuccess);
        NativeAdrenoSubmitBuffer buffer{p.handle,0x800000,4096};
        static unsigned calls;calls=0;
        native_adreno_line_audit=[](uint64_t,size_t,uint32_t *,uint32_t *) {++calls;return 0;};
        native_adreno_audit_mode=~0u;   /* read from the boot arguments: none on the host */
        const uint32_t wc=native_adreno_wc_audit[0],sync=native_adreno_sync_audit[0];
        for(uint32_t n=1;n<=4;++n) {
            uint32_t fence=0;
            CHECK(p.submit.submit(&buffer,1,0,&fence)==kIOReturnSuccess && fence==n);
            fencemem[1]=n;CHECK(p.submit.wait(n,&ringclock,1000)==kIOReturnSuccess);
        }
        CHECK(native_adreno_audit_mode==0 && calls==0 && native_adreno_wc_audit[0]==wc && native_adreno_sync_audit[0]==sync);
        CHECK(p.vm.writeCombinedTrusted());
        native_adreno_line_audit=nullptr;
        CHECK(p.submit.detach()==kIOReturnSuccess);
    }
    native_adreno_audit_mode=1;   /* gpu-audit=1 for the audit tests below */
    /* int-100: a write-combined BO is not maintained while the IB audit finds no dirty line; a dirty line or a failed audit ends the
     * trust, and that submission already maintains every BO through its physical pages. */
    {
        Path p;CHECK(exposed(p,true)==kIOReturnSuccess);
        NativeAdrenoSubmitBuffer buffer{p.handle,0x800000,4096};
        static std::vector<uint64_t> audited;static uint32_t dirty_lines,fail;
        audited.clear();dirty_lines=0;fail=0;
        native_adreno_line_audit=[](uint64_t physical,size_t bytes,uint32_t *dirty,uint32_t *stale) {
            audited.push_back(physical);audited.push_back(bytes);
            if(fail)return 1;
            *dirty+=dirty_lines;*stale+=1;return 0;};
        const uint32_t submissions=native_adreno_wc_audit[0],lines=native_adreno_wc_audit[1];
        const unsigned physical=fixture.physical_calls,owned=fixture.cache_calls;
        uint32_t fence=0;
        CHECK(p.submit.submit(&buffer,1,0,&fence)==kIOReturnSuccess && fence==1);
        CHECK(audited.size()==2 && audited[0]==p.object.spans()[0].physical && audited[1]==4096);
        CHECK(native_adreno_wc_audit[0]==submissions+1 && native_adreno_wc_audit[1]==lines+64 && native_adreno_wc_distrusted==0);
        CHECK(fixture.physical_calls==physical && fixture.cache_calls==owned && p.vm.writeCombinedTrusted());   /* no maintenance */
        CHECK(p.submit.fencePending(1) && !p.submit.fencePending(2));   /* one ring: pending until the CP writes it; 2 was never handed out */
        fencemem[1]=1;CHECK(!p.submit.fencePending(1));
        CHECK(p.submit.wait(1,&ringclock,1000)==kIOReturnSuccess && !p.submit.fencePending(1));
        dirty_lines=2;
        const uint32_t dirty_submission=native_adreno_submit_ibs[0]+1;
        CHECK(p.submit.submit(&buffer,1,0,&fence)==kIOReturnSuccess && fence==2);
        CHECK(!p.vm.writeCombinedTrusted() && native_adreno_wc_distrusted==dirty_submission);
        CHECK(fixture.physical_calls>physical);   /* this submission maintained the write-combined BO */
        const size_t calls=audited.size();
        fencemem[1]=2;CHECK(p.submit.wait(2,&ringclock,1000)==kIOReturnSuccess);
        CHECK(p.submit.submit(&buffer,1,0,&fence)==kIOReturnSuccess && audited.size()==calls);   /* no audit once distrusted */
        fencemem[1]=3;CHECK(p.submit.wait(3,&ringclock,1000)==kIOReturnSuccess);
        CHECK(p.submit.detach()==kIOReturnSuccess);
    }
    /* The audit compares only the first 16 submissions after attach; the re-assertion runs at each. */
    {
        Path p;CHECK(exposed(p,true)==kIOReturnSuccess);
        NativeAdrenoSubmitBuffer buffer{p.handle,0x800000,4096};
        static unsigned calls;calls=0;
        native_adreno_line_audit=[](uint64_t,size_t,uint32_t *,uint32_t *stale) {++calls;*stale+=0;return 0;};
        const uint32_t submissions=native_adreno_wc_audit[0];
        for(uint32_t n=1;n<=24;++n) {
            uint32_t fence=0;
            CHECK(p.submit.submit(&buffer,1,0,&fence)==kIOReturnSuccess && fence==n);
            fencemem[1]=n;CHECK(p.submit.wait(n,&ringclock,1000)==kIOReturnSuccess);
        }
        CHECK(calls==16 && native_adreno_wc_audit[0]==submissions+16 && p.vm.writeCombinedTrusted());
        native_adreno_line_audit=nullptr;
        CHECK(p.submit.detach()==kIOReturnSuccess);
    }
    {
        Path p;CHECK(exposed(p,true)==kIOReturnSuccess);
        NativeAdrenoSubmitBuffer buffer{p.handle,0x800000,4096};
        native_adreno_line_audit=[](uint64_t,size_t,uint32_t *,uint32_t *) {return 1;};
        uint32_t fence=0;
        CHECK(p.submit.submit(&buffer,1,0,&fence)==kIOReturnSuccess && fence==1);
        CHECK(!p.vm.writeCombinedTrusted() && native_adreno_wc_audit[4]==kIOReturnIOError);
        native_adreno_line_audit=nullptr;native_adreno_wc_distrusted=0;
        fencemem[1]=1;CHECK(p.submit.wait(1,&ringclock,1000)==kIOReturnSuccess);
        CHECK(p.submit.detach()==kIOReturnSuccess);
    }
    /* Refusals precede references, ring bytes and WPTR updates. */
    {
        Path p;CHECK(exposed(p)==kIOReturnSuccess);
        const uint32_t wptr=cp.regs[0x0807];
        uint32_t fence=0;
        CHECK(p.submit.submit(nullptr,1,0,&fence)==kIOReturnBadArgument);
        NativeAdrenoSubmitBuffer zero[1]={{p.handle,0x800000,0}};
        CHECK(p.submit.submit(zero,1,0,&fence)==kIOReturnBadArgument);
        NativeAdrenoSubmitBuffer unaligned[1]={{p.handle,0x800000,4098}};
        CHECK(p.submit.submit(unaligned,1,0,&fence)==kIOReturnBadArgument);
        NativeAdrenoSubmitBuffer wrong[1]={{p.handle,0x802000,4096}};
        CHECK(p.submit.submit(wrong,1,0,&fence)==kIOReturnBadArgument);
        NativeAdrenoSubmitBuffer overrun[1]={{p.handle,0x800000,8192+4}};
        CHECK(p.submit.submit(overrun,1,0,&fence)==kIOReturnBadArgument);
        NativeAdrenoSubmitBuffer unknown[1]={{p.handle+7,0x800000,4096}};
        CHECK(p.submit.submit(unknown,1,0,&fence)==kIOReturnBadArgument);
        NativeAdrenoSubmitBuffer many[NativeAdrenoSubmitMaxBuffers+1];
        for(auto &b:many)b={p.handle,0x800000,4096};
        CHECK(p.submit.submit(many,NativeAdrenoSubmitMaxBuffers+1,0,&fence)==kIOReturnBadArgument);
        CHECK(p.object.deviceReferences()==1 && cp.regs[0x0807]==wptr && p.submit.outstanding()==0);
        /* Duplicate buffers share one device reference. */
        NativeAdrenoSubmitBuffer twice[2]={{p.handle,0x800000,4096},{p.handle,0x800000,4096}};
        CHECK(p.submit.submit(twice,2,0,&fence)==kIOReturnSuccess);
        CHECK(p.object.deviceReferences()==2 && cp.regs[0x0807]==11+8+31);
        fencemem[1]=1;
        CHECK(p.submit.wait(1,&ringclock,100)==kIOReturnSuccess && p.object.deviceReferences()==1);
        CHECK(p.submit.detach()==kIOReturnSuccess);
    }
    /* A commit that poisons the ring retains every reference and disables execbuf. */
    {
        Path p;CHECK(exposed(p)==kIOReturnSuccess);
        cp.fail_publish=cp.publish+1;
        NativeAdrenoSubmitBuffer buffer{p.handle,0x800000,4096};
        uint32_t fence=0;
        CHECK(p.submit.submit(&buffer,1,0,&fence)==kIOReturnIOError);
        CHECK(!fence && p.submit.poisoned() && p.ring.poisoned());
        CHECK(p.submit.outstanding()==1 && p.submit.stats().poisoned_jobs==1);
        CHECK(p.object.deviceReferences()==2); /* never retired */
        CHECK(p.submit.detach()==kIOReturnBusy && p.submit.wait(1,&ringclock,10)==kIOReturnNotReady);
        CHECK(p.ring.detach()==kIOReturnBusy && p.vm.detach()==kIOReturnBusy);
        CHECK(p.object.release()==kIOReturnBusy);
    }
    /* A refused attach leaves both peers untouched. */
    {
        Path p;NativeAdrenoRing ring;NativeAdrenoSubmit submit;
        CHECK(submit.attach(nullptr,&ring)==kIOReturnBadArgument);
        CHECK(submit.attach(&p.vm,nullptr)==kIOReturnBadArgument);
        CHECK(submit.detach()==kIOReturnNotReady);
        CHECK(submit.submit(nullptr,1,0,nullptr)==kIOReturnBadArgument);
    }
    /* msm hangcheck: no fence progress with work pending is a hang once the CP state stops moving or after three progress retries; recovery
     * marks the guilty job's VM, moves the fence past it, restarts the ring and replays the later jobs with their own fences. */
    {
        Path p;CHECK(exposed(p)==kIOReturnSuccess);
        NativeAdrenoSubmitBuffer buffer{p.handle,0x800000,4096};
        uint32_t fence=0;
        for(uint32_t n=1;n<=3;++n)CHECK(p.submit.submit(&buffer,1,0,&fence)==kIOReturnSuccess && fence==n);
        bool hung=true;
        for(uint32_t tick=1;tick<=3;++tick) {
            cp.regs[0x0928]=0x1000*tick;   /* CP_IB1_BASE moves: progress */
            CHECK(p.submit.hangCheck(&hung)==kIOReturnSuccess && !hung);
        }
        cp.regs[0x0928]=0x9000;
        CHECK(p.submit.hangCheck(&hung)==kIOReturnSuccess && hung);   /* three retries spent */
        CHECK(p.submit.hangCheck(&hung)==kIOReturnSuccess && !hung);   /* progress retries start again: one retry for the changed state */
        static uint32_t restarted;restarted=~0u;
        auto restart=[](void *,NativeAdrenoRing *ring,uint32_t completed)->IOReturn {restarted=completed;return ring->restart(completed);};
        NativeAdrenoRecovery result{};
        const uint32_t refs=p.object.deviceReferences();
        CHECK(p.submit.recover(restart,nullptr,&ringclock,&result)==kIOReturnSuccess);
        CHECK(restarted==1 && result.fence==1 && result.replayed==2 && result.guilty==&p.vm && p.vm.hangs()==1 && p.vm.faults()==0 && !p.vm.unusable());
        CHECK(fencemem[1]==1 && p.ring.lastSubmitted()==3 && p.submit.outstanding()==2 && p.object.deviceReferences()==refs-1);
        CHECK(p.submit.stats().restarts==1);
        fencemem[1]=3;
        CHECK(p.submit.wait(3,&ringclock,1000)==kIOReturnSuccess && p.submit.wait(2,&ringclock,1000)==kIOReturnSuccess);
        CHECK(p.submit.outstanding()==0);
        /* Completed recovery leaves the process usable. */
        p.vm.noteHang();
        CHECK(p.vm.hangs()==2 && p.vm.faults()==0 && !p.vm.unusable());
        CHECK(p.submit.submit(&buffer,1,0,&fence)==kIOReturnSuccess);
        /* A restart that fails leaves the submit object poisoned. */
        CHECK(p.submit.recover([](void *,NativeAdrenoRing *,uint32_t)->IOReturn {return kIOReturnIOError;},nullptr,&ringclock,&result)==kIOReturnIOError);
        CHECK(p.submit.poisoned());
    }
    {
        Path p;CHECK(exposed(p)==kIOReturnSuccess);NativeAdrenoRing low;
        uint32_t lowmem[8192]{},lowfence[1024]{};uint8_t rec[2][64]{},info[2][32]{};
        CHECK(low.attachSecondary(&ringio,&ringcache,lowmem,sizeof(lowmem),0x200000,lowfence,sizeof(lowfence),0x208000)==0);
        CHECK(p.ring.setPreempt(0x300000,0x400000,0x108008)==0 && low.setPreempt(0x500000,0x600000,0x208008)==0);
        NativeAdrenoSubmit::PreemptSetup setup{};setup.io={&cp,cwrite,cread,cbarrier,nullptr,cgmuvote};
        setup.pages={{0x300000,0x500000},{0x400000,0x600000},{rec[0],rec[1]},{info[0],info[1]}};
        setup.now=cnow;setup.clock=&cp;setup.mode=2;
        CHECK(p.submit.setPreemption(&p.ring,&low,setup,true)==0);
        auto write=[](void *,uint64_t)->IOReturn{return 0;};
        CHECK(p.submit.setTableSwitching(2,write,nullptr)==0);
        p.ring.tableInstalled(0xaaaa000);low.tableInstalled(0xbbbb000);p.ring.useContext(42);
        NativeAdrenoSubmitBuffer buffer{p.handle,0x800000,4096};uint32_t first=0,second=0;
        clock_us=1;p.submit.nextPriority(1);CHECK(p.submit.submit(&p.vm,&buffer,1,7,&first,&ringclock)==0);
        CHECK(low.lastSubmitted()==first && p.ring.lastSubmitted()==0 && low.table()==p.vm.ttbr0() && p.ring.table()==0xaaaa000);
        CHECK(low.context()==42 && cp.regs[0x08a0]==0x341);
        uint32_t overtaking=0;
        p.submit.nextPriority(0);CHECK(p.submit.submit(&p.vm,&buffer,1,8,&overtaking,&ringclock)==0);
        cp.regs[0x08a0]=0x340;CHECK(p.submit.preemptIrq());
        CHECK(cp.regs[0x08a0]==0x341 && p.submit.deadReason()==0); // immediately switch back for newly queued high work
        cp.regs[0x08a0]=0x340;CHECK(p.submit.preemptIrq());
        clock_us=300001;CHECK(p.submit.retireCompleted()==0 && cp.regs[0x08a0]==0x341);
        cp.regs[0x08a0]=0x340;CHECK(p.submit.preemptIrq() && cp.regs[0x08a0]==0x340); // aged low ring gets a time slice
        clock_us+=2001;CHECK(p.submit.retireCompleted()==0 && cp.regs[0x08a0]==0x341);
        cp.regs[0x08a0]=0x340;CHECK(p.submit.preemptIrq() && cp.regs[0x08a0]==0x340); // no immediate aging ping-pong
        fencemem[1]=overtaking;CHECK(p.submit.retireCompleted()==0);
        cp.regs[0x08a0]=0x340;CHECK(p.submit.preemptIrq());
        p.submit.nextPriority(0);CHECK(p.submit.submit(&p.vm,&buffer,1,7,&second,&ringclock)==0);
        CHECK(low.lastSubmitted()==second && p.ring.lastSubmitted()==overtaking); // own queue cannot overtake
        lowfence[1]=second;CHECK(p.submit.retireCompleted()==0);
        CHECK(p.submit.submit(&p.vm,&buffer,1,7,&second,&ringclock)==0 && p.ring.lastSubmitted()==second);
        CHECK(p.ring.table()==p.vm.ttbr0());
        cp.regs[0x08a0]=0x340;CHECK(p.submit.preemptIrq());fencemem[1]=second;CHECK(p.submit.retireCompleted()==0);
        p.submit.nextPriority(1);CHECK(p.submit.submit(&p.vm,&buffer,1,7,&first,&ringclock)==0);
        cp.regs[0x08a0]=0x340;CHECK(p.submit.preemptIrq());
        // Another ring can advance beyond the small fence-history table while this job remains pending.
        for(unsigned i=0;i<8193;++i) {
            p.submit.nextPriority(0);CHECK(p.submit.submit(&p.vm,&buffer,1,8,&second,&ringclock)==0);
            cp.regs[0x08a0]=0x340;(void)p.submit.preemptIrq();
            fencemem[0]=p.ring.writePointer();fencemem[1]=second;
            CHECK(p.submit.retireCompleted()==0 && p.submit.outstanding()==1);
            cp.regs[0x08a0]=0x340;(void)p.submit.preemptIrq();
        }
        uint32_t issued=0,retired=0;CHECK(p.submit.fenceProgress(&issued,&retired) && issued==second && retired==first-1);
        CHECK(p.submit.observeFence(first,nullptr,0)==kIOReturnTimeout);
        /* A waiter's check is its own ring's: the high ring's finished fence is not pending while the scanout's prefix still waits for
         * the low ring's older one. Neither an unknown fence nor fence 0 is pending (wait() answers those). */
        CHECK(p.submit.fencePending(first) && !p.submit.fencePending(second) && !p.submit.fencePending(second+1) && !p.submit.fencePending(0));
        p.submit.nextPriority(0);uint32_t running=0;CHECK(p.submit.submit(&p.vm,&buffer,1,8,&running,&ringclock)==0);
        cp.regs[0x08a0]=0x340;(void)p.submit.preemptIrq();
        CHECK(p.submit.fencePending(running) && p.submit.fencePending(first));
        fencemem[0]=p.ring.writePointer();fencemem[1]=running;
        CHECK(!p.submit.fencePending(running) && p.submit.fencePending(first));   /* written, not yet retired */
        CHECK(p.submit.wait(running,&ringclock,0)==0 && !p.submit.fencePending(running) && p.submit.outstanding()==1);second=running;
        cp.regs[0x08a0]=0x340;(void)p.submit.preemptIrq();
        lowfence[1]=first;CHECK(!p.submit.fencePending(first));
        CHECK(p.submit.retireCompleted()==0 && p.submit.outstanding()==0);
        CHECK(p.submit.fenceProgress(&issued,&retired) && issued==second && retired==second);
        CHECK(p.submit.wait(first,&ringclock,0)==0 && p.submit.observeFence(first,nullptr,0)==0);
        CHECK(p.submit.wait(second+1,&ringclock,0)==kIOReturnBadArgument);
        CHECK(p.submit.setTableSwitching(1,write,nullptr)==0);p.submit.nextPriority(2);
        CHECK(p.submit.submit(&p.vm,&buffer,1,7,&second,&ringclock)==0 && p.ring.lastSubmitted()==second);
        fencemem[1]=second;CHECK(p.submit.retireCompleted()==0);
        std::puts("PASS two-ring priorities, per-ring table/context, queue FIFO, fence-history rollover and CPU-table fallback");
    }
    {
        /* A wait that pauses under the caller's lock (a connection's teardown) for a low-ring job behind a busy high ring. The pause
         * plays the CP: the high job ends, a triggered switch finishes, and the low ring's job runs only once the CP was switched
         * to it. Nothing but the wait looks at the rings, so the wait itself has to trigger and complete the switch. */
        Path p;CHECK(exposed(p)==kIOReturnSuccess);NativeAdrenoRing low;
        static uint32_t lowfence[1024];std::memset(lowfence,0,sizeof(lowfence));
        uint32_t lowmem[8192]{};uint8_t rec[2][64]{},info[2][32]{};
        CHECK(low.attachSecondary(&ringio,&ringcache,lowmem,sizeof(lowmem),0x200000,lowfence,sizeof(lowfence),0x208000)==0);
        CHECK(p.ring.setPreempt(0x300000,0x400000,0x108008)==0 && low.setPreempt(0x500000,0x600000,0x208008)==0);
        NativeAdrenoSubmit::PreemptSetup setup{};setup.io={&cp,cwrite,cread,cbarrier,nullptr,cgmuvote};
        setup.pages={{0x300000,0x500000},{0x400000,0x600000},{rec[0],rec[1]},{info[0],info[1]}};
        setup.now=cnow;setup.clock=&cp;setup.mode=2;
        CHECK(p.submit.setPreemption(&p.ring,&low,setup,true)==0);
        auto write=[](void *,uint64_t)->IOReturn{return 0;};
        CHECK(p.submit.setTableSwitching(2,write,nullptr)==0);
        p.ring.tableInstalled(0xaaaa000);low.tableInstalled(0xbbbb000);
        NativeAdrenoSubmitBuffer buffer{p.handle,0x800000,4096};
        static uint32_t high_fence,low_fence,pauses,switched;high_fence=low_fence=pauses=switched=0;
        clock_us=1;p.submit.nextPriority(0);CHECK(p.submit.submit(&p.vm,&buffer,1,8,&high_fence,&ringclock)==0);
        p.submit.nextPriority(1);CHECK(p.submit.submit(&p.vm,&buffer,1,7,&low_fence,&ringclock)==0);
        CHECK(p.ring.lastSubmitted()==high_fence && low.lastSubmitted()==low_fence && !(cp.regs[0x08a0]&1));   /* the high ring keeps the CP */
        const NativeAdrenoRingClock playing{&cp,cnow,[](void *)->int {
            clock_us+=1000;++pauses;
            if(pauses==1)fencemem[1]=high_fence;                                    /* the high ring goes idle */
            else if(cp.regs[0x08a0]&1) {cp.regs[0x08a0]&=~1u;switched=pauses;}      /* the CP finishes a triggered switch */
            else if(switched && pauses>switched)lowfence[1]=low_fence;              /* and only then runs the low ring's job */
            return 0;}};
        CHECK(p.submit.wait(low_fence,&playing,2000000)==kIOReturnSuccess);
        CHECK(switched && pauses<=8 && p.submit.preemptStats().triggers==1 && p.submit.preemptStats().completed==1);
        CHECK(p.submit.retireCompleted()==0 && p.submit.outstanding()==0);
        std::puts("PASS a pausing wait drives the ring switch for a low-ring job");
    }
    /* The driver's own synchronized buffers at a submission: not cleaned, and audited by the first 8 submissions and every 1024th. A
     * dirty line the audit finds ends the trust: that submission and every later one clean them again, and the audits stop. */
    {
        Path p;CHECK(exposed(p)==kIOReturnSuccess);
        NativeAdrenoMemory driver;
        CHECK(driver.allocate(8192,40,true,8192)==kIOReturnSuccess && driver.declareSynchronized()==kIOReturnSuccess);
        uint64_t mapped=0;
        CHECK(p.vm.map(&driver,0,0x804000,3,0,2,&mapped)==kIOReturnSuccess);
        NativeAdrenoSubmitBuffer buffer{p.handle,0x800000,4096};
        static uint64_t dirty_page;static unsigned pages;dirty_page=0;pages=0;
        native_adreno_line_audit=[](uint64_t physical,size_t bytes,uint32_t *dirty,uint32_t *) {
            CHECK(bytes==4096);++pages;if(physical==dirty_page)*dirty+=1;return 0;};
        native_adreno_submit_ibs[0]=0;native_adreno_sync_audit[4]=0;
        const uint32_t audits=native_adreno_sync_audit[0],lines=native_adreno_sync_audit[1];
        uint32_t fence=0,n=0;
        auto run=[&](unsigned owned) {
            const unsigned before=fixture.cache_calls;
            CHECK(p.submit.submit(&buffer,1,0,&fence)==kIOReturnSuccess && fence==++n);
            CHECK(fixture.cache_calls==before+owned);
            fencemem[1]=n;CHECK(p.submit.wait(n,&ringclock,1000)==kIOReturnSuccess);
        };
        const unsigned physical=fixture.physical_calls;
        for(unsigned i=0;i<10;++i)run(1);   /* the IB's own buffer alone (kernel memory nobody declared) */
        CHECK(pages==16 && native_adreno_sync_audit[0]==audits+8 && native_adreno_sync_audit[1]==lines+8*128);
        CHECK(fixture.physical_calls==physical+16);   /* each audited page's lines dropped again */
        native_adreno_submit_ibs[0]=1023;run(1);
        CHECK(pages==18 && native_adreno_sync_audit[0]==audits+9 && !native_adreno_sync_audit[4]);
        native_adreno_submit_ibs[0]=2047;dirty_page=driver.spans()[0].physical+4096;
        run(2);   /* found dirty: cleaned from this submission on */
        CHECK(pages==20 && native_adreno_sync_audit[2]==1 && native_adreno_sync_audit[4]==native_adreno_sync_audit[0]);
        native_adreno_submit_ibs[0]=3071;run(2);
        CHECK(pages==20);
        native_adreno_line_audit=nullptr;native_adreno_sync_audit[4]=0;native_adreno_sync_audit[2]=0;
        run(1);   /* trusted again (the host's reset): left alone */
        CHECK(p.vm.unmap(mapped)==kIOReturnSuccess && driver.release()==kIOReturnSuccess);
        CHECK(p.submit.detach()==kIOReturnSuccess);
        std::puts("PASS synchronized driver buffers at a submission, their audit and its distrust");
    }
    /* The submission log keeps the IB's address and size always and reads its words only with diag=1; the phases of a submission with
     * a clock are timed. */
    {
        Path p;CHECK(exposed(p)==kIOReturnSuccess);
        NativeAdrenoSubmitBuffer buffer{p.handle,0x800000,4096};
        static_cast<volatile uint32_t *>(p.object.mapping())[0]=0x70970001u;
        uint32_t fence=0;
        native_adreno_diag=0;
        CHECK(p.submit.submit(&buffer,1,0,&fence)==kIOReturnSuccess && fence==1);
        const volatile uint32_t *row=native_adreno_ib_log[(native_adreno_ib_log_count-1)&3];
        CHECK(row[0]==1 && row[1]==0x800000 && row[3]==4096 && row[4]==0);
        fencemem[1]=1;CHECK(p.submit.wait(1,&ringclock,1000)==kIOReturnSuccess);
        native_adreno_diag=1;
        const uint64_t timed=native_adreno_submit_us[0],total=native_adreno_submit_us[6];
        clock_us=50;
        CHECK(p.submit.submit(&p.vm,&buffer,1,0,&fence,&ringclock)==kIOReturnSuccess && fence==2);
        row=native_adreno_ib_log[(native_adreno_ib_log_count-1)&3];
        CHECK(row[0]==2 && row[4]==0x70970001u);
        CHECK(native_adreno_submit_us[0]==timed+1 && native_adreno_submit_us[6]==total);   /* the host's clock stands still */
        native_adreno_diag=0;
        fencemem[1]=2;CHECK(p.submit.wait(2,&ringclock,1000)==kIOReturnSuccess);
        CHECK(p.submit.detach()==kIOReturnSuccess);
        std::puts("PASS the submission log without and with diag=1, and the timed phases");
    }
    {
        Path p;CHECK(exposed(p)==kIOReturnSuccess);
        NativeAdrenoSubmitBuffer buffer{p.handle,0x800000,4096};
        uint32_t issued=0;
        for(uint32_t n=1;n<=3;++n)
            CHECK(p.submit.submit(&buffer,1,0,&issued)==kIOReturnSuccess && issued==n);
        fencemem[1]=2;
        CHECK(p.submit.observeFence(2,nullptr,0)==kIOReturnSuccess && p.ring.completedFence()==2);
        CHECK(p.submit.outstanding()==3 && p.object.deviceReferences()==4);
        const unsigned sampled=cp.acquire;
        CHECK(p.submit.retireCompleted()==kIOReturnSuccess && cp.acquire==sampled+1);
        CHECK(p.submit.outstanding()==1 && p.object.deviceReferences()==2);
        CHECK(p.submit.stats().references_retired==2 && p.submit.detach()==kIOReturnBusy);
        fencemem[1]=3;
        CHECK(p.submit.retireCompleted()==kIOReturnSuccess && cp.acquire==sampled+2);
        CHECK(p.submit.detach()==kIOReturnSuccess && p.ring.completedFence()==3);
        CHECK(p.object.deviceReferences()==1);
        std::puts("PASS one completion read per ring retires only jobs covered by its timestamp");
    }
    std::printf("PASS: 1 execbuf submission with exact IB packet and fence, 8 pre-reference refusals, 1 duplicate-buffer reference case and 1 poisoned-commit retention case over software SMMU/CP peers; 2 write-combine audit cases (trusted, dirty and failed); 1 hangcheck and 1 recovery with replay, 1 usable VM after recovery and 1 failed restart\n");
    testHiddenActiveFence();
    testSwitchCompletesWithoutInterrupt();
    testImmediateSwitchCompletion();
    testDisablePreemptionRestoresPrimary();
    testTimelineDetach();
    testFenceSequenceExhaustion();
    testSwitchCompletesBeforeRecovery();
    testSwitchTimeoutRecovery(true);
    testSwitchTimeoutRecovery(false);
    testRecoveryReplaysOnlyUnexposedWork();
    testSwitchRestartFailureRetainsReferences();
    testStalledWaitDoesNotTouchInactiveRing();
    testBoardResetKeepsFirstLowWorkOnPrimary(false);
    testBoardResetKeepsFirstLowWorkOnPrimary(true);
    testBoardResetReplayAndLowQueueContinuation();
    testBoardExecutionResetAndSecondHangKeepFenceHistory();
    testForgetDisplacedVm();
    testForgetUnreplacedVm();
    testForgetInactiveTail(true);
    testForgetInactiveTail(false);
}
/* Physical-page maintenance (imported BOs): record it; no host page is touched. */
/* Host double: the page attribute change of user BOs has no host equivalent (no host page is touched). */
IOReturn NativeAdrenoSetPagesCacheMode(const NativeAdrenoPhysicalSpan *spans,size_t count,bool){return spans && count?kIOReturnSuccess:kIOReturnBadArgument;}
IOReturn NativeAdrenoReassertWriteCombined(const NativeAdrenoPhysicalSpan *spans,size_t count,uint32_t *reset)
{return spans && count && reset?kIOReturnSuccess:kIOReturnBadArgument;}
IOReturn NativeAdrenoMaintainPhysicalPages(uint64_t physical,size_t bytes,NativeAdrenoCacheOperation op)
{
    if(!physical || !bytes || ((physical|bytes)&4095) || op>NativeAdrenoInvalidate)std::abort();
    ++fixture.physical_calls;fixture.physical_bytes+=bytes;
    fixture.physical_operations.push_back({physical,bytes,static_cast<unsigned>(op)});
    return kIOReturnSuccess;
}
