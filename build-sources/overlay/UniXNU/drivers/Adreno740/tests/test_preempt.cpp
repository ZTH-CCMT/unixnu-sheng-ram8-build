// SPDX-License-Identifier: MIT
#include "NativeAdrenoPreempt.h"
#include <cassert>
#include <cstdio>
#include <vector>
#include <cstring>
struct Peer {uint32_t cntl=0;uint64_t now=0;bool fail=false;uint32_t failed_read=UINT32_MAX;unsigned sqe_calls=0;std::vector<uint32_t> order;};
static int write(void *v,uint32_t r,uint32_t n) {auto &p=*(Peer*)v;p.order.push_back(r);if(p.fail)return 1;if(r==0x08a0)p.cntl=n;return 0;}
static int read(void *v,uint32_t r,uint32_t *n) {auto &p=*(Peer*)v;*n=r==0x08a0?p.cntl:0;return r==p.failed_read?17:0;}
static int barrier(void *) {return 0;}
static int gmu(void *v,uint32_t,uint32_t n) {((Peer*)v)->order.push_back(n?0xaabb:0xccdd);return 0;}
static uint64_t now(void *v) {return ((Peer*)v)->now;}
static int sqe(void *v,NativeAdrenoSqeSnapshot *out)
{
 auto &p=*(Peer*)v;++p.sqe_calls;p.now+=7;
 assert(out->words[1][0]==UINT32_MAX && !out->valid_mask[1][0]);
 out->words[0][0]=0;out->valid_mask[0][0]=1;out->read_status[0]=19;out->first_failed_word[0]=1;
 return 19;
}
static void testEvidence()
{
 Peer p;NativeAdrenoPreempt a;NativeAdrenoPreemptIo io{&p,write,read,barrier,&p,gmu,sqe};
 alignas(4) uint8_t r0[64]{},r1[64]{},s0[32]{},s1[32]{};
 NativeAdrenoPreemptRings rings{{0x10000,0x20000},{0x30000,0x40000},{r0,r1},{s0,s1}};
 assert(!a.attach(&io,rings,0,now,&p));
 NativeAdrenoPublishedTail tail{};tail.hw_rptr=tail.hw_wptr=0x9ab;tail.valid_mask=7;
 NativeAdrenoSwitchEvidence out{},keep{};
 assert(!a.captureTimeoutEvidence(2,&out));
 struct TailPeer {Peer *peer;NativeAdrenoPublishedTail *tail;} tail_peer{&p,&tail};
 p.now=55;r0[8]=0x12;assert(a.trigger(1,true,[](void *cookie,NativeAdrenoPublishedTail *out) {
     auto &c=*static_cast<TailPeer *>(cookie);
     // No new MMIO read of GX before the existing keepalive vote; STOP is later.
     assert(c.peer->order.back()==0xaabb && c.peer->cntl==0);
     *out=*c.tail;
 },&tail_peer));
 r0[8]=0x34;p.failed_read=0x08a2;p.now+=1000001;
 assert(a.captureTimeoutEvidence(2,&out) && p.sqe_calls==1);
 assert(out.version==1 && out.word_count==NativeAdrenoSwitchEvidenceWords && out.reason==2);
 assert(out.sequence==native_adreno_switch_count && out.trigger_us[0]==55 && out.from_ring==0 && out.to_ring==1);
 assert(out.before.record[0][2]==0x12 && out.after.record[0][2]==0x34);
 assert(out.before.valid_mask==15 && out.after.valid_mask==15);
 assert(out.register_status[2]==17 && out.registers[2]==UINT32_MAX && !(out.register_valid_mask&4));
 assert(out.registers[1]==0 && (out.register_valid_mask&2));
 assert(out.sqe_status==19 && out.sqe.words[0][0]==0 && (out.sqe.valid_mask[0][0]&1));
 assert(out.sqe.words[0][1]==UINT32_MAX && !(out.sqe.valid_mask[0][0]&2));
 assert(out.sqe.words[1][0]==UINT32_MAX && !out.sqe.valid_mask[1][0]);
 assert(out.begin_us[0]+7==out.end_us[0]);
 assert(out.registers[0]==0x341 && out.registers[8]==0x341 && a.inFlight());
 keep=out;assert(!a.captureTimeoutEvidence(2,&out) && !std::memcmp(&out,&keep,sizeof out));
 assert(a.noteTimeout(nullptr));assert(!a.captureTimeoutEvidence(2,&out));
 p.failed_read=UINT32_MAX;p.cntl&=~1u;assert(a.complete() && a.releaseKeepalive());
 assert(!a.restart(0));assert(!std::memcmp(&out,&keep,sizeof out));
 assert(a.trigger(1,true));assert(!a.captureTimeoutEvidence(2,&out));
 p.cntl&=~1u;assert(a.complete() && a.releaseKeepalive() && !a.detach());
 std::puts("PASS timeout SQE even at equal ring pointers, explicit read validity, capture interval, pre-reset pages and one capture per switch");
}
int main() {
 testEvidence();
 Peer p;NativeAdrenoPreempt a;NativeAdrenoPreemptIo io{&p,write,read,barrier,&p,gmu};
 alignas(4) uint8_t r0[64]{},r1[64]{},s0[32]{},s1[32]{};
 NativeAdrenoPreemptRings rings{{0x10000,0x20000},{0x30000,0x40000},{r0,r1},{s0,s1}};
 assert(!a.attach(&io,rings,0,now,&p));p.order.clear();
 assert(a.trigger(1,true) && p.order.front()==0xaabb && p.cntl==0x341);
 assert(!a.complete() && a.stats().faulted==0);p.now=40000;assert(!a.timedOut(1000000));
 p.cntl=0x340;assert(a.complete() && a.loaded()==1 && p.order.back()!=0xccdd);
 assert(a.releaseKeepalive() && p.order.back()==0xccdd);
 uint32_t frozen[NativeAdrenoSwitchLogWords],untouched[NativeAdrenoSwitchLogWords];
 for(auto &word:frozen)word=0xdeadbeef;
 std::memcpy(untouched,frozen,sizeof(frozen));
 assert(!a.noteTimeout(nullptr,frozen) && !std::memcmp(frozen,untouched,sizeof(frozen)));
 const uint64_t started=p.now;
 assert(a.trigger(0,true));p.now+=1000001;assert(a.timedOut(1000000));
 const uint32_t progress[6]={0x3a000,1,0x44000,1,0,0};
 assert(a.noteTimeout(progress,frozen) && a.stats().faulted==1);
 assert(frozen[1]==started && !frozen[2] && frozen[3]==p.now && !frozen[4]);
 assert(frozen[5]==(1u|2u<<16) && frozen[6]==0x341 && frozen[7]==0x341);
 assert(!std::memcmp(frozen+28,progress,sizeof(progress)));
 std::memcpy(untouched,frozen,sizeof(frozen));
 assert(!a.noteTimeout(nullptr,frozen) && a.stats().faulted==1);
 assert(!std::memcmp(frozen,untouched,sizeof(frozen)));
 // A late STOP-clear still completes normally, without reopening the timed-out event.
 p.cntl&=~1u;assert(a.complete() && a.loaded()==0 && a.releaseKeepalive());
 assert(!a.noteTimeout(nullptr,frozen) && !std::memcmp(frozen,untouched,sizeof(frozen)));
 p.order.clear();
 assert(a.restart(1)==kIOReturnSuccess && !a.inFlight() && a.loaded()==1);
 const std::vector<uint32_t> restart_order{0x8e50,0x08a1,0x08a2};
 assert(p.order==restart_order);
 // Exercise more than the circular log capacity before checking the copied event.
 for(unsigned i=0;i<20;++i) {
     assert(a.trigger(a.loaded()^1u,true));p.cntl&=~1u;
     assert(a.complete() && a.releaseKeepalive());
 }
 assert(!a.noteTimeout(nullptr,frozen) && !std::memcmp(frozen,untouched,sizeof(frozen)));
 p.fail=true;assert(!a.trigger(0,true) && p.order.back()==0xccdd && a.stats().faulted==2);
 assert(!a.detach());std::puts("PASS preemption ordering, timeout snapshot, stale/repeated capture refusal, late completion, restart, circular wrap and refused trigger");
}
