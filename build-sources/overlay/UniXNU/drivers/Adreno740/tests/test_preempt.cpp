// SPDX-License-Identifier: MIT
#include "NativeAdrenoPreempt.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <cstring>

// Setup and state transitions must run even in a build with NDEBUG.
#define CHECK(condition) do { \
    if (!(condition)) { \
        std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); \
        std::exit(1); \
    } \
} while (0)

using SwitchState=NativeAdrenoSwitchState;

struct Peer {
 uint32_t cntl=0, registers[0x9000]={};
 uint64_t now=0;
 bool fail=false, failClear=false;
 uint32_t failed_read=UINT32_MAX;
 unsigned sqe_calls=0;
 std::vector<uint32_t> order;
};
static int write(void *v,uint32_t r,uint32_t n) {
 auto &p=*(Peer*)v;
 p.order.push_back(r);
 if(p.fail)return 1;
 CHECK(r<sizeof p.registers/sizeof p.registers[0]);
 p.registers[r]=n;
 if(r==0x08a0)p.cntl=n;
 return 0;
}
static int read(void *v,uint32_t r,uint32_t *n) {
 auto &p=*(Peer*)v;
 CHECK(r<sizeof p.registers/sizeof p.registers[0]);
 *n=r==0x08a0?p.cntl:p.registers[r];
 return r==p.failed_read?17:0;
}
static int barrier(void *) {return 0;}
static int gmu(void *v,uint32_t r,uint32_t n) {
 auto &p=*(Peer*)v;
 CHECK(r==0x1f8c4);
 p.order.push_back(n?0xaabb:0xccdd);
 return !n && p.failClear;
}
static uint64_t now(void *v) {return ((Peer*)v)->now;}
static int sqe(void *v,NativeAdrenoSqeSnapshot *out)
{
 auto &p=*(Peer*)v;++p.sqe_calls;p.now+=7;
 CHECK(out->words[1][0]==UINT32_MAX && !out->valid_mask[1][0]);
 out->words[0][0]=0;out->valid_mask[0][0]=1;out->read_status[0]=19;out->first_failed_word[0]=1;
 return 19;
}
static void testEvidence()
{
 Peer p;NativeAdrenoPreempt a;NativeAdrenoPreemptIo io{&p,write,read,barrier,&p,gmu,sqe};
 alignas(4) uint8_t r0[64]{},r1[64]{},s0[32]{},s1[32]{};
 NativeAdrenoPreemptRings rings{{0x10000,0x20000},{0x30000,0x40000},{r0,r1},{s0,s1}};
 CHECK(!a.attach(&io,rings,0,now,&p));
 NativeAdrenoPublishedTail tail{};tail.hw_rptr=tail.hw_wptr=0x9ab;tail.valid_mask=7;
 NativeAdrenoSwitchEvidence out{},keep{};
 CHECK(!a.captureTimeoutEvidence(2,&out));
 struct TailPeer {Peer *peer;NativeAdrenoPublishedTail *tail;} tail_peer{&p,&tail};
 p.now=55;r0[8]=0x12;CHECK(a.trigger(1,true,[](void *cookie,NativeAdrenoPublishedTail *out) {
     auto &c=*static_cast<TailPeer *>(cookie);
     // No new MMIO read of GX before the existing keepalive vote; STOP is later.
     CHECK(c.peer->order.back()==0xaabb && c.peer->cntl==0);
     *out=*c.tail;
 },&tail_peer));
 r0[8]=0x34;p.failed_read=0x08a2;p.now+=1000001;
 p.registers[0x08ab]=0xfeed;
 p.registers[0x0806]=0x105;
 p.registers[0x0807]=0x207;
 CHECK(a.captureTimeoutEvidence(2,&out) && p.sqe_calls==1);
 CHECK(out.version==1 && out.word_count==NativeAdrenoSwitchEvidenceWords && out.reason==2);
 CHECK(out.sequence==native_adreno_switch_count && out.trigger_us[0]==55 && out.from_ring==0 && out.to_ring==1);
 CHECK(out.before.record[0][2]==0x12 && out.after.record[0][2]==0x34);
 CHECK(out.before.valid_mask==15 && out.after.valid_mask==15);
 CHECK(out.register_status[2]==17 && out.registers[2]==UINT32_MAX && !(out.register_valid_mask&4));
 CHECK(out.registers[1]==0x40000 && (out.register_valid_mask&2));
 CHECK(out.registers[3]==0x20000 && out.registers[4]==0 && out.registers[5]==0xfeed);
 CHECK(out.registers[6]==0x105 && out.registers[7]==0x207);
 CHECK(out.register_valid_mask==(0x1ffu&~4u));
 CHECK(out.sqe_status==19 && out.sqe.words[0][0]==0 && (out.sqe.valid_mask[0][0]&1));
 CHECK(out.sqe.words[0][1]==UINT32_MAX && !(out.sqe.valid_mask[0][0]&2));
 CHECK(out.sqe.words[1][0]==UINT32_MAX && !out.sqe.valid_mask[1][0]);
 CHECK(out.begin_us[0]+7==out.end_us[0]);
 CHECK(out.registers[0]==0x341 && out.registers[8]==0x341 && a.inFlight());
 keep=out;CHECK(!a.captureTimeoutEvidence(2,&out) && !std::memcmp(&out,&keep,sizeof out));
 CHECK(a.noteTimeout(nullptr));CHECK(!a.captureTimeoutEvidence(2,&out));
 p.failed_read=UINT32_MAX;p.cntl&=~1u;CHECK(a.complete()==SwitchState::Completed && a.releaseKeepalive());
 CHECK(!a.restart(0));CHECK(!std::memcmp(&out,&keep,sizeof out));
 CHECK(a.trigger(1,true,nullptr,nullptr,nullptr,false));
 CHECK(!a.captureTimeoutEvidence(2,&out));
 p.cntl&=~1u;CHECK(a.complete()==NativeAdrenoSwitchState::Completed);
 CHECK(a.trigger(0,true));CHECK(a.captureTimeoutEvidence(2,&out));
 CHECK(out.from_ring==1 && out.to_ring==0 && out.before.valid_mask==0xf);
 CHECK(out.published_tail.valid_mask==0 && out.published_tail.count==0);
 CHECK(out.published_tail.hw_wptr==UINT32_MAX && out.published_tail.read_status[1]==-1);
 CHECK(out.published_tail.words[0]==UINT32_MAX);
 p.cntl&=~1u;CHECK(a.complete()==SwitchState::Completed && a.releaseKeepalive() && !a.detach());
 std::puts("PASS timeout SQE even at equal ring pointers, explicit read validity, capture interval, pre-reset pages and one capture per switch");
}
static void testSwitchStates()
{
 Peer p;NativeAdrenoPreempt a;
 NativeAdrenoPreemptIo io{&p,write,read,barrier,&p,nullptr};
 alignas(4) uint8_t r0[64]{},r1[64]{},s0[32]{},s1[32]{};
 NativeAdrenoPreemptRings rings{{0x10000,0x20000},{0x30000,0x40000},{r0,r1},{s0,s1}};
 CHECK(a.attach(&io,rings,0,now,&p)==kIOReturnBadArgument && p.order.empty());
 CHECK(a.restart(0)==kIOReturnNotReady && a.complete()==SwitchState::Idle);
 io.gmu_write32=gmu;
 CHECK(a.attach(&io,rings,0,now,&p)==kIOReturnSuccess);
 CHECK(a.complete()==SwitchState::Idle && a.restart(2)==kIOReturnBadArgument);
 bool ready=true;
 p.now=101;
 CHECK(a.trigger(1,true,nullptr,nullptr,&ready) && !ready);
 CHECK(a.complete()==SwitchState::Pending);
 p.failed_read=0x08a0;
 p.cntl&=~1u;
 CHECK(a.complete()==SwitchState::ReadError && a.inFlight() && a.loaded()==0);
 CHECK(a.readCntl()==UINT32_MAX);
 const uint32_t slot=(native_adreno_switch_count-1)&(NativeAdrenoSwitchLogRows-1);
 volatile const uint32_t *const row=native_adreno_switch_log[slot];
 CHECK(row[5]==(1u<<8) && row[3]==0);
 p.now=121;
 p.failed_read=UINT32_MAX;
 CHECK(a.restart(0)==kIOReturnSuccess && !a.inFlight());
 CHECK(row[5]==((1u<<8)|(4u<<16)) && row[3]==121);
 CHECK(row[7]==UINT32_MAX && row[20]==UINT32_MAX);
 CHECK(a.stats().faulted==1 && a.stats().completed==0 && a.complete()==SwitchState::Idle);
 CHECK(a.detach()==kIOReturnSuccess);
 std::puts("PASS missing keepalive, all completion results, and in-flight reset closes the original event");
}
int main() {
 testEvidence();
 testSwitchStates();
 Peer p;NativeAdrenoPreempt a;NativeAdrenoPreemptIo io{&p,write,read,barrier,&p,gmu};
 alignas(4) uint8_t r0[64]{},r1[64]{},s0[32]{},s1[32]{};
 NativeAdrenoPreemptRings rings{{0x10000,0x20000},{0x30000,0x40000},{r0,r1},{s0,s1}};
 CHECK(!a.attach(&io,rings,0,now,&p));p.order.clear();
 CHECK(a.trigger(1,true) && p.order.front()==0xaabb && p.cntl==0x341);
 CHECK(a.complete()==SwitchState::Pending && a.stats().faulted==0);p.now=40000;CHECK(!a.timedOut(1000000));
 p.cntl=0x340;CHECK(a.complete()==SwitchState::Completed && a.loaded()==1 && p.order.back()!=0xccdd);
 CHECK(a.releaseKeepalive() && p.order.back()==0xccdd);
 uint32_t frozen[NativeAdrenoSwitchLogWords],untouched[NativeAdrenoSwitchLogWords];
 for(auto &word:frozen)word=0xdeadbeef;
 std::memcpy(untouched,frozen,sizeof(frozen));
 CHECK(!a.noteTimeout(nullptr,frozen) && !std::memcmp(frozen,untouched,sizeof(frozen)));
 const uint64_t started=p.now;
 CHECK(a.trigger(0,true));p.now+=1000001;CHECK(a.timedOut(1000000));
 const uint32_t progress[6]={0x3a000,1,0x44000,1,0,0};
 CHECK(a.noteTimeout(progress,frozen) && a.stats().faulted==1);
 CHECK(frozen[1]==started && !frozen[2] && frozen[3]==p.now && !frozen[4]);
 CHECK(frozen[5]==(1u|2u<<16) && frozen[6]==0x341 && frozen[7]==0x341);
 CHECK(!std::memcmp(frozen+28,progress,sizeof(progress)));
 std::memcpy(untouched,frozen,sizeof(frozen));
 CHECK(!a.noteTimeout(nullptr,frozen) && a.stats().faulted==1);
 CHECK(!std::memcmp(frozen,untouched,sizeof(frozen)));
 // A late STOP-clear completes exactly once, leaving the frozen timeout record.
 p.cntl&=~1u;CHECK(a.complete()==SwitchState::Completed && a.loaded()==0 && a.releaseKeepalive());
 CHECK(a.stats().completed==2 && a.stats().faulted==0 && a.complete()==SwitchState::Idle);
 CHECK(!a.noteTimeout(nullptr,frozen) && !std::memcmp(frozen,untouched,sizeof(frozen)));
 p.order.clear();
 CHECK(a.restart(1)==kIOReturnSuccess && !a.inFlight() && a.loaded()==1);
 const std::vector<uint32_t> restart_order{0x8e50,0x08a1,0x08a2};
 CHECK(p.order==restart_order);
 // Exercise more than the circular log capacity before checking the copied event.
 for(unsigned i=0;i<20;++i) {
     CHECK(a.trigger(a.loaded()^1u,true));p.cntl&=~1u;
     CHECK(a.complete()==SwitchState::Completed && a.releaseKeepalive());
 }
 CHECK(!a.noteTimeout(nullptr,frozen) && !std::memcmp(frozen,untouched,sizeof(frozen)));
 p.fail=true;p.failClear=true;
 CHECK(!a.trigger(0,true) && p.order.back()==0xccdd && a.stats().faulted==1);
 p.fail=false;p.order.clear();
 CHECK(!a.trigger(0,true) && p.order==std::vector<uint32_t>{0xccdd});
 p.failClear=false;p.order.clear();
 CHECK(a.trigger(0,true) && p.order[0]==0xccdd && p.order[1]==0xaabb);
 p.cntl&=~1u;
 CHECK(a.complete()==SwitchState::Completed && a.releaseKeepalive());
 r0[60]=0x67;r0[61]=0x45;r0[62]=0x23;r0[63]=1;
 CHECK(NativeAdrenoPreempt::recordWord(r0,60)==0x01234567);
 CHECK(!NativeAdrenoPreempt::recordWord(r0,61) && !NativeAdrenoPreempt::recordWord(r0,63));
 CHECK(!a.detach());std::puts("PASS preemption ordering, timeout snapshot, stale/repeated capture refusal, late completion, restart, circular wrap and refused trigger");
}
