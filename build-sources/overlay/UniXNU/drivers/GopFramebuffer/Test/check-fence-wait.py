#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Run the display and (when available) Adreno fence methods against simulated registers and page mappings."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

source = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[1] / 'NativeLegacyDisplay.cpp'
text = source.read_text()
adreno = Path(sys.argv[2]) if len(sys.argv) > 2 else source.parents[1] / 'Adreno740'
gpu_methods = []
if adreno.is_dir():
    for filename, first, end in (
        ('NativeAdrenoMemory.cpp', 'bool NativeAdrenoMemory::mayOwnScanoutPages(', '/* The descriptor'),
        ('NativeAdrenoVm.cpp', 'bool NativeAdrenoVm::mayWriteScanout(', 'IOReturn NativeAdrenoVm::describe('),
        ('NativeAdrenoSubmit.cpp', 'bool NativeAdrenoSubmit::fenceProgress(', 'bool NativeAdrenoSubmit::fencePending('),
        ('NativeAdrenoPublish.cpp', 'extern "C" IOReturn native_adreno_scanout_wait(', '/* Publishes the service'),
    ):
        body = (adreno / filename).read_text()
        if first not in body:
            if len(sys.argv) > 2:
                sys.exit(f'Expected D candidate methods in {adreno / filename}')
            gpu_methods.clear()
            break
        at = body.index(first)
        gpu_methods.append(body[at:body.index(end, at)])
methods = []
# The cancellation method ends at its closing brace: it answers differently on macOS inside its own conditional block.
for start, end, keep in [('    bool isValidated(', '    uint64_t traceRequestIds[8];', ''),
                         ('    static uint32_t gpuResetEpoch(', '    unsigned validatedCount()', ''),
                         ('    void addValidated(', '    const OriginalLegacySwapRequest *gpuRequest=', ''),
                         ('    static void sortScanoutPages(', '    int directSlot(', ''),
                         ('    bool gpuWrote(', '    static void fenceFired(', ''),
                         ('    bool cancelSwapsAfterTimeout()', '\n    }\n#endif', '\n    }')]:
    at = text.index(start)
    methods.append(text[at:text.index(end, at)].replace(' override', '') + keep)

harness = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>
using IOReturn=int;
enum {kIOReturnSuccess,kIOReturnUnsupported,kIOReturnBusy,kIOReturnNotReady,kIOReturnTimeout,kIOReturnAborted};
struct OriginalLegacySwapRequest {};
struct OSObject {};
struct IOMemoryDescriptor {};
static uint32_t resetEpoch=1;
static uint32_t epoch() { return resetEpoch; }
static uint32_t (*queryEpoch)()=epoch;
#define native_adreno_reset_epoch (*queryEpoch)
static uint64_t clockNow=1000000;
static uint64_t mach_absolute_time() { return clockNow; }
static void absolutetime_to_nanoseconds(uint64_t time,uint64_t *out) { *out=time; }
static uint32_t native_legacy_direct[14];
static uint64_t native_legacy_swap_timeouts;
static bool queryKnown,resetDuringQuery;
static uint32_t querySubmitted,queryRetired,queryReason;
static bool detail(uint32_t *submitted,uint32_t *retired,uint32_t *extra) {
    if(resetDuringQuery){resetEpoch+=2;resetDuringQuery=false;}
    extra[2]=queryReason;
    if(queryKnown){*submitted=querySubmitted;*retired=queryRetired;}
    return queryKnown;
}
static bool legacy(uint32_t *submitted,uint32_t *retired) {
    uint32_t extra[3]={};return detail(submitted,retired,extra);
}
static bool (*queryDetail)(uint32_t *,uint32_t *,uint32_t *)=detail;
static bool (*queryLegacy)(uint32_t *,uint32_t *)=legacy;
#define native_adreno_fences_detail (*queryDetail)
#define native_adreno_fences (*queryLegacy)
static IOReturn (*queryScanout)(const uint64_t *,unsigned,bool *,uint32_t *,uint32_t *)=nullptr;
#define native_adreno_scanout_wait (*queryScanout)
enum {GpuTraceDisplayFence=22};
static unsigned lastReason,abortReports,failedReports;
template<class... Args> static void gpuTraceEvent(unsigned,unsigned,uint64_t,unsigned reason,Args...) {
    lastReason=reason;
    if(reason==5)++abortReports;
    if(reason==6)++failedReports;
}
struct Timer {
    unsigned delay=0,polls=0;
    void setTimeoutUS(unsigned us) {delay=us;++polls;}
    void cancelTimeout() {delay=0;}
};
struct Display {
    const OriginalLegacySwapRequest *fenceFor=nullptr,*gpuRequest=nullptr,*validated[8]={};
    uint32_t fenceTarget=0,validatedGpuEpoch[8]={};
    uint32_t scanoutTargets[2]={};
    uint64_t traceRequestIds[8]={},validatedAt[8]={};
    unsigned directMaps=0,retirements=0;
    bool rekick=false,resetDuringMap=false;
    struct Slot {unsigned pageCount;};
    Slot directSlots[3]{};
    const uint64_t *mappedPages=nullptr;
    const uint64_t *scanoutPages(unsigned) const {return mappedPages;}
    bool retireSwap(const OriginalLegacySwapRequest *request,bool force) {
        assert(force && transferred && isValidated(request));
        removeValidated(request);
        ++retirements;
        return true;
    }
    int directSlot(OSObject *,IOMemoryDescriptor *) {
        ++directMaps;
        if(resetDuringMap)resetEpoch+=2;
        return 2;
    }
    uint64_t fenceSince=0;
    bool fenceCaptured=false,fenceSlow=false,traceGpuComplete=false,gpuRetired=false,transferred=false,scanoutCaptured=false;
    Timer timer;
    Timer *fenceTimer=&timer;
    unsigned waits=0,passed=0;
    uint64_t traceId(const OriginalLegacySwapRequest *) {return 1;}
    void stat(unsigned) {}
    void note(unsigned event,int,const OriginalLegacySwapRequest *) {if(event==2)++waits;if(event==3)++passed;}
#include "methods.inc"
};
static void query(bool known,uint32_t submitted=0,uint32_t retired=0,uint32_t reason=2) {
    queryKnown=known;querySubmitted=submitted;queryRetired=retired;queryReason=reason;
}
#if TEST_ADRENO
struct NativeAdrenoPhysicalSpan {uint64_t offset,physical,bytes;};
struct NativeAdrenoMemory {
    const NativeAdrenoPhysicalSpan *ranges=nullptr;
    bool owned=false;
    const NativeAdrenoPhysicalSpan *spans() const {return ranges;}
    bool indexedOwnedPages() const {return owned;}
    static bool mayOwnScanoutPages(const uint64_t *,size_t);
};
static uint64_t ownedBits[16]{};
static uint64_t *ownedMap() {return ownedBits;}
static bool ownedIndex(uint64_t page,uint64_t *at) {
    *at=page/4096;
    return *at<sizeof(ownedBits)*8;
}
enum {ArmSmmu4KWrite=2};
struct NativeAdrenoVm {
    struct Mapping {NativeAdrenoMemory *memory;unsigned permissions;size_t mapped_spans,spans;};
    const Mapping *mappings_;
    size_t high_;
    uint64_t unmapped=0;
    bool addressSpace=true;
    uint64_t writeUnmaps() const {return unmapped;}
    bool hasScanoutAddressSpace() const {return addressSpace;}
    bool mayWriteScanout(const uint64_t *,size_t,bool,size_t &) const;
};
struct Ring {
    uint32_t done,issued;
    bool stale=false;
    IOReturn fenceValue(uint32_t *value) const {
        if(stale)return kIOReturnNotReady;
        *value=done;return kIOReturnSuccess;
    }
    uint32_t lastSubmitted() const {return issued;}
};
struct NativeAdrenoSubmit {
    struct Job {bool active;uint32_t fence;unsigned ring;NativeAdrenoVm *vm;uint64_t write_unmaps;};
    std::vector<Job> jobs_;
    Ring rings[2]{};
    NativeAdrenoVm *vm_;
    bool attached_=true,poisoned_=false,multi=true;
    unsigned table_mode_=2,ring_count_=2;
    uint32_t fence_issued_=0,failed=0;
    bool preemption() const {return multi;}
    Ring *ring(unsigned index) {return &rings[index];}
    bool failedFence(uint32_t fence) const {return fence==failed;}
    bool fenceProgress(uint32_t *,uint32_t *,uint32_t *);
    IOReturn captureScanoutTargets(const uint64_t *,size_t,uint32_t *);
    IOReturn scanoutProgress(const uint32_t *,uint32_t *);
};
static struct {bool published=true;NativeAdrenoSubmit *submit=nullptr;int *lock=nullptr;} g;
static bool lockHeld=false;
static bool IOLockTryLock(int *) {return !lockHeld;}
static void IOLockUnlock(int *) {}
#undef native_adreno_scanout_wait
#include "gpu.inc"
static bool actualPrefix(uint32_t *submitted,uint32_t *retired,uint32_t *extra) {
    const bool known=g.submit->fenceProgress(submitted,retired,extra);
    extra[2]=known?2:3;return known;
}
static void stress() {
    std::vector<uint64_t> scattered(8192);
    for(size_t i=0;i<scattered.size();++i)scattered[i]=uint64_t(i*4093%scattered.size())*4096;
    Display::sortScanoutPages(scattered.data(),static_cast<unsigned>(scattered.size()));
    for(size_t i=0;i<scattered.size();++i)assert(scattered[i]==i*4096);
    constexpr uint32_t before=1500,backgroundStart=1990,frontDone=2000,backgroundDone=10000;
    constexpr uint32_t refresh=8333;
    NativeAdrenoPhysicalSpan frontRanges[]={{0,0x8000,4096},{4096,0xa000,4096}};
    NativeAdrenoPhysicalSpan backgroundRanges[]={{0,0x9000,4096}}; // between the frame's two physical pages
    NativeAdrenoMemory frontMemory{frontRanges},backgroundMemory{backgroundRanges};
    NativeAdrenoVm::Mapping frontMapping{&frontMemory,ArmSmmu4KWrite,2,2};
    NativeAdrenoVm::Mapping backgroundMapping{&backgroundMemory,ArmSmmu4KWrite,1,1};
    NativeAdrenoVm front{&frontMapping,1},background{&backgroundMapping,1};
    uint64_t pages[]={0x8000,0xa000};
    NativeAdrenoSubmit gpu{};
    gpu.vm_=&front;g.submit=&gpu;
    queryDetail=actualPrefix;
    queryEpoch=epoch;
    resetEpoch=101;
    auto submit=[&](uint32_t first,uint32_t second) {
        gpu.rings[0]={first-1,first};
        gpu.rings[1]={second-1,second};
        gpu.fence_issued_=first>second?first:second;
        gpu.jobs_={{true,first,0,&front,front.unmapped},{true,second,1,&background,background.unmapped}};
    };
    auto sample=[&](Display &display,OriginalLegacySwapRequest &request,uint32_t us) {
        clockNow=uint64_t(us)*1000;
        display.transferred=false;
        int slot=-1;
        bool ready=display.prepareDirectSwap(&request,nullptr,nullptr,slot);
        assert(!ready || slot==2);
        return ready;
    };
    auto start=[&](Display &display,OriginalLegacySwapRequest &request) {
        display.mappedPages=pages;
        display.directSlots[2].pageCount=2;
        display.addValidated(&request,resetEpoch);
    };
    for(bool backgroundFirst:{true,false}) {
        submit(backgroundFirst?12:11,backgroundFirst?11:12);
        OriginalLegacySwapRequest request;
        Display oldPath,newPath;clockNow=1200000;
        start(oldPath,request);start(newPath,request);
        const uint32_t firstPoll=backgroundFirst?before:backgroundStart;
        queryScanout=nullptr;
        assert(!sample(oldPath,request,firstPoll));
        queryScanout=native_adreno_scanout_wait;
        assert(!sample(newPath,request,firstPoll));
        assert(newPath.scanoutTargets[0]==gpu.rings[0].issued && !newPath.scanoutTargets[1]);
        uint32_t nextPoll=firstPoll+newPath.timer.delay;
        while(nextPoll<frontDone) {
            assert(!sample(newPath,request,nextPoll));
            nextPoll+=newPath.timer.delay;
        }
        gpu.rings[0].done=gpu.rings[0].issued;
        assert(sample(newPath,request,nextPoll));
        assert(nextPoll==(backgroundFirst?2100:2190));
        queryScanout=nullptr;
        assert(!sample(oldPath,request,frontDone) && !sample(oldPath,request,refresh));
        gpu.rings[1].done=gpu.rings[1].issued;
        assert(sample(oldPath,request,backgroundDone));
    }
    std::puts("PASS 1.99 ms independent submit: 2.00 ms GPU finish, 2.19 ms display poll, old path blocked at 8.333 ms");
    queryScanout=native_adreno_scanout_wait;
    unsigned released=0,held=0,polls=0;
    for(unsigned trial=0;trial<100000;++trial) {
        const uint32_t id=10+2*trial,first=id+(trial&1),second=id+1-(trial&1);
        const unsigned kind=trial%5;
        // Independent, genuine alias, read-only alias, same address space, removed mapping.
        backgroundRanges[0].physical=kind==1 || kind==2?pages[1]:0x9000;
        backgroundMapping.permissions=kind==2?1:ArmSmmu4KWrite;
        submit(first,second);
        if(kind==3)gpu.jobs_[1].vm=&front;
        if(kind==4)++background.unmapped;
        OriginalLegacySwapRequest request;
        Display display;start(display,request);
        lockHeld=(trial%7==0);
        assert(!sample(display,request,backgroundStart));++polls;
        assert(display.scanoutCaptured!=lockHeld);
        lockHeld=false;
        // The foreground cannot pass before its own write has finished.
        const uint32_t ready=frontDone+(trial%41)*200;
        for(uint32_t us=frontDone;us<ready;us+=200) {
            assert(!sample(display,request,us));++polls;
        }
        gpu.rings[0].done=first;
        const bool independent=kind==0 || kind==2;
        assert(sample(display,request,ready)==independent);++polls;
        if(independent)++released;
        else {
            ++held;
            gpu.rings[1].done=second;
            assert(sample(display,request,backgroundDone+ready));++polls;
        }
        // Reusing the same request and physical buffer must wait for a new write.
        display.removeValidated(&request);
        submit(first+200002,second+200002);
        start(display,request);
        assert(!sample(display,request,backgroundStart));++polls;
    }
    std::printf("PASS 100000 two-ring swaps (%u polls): %u independent, %u held, all waited on reuse\n",polls,released,held);

    // Fill the low ring with work issued both before and after the visible write.
    std::vector<NativeAdrenoVm> otherProcesses(16,background);
    for(unsigned frame=0;frame<2000;++frame) {
        const uint32_t base=1000+257*frame,visible=base+128;
        gpu.jobs_.clear();
        for(uint32_t id=base;id<=base+256;++id)
            gpu.jobs_.push_back(id==visible?NativeAdrenoSubmit::Job{true,id,0,&front,front.unmapped}:
                NativeAdrenoSubmit::Job{true,id,1,&otherProcesses[id%otherProcesses.size()],background.unmapped});
        gpu.rings[0]={visible-1,visible};
        gpu.rings[1]={base-1,base+256};
        gpu.fence_issued_=base+256;
        OriginalLegacySwapRequest request;
        Display display;start(display,request);
        assert(!sample(display,request,backgroundStart) && !display.scanoutTargets[1]);
        gpu.rings[0].done=visible;
        assert(sample(display,request,frontDone));
    }
    std::puts("PASS 2000 bursts of 256 independent jobs from 16 GPU address spaces");

    // A late unrelated record cannot move a target already captured for an accepted swap.
    submit(40,41);
    gpu.jobs_[1].active=false;gpu.fence_issued_=40;
    OriginalLegacySwapRequest request;
    Display late;start(late,request);
    assert(!sample(late,request,before));
    gpu.jobs_[1].active=true;gpu.fence_issued_=41;
    gpu.rings[0].done=40;
    gpu.rings[1].stale=true;
    assert(sample(late,request,frontDone));

    // A missing unrelated completion register is fine. A missing producer register retries.
    submit(42,43);
    Display stale;start(stale,request);
    gpu.rings[0].stale=true;gpu.rings[1].stale=true;
    assert(!sample(stale,request,backgroundStart) && stale.scanoutCaptured && !stale.scanoutTargets[1]);
    gpu.rings[0].stale=false;gpu.rings[0].done=42;
    assert(sample(stale,request,frontDone));

    // Large private BOs do not spend the locked page-walk budget for an ordinary IOSurface.
    std::vector<NativeAdrenoPhysicalSpan> privateRanges(20000,{0,0x9000,4096});
    NativeAdrenoMemory privateMemory{privateRanges.data(),true};
    NativeAdrenoVm::Mapping privateMapping{&privateMemory,ArmSmmu4KWrite,privateRanges.size(),privateRanges.size()};
    background.mappings_=&privateMapping;
    submit(44,45);
    uint32_t targets[2]{};
    assert(!gpu.captureScanoutTargets(pages,2,targets) && targets[0]==44 && !targets[1]);
    // If the frame can overlap a private allocation, or the page walk cannot finish, keep that ring.
    ownedBits[0]=1ull<<(pages[0]/4096);
    privateRanges.back().physical=pages[0];
    assert(!gpu.captureScanoutTargets(pages,2,targets) && targets[0]==44 && targets[1]==45);
    ownedBits[0]=0;background.mappings_=&backgroundMapping;

    // A reset or a failed producer cancels the swap before flip().
    submit(46,47);
    Display reset;start(reset,request);
    assert(!sample(reset,request,backgroundStart));resetEpoch+=2;
    gpu.rings[0].done=46;
    assert(!sample(reset,request,frontDone) && reset.retirements==1 && !reset.traceGpuComplete);
    submit(48,49);
    Display failed;start(failed,request);
    assert(!sample(failed,request,backgroundStart));gpu.failed=48;
    gpu.rings[0].done=48;
    assert(!sample(failed,request,frontDone) && failed.retirements==1 && failedReports==1);
    gpu.failed=0;

    // Unsupported page-table mode leaves the old global fence check in charge.
    submit(50,51);
    gpu.table_mode_=1;
    Display fallback;start(fallback,request);
    gpu.rings[0].done=50;
    assert(!sample(fallback,request,frontDone) && fallback.fenceCaptured && !fallback.scanoutCaptured);
    gpu.rings[1].done=51;
    assert(sample(fallback,request,backgroundDone));
    std::puts("PASS late submit, unavailable fence register, mapping budget, reset, failed producer and legacy fallback");
}
#endif
int main() {
    OriginalLegacySwapRequest a,b;
    Display d;
    // Reproduce request 41: known pending target, then a busy query.
    query(true,89,88);
    assert(!d.gpuWrote(&a) && d.fenceTarget==89 && d.timer.delay==200);
    query(false,0,0,1);
    assert(!d.gpuWrote(&a) && !d.traceGpuComplete && lastReason==4);
    assert(d.fenceFor==&a && d.fenceTarget==89 && d.waits==1);
    // A later submission must not move this frame's target.
    query(true,99,88);assert(!d.gpuWrote(&a) && d.fenceTarget==89);
    query(true,99,89);assert(d.gpuWrote(&a) && d.traceGpuComplete && d.passed==1);

    // Initial busy polls have no valid target, including zero.
    query(false,0,0,1);assert(!d.gpuWrote(&b) && !d.fenceCaptured && !d.traceGpuComplete);
    query(true,100,99);assert(!d.gpuWrote(&b) && d.fenceTarget==100);
    query(false,0,0,3);assert(!d.gpuWrote(&b) && d.fenceTarget==100);
    query(true,101,100);assert(d.gpuWrote(&b));
    query(false,0,0,0);assert(!d.gpuWrote(&a)); // Provider not yet published.
    query(true,0,0);assert(d.gpuWrote(&a)); // A successful empty snapshot is complete.

    // Passing 250 ms records the slow wait once, without presenting the frame.
    query(true,120,119);assert(!d.gpuWrote(&a));
    const uint32_t slow=native_legacy_direct[12];
    clockNow+=250000000;
    assert(!d.gpuWrote(&a) && !d.traceGpuComplete && native_legacy_direct[12]==slow+1);
    query(false,0,0,1);clockNow+=250000000;
    assert(!d.gpuWrote(&a) && native_legacy_direct[12]==slow+1 && d.fenceTarget==120);
    query(true,130,120);assert(d.gpuWrote(&a) && d.traceGpuComplete);

    // A new head discards the previous head's target and elapsed time.
    query(true,140,139);assert(!d.gpuWrote(&a));
    query(false,0,0,1);assert(!d.gpuWrote(&b) && !d.fenceCaptured && !d.fenceTarget && !d.fenceSlow);
    query(true,150,149);assert(!d.gpuWrote(&b) && d.fenceTarget==150);
    d.validated[0]=&b;d.gpuRequest=&b;d.transferred=true;d.gpuRetired=true;
    assert(d.cancelSwapsAfterTimeout() && !d.timer.delay && !d.fenceFor && !d.fenceCaptured);
    assert(!d.validated[0] && !d.gpuRequest && !d.transferred && !d.gpuRetired && !d.traceGpuComplete);
    // The family may reuse the cancelled request address.
    query(true,160,159);assert(!d.gpuWrote(&b) && d.fenceTarget==160);
    query(true,161,160);assert(d.gpuWrote(&b));

    // Completion comparison retains the GPU's wrapping 32-bit fence semantics.
    query(true,UINT32_MAX,UINT32_MAX-1);assert(!d.gpuWrote(&a));
    query(true,0,0);assert(d.gpuWrote(&a));
    query(true,0,UINT32_MAX);assert(!d.gpuWrote(&b));
    query(true,1,0);assert(d.gpuWrote(&b));

    // Legacy providers also defer failed queries. CPU-only builds keep their path.
    queryDetail=nullptr;
    query(false,0,0,1);assert(!d.gpuWrote(&a));
    query(true,170,169);assert(!d.gpuWrote(&a));
    query(true,171,170);assert(d.gpuWrote(&a));
    queryLegacy=nullptr;assert(d.gpuWrote(&b) && !d.traceGpuComplete && lastReason==0);
    Display scanout;
    int slot=-1;
    queryDetail=detail;
    query(true,180,179);
    assert(Display::gpuEpochStable(1,1) && Display::gpuEpochStable(0,0));
    assert(!Display::gpuEpochStable(2,2) && !Display::gpuEpochStable(1,3));
    scanout.addValidated(&a,resetEpoch);
    assert(!scanout.prepareDirectSwap(&a,nullptr,nullptr,slot) && scanout.transferred);
    assert(scanout.timer.delay==200 && scanout.directMaps==0);
    resetEpoch+=2;
    scanout.addValidated(&a,resetEpoch); // A repeated validation must retain the original epoch.
    scanout.transferred=false; // The fence timer returned control to the queue.
    assert(!scanout.prepareDirectSwap(&a,nullptr,nullptr,slot));
    assert(!scanout.isValidated(&a) && !scanout.transferred && scanout.rekick && !scanout.timer.delay);
    assert(scanout.retirements==1 && abortReports==1 && scanout.directMaps==0 && slot==-1);

    scanout.addValidated(&b,resetEpoch);
    query(true,181,181);
    resetDuringQuery=true;
    assert(!scanout.prepareDirectSwap(&b,nullptr,nullptr,slot));
    assert(scanout.retirements==2 && abortReports==2 && scanout.directMaps==0 && !scanout.traceGpuComplete);

    scanout.addValidated(&a,resetEpoch);
    scanout.resetDuringMap=true;
    assert(!scanout.prepareDirectSwap(&a,nullptr,nullptr,slot));
    assert(scanout.retirements==3 && abortReports==3 && scanout.directMaps==1);

    scanout.resetDuringMap=false;
    scanout.addValidated(&b,resetEpoch);
    assert(scanout.prepareDirectSwap(&b,nullptr,nullptr,slot) && slot==2);
    assert(scanout.retirements==3 && scanout.directMaps==2);

    Display duringReset;
    ++resetEpoch;
    duringReset.addValidated(&a,resetEpoch);
    assert(!duringReset.prepareDirectSwap(&a,nullptr,nullptr,slot));
    assert(duringReset.retirements==1 && duringReset.directMaps==0 && abortReports==4);

    Display oldProvider;
    queryEpoch=nullptr;
    oldProvider.addValidated(&a,Display::gpuResetEpoch());
    assert(oldProvider.prepareDirectSwap(&a,nullptr,nullptr,slot) && slot==2 && abortReports==4);
    std::puts("PASS: display defers uncertain fences and drops swaps crossing GPU reset without scanout");
#if TEST_ADRENO
    stress();
#endif
}
'''

with tempfile.TemporaryDirectory(prefix='gop-fence-wait-') as directory:
    root = Path(directory)
    (root / 'methods.inc').write_text('\n'.join(methods))
    (root / 'gpu.inc').write_text('\n'.join(gpu_methods))
    (root / 'test.cpp').write_text(harness)
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    f'-DTEST_ADRENO={int(bool(gpu_methods))}',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    str(root / 'test.cpp'), '-o', str(root / 'test')], check=True)
    subprocess.run([str(root / 'test')], check=True)
