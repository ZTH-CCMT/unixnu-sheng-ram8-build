// SPDX-License-Identifier: BSD-2-Clause-Patent
// Pure report planning/ACK bookkeeping. No allocation, timer, I/O or OS ABI.
#ifndef NATIVE_HID_TOUCH_PLAN_H
#define NATIVE_HID_TOUCH_PLAN_H
#include <stdint.h>
#include <string.h>
#define NHT_CONTACTS 10
#define NHT_REPORT_BYTES (1+NHT_CONTACTS*6+1)
struct nht_point {int id,x,y;}; // Already normalized0..32767; IDs0..255.
struct nht_state {uint32_t sequence;unsigned count;struct nht_point point[NHT_CONTACTS];unsigned cancel_sent;};
struct nht_packet {unsigned char bytes[NHT_REPORT_BYTES];unsigned entries,downs,ups;struct nht_state acknowledged;};
struct nht_plan {uint32_t sequence;unsigned count,next;struct nht_packet packet[3];};
enum nht_contact_flags { NHT_UP=2, NHT_DOWN=3, NHT_CANCEL=7 };
static inline int nht_index(const struct nht_point *p,unsigned count,int id) {for(unsigned i=0;i<count;++i)if(p[i].id==id)return (int)i;return -1;}
static inline int nht_valid(const struct nht_point *p,unsigned count) {
 if(count>NHT_CONTACTS || (count && !p))return 0;
 for(unsigned i=0;i<count;++i)if(p[i].id<0 || p[i].id>255 || p[i].x<0 || p[i].x>32767 || p[i].y<0 || p[i].y>32767 || nht_index(p,i,p[i].id)>=0)return 0;
 return 1;
}
static inline void nht_entry(struct nht_packet *q,const struct nht_point *p,enum nht_contact_flags flags) {
 unsigned char *f=q->bytes+1+q->entries*6;f[0]=(unsigned char)flags;f[1]=(unsigned char)p->id;
 f[2]=(unsigned char)p->x;f[3]=(unsigned char)(p->x>>8);f[4]=(unsigned char)p->y;f[5]=(unsigned char)(p->y>>8);
 ++q->entries;if(flags!=NHT_UP)q->acknowledged.point[q->acknowledged.count++]=*p,++q->downs;else ++q->ups;
}
// Tip + Untouch marks a cancellation. Follow it with tip-up to clear the
// parser's contact state. Keep the intermediate ACK when the clear needs retry.
// A collection cancel is safe when no acknowledged contact continues.
// Returns 0..3 packets, or -1 for invalid input. Input/ack state is read-only.
// If no packets are needed, caller may commit the empty snapshot's sequence.
static inline int nht_build_with_cancel(const struct nht_state *ack,const struct nht_point *now,unsigned count,
                                        uint32_t sequence,uint32_t cancelled_ids,struct nht_plan *plan) {
 if(!ack || !plan || (sequence&1) || !nht_valid(ack->point,ack->count) || !nht_valid(now,count))return -1;
 memset(plan,0,sizeof(*plan));plan->sequence=sequence;
 if(sequence==ack->sequence)return 0;
 unsigned removed=0;
 uint32_t cancelled=0;
 for(unsigned j=0;j<ack->count;++j) {
  const int id=ack->point[j].id;
  if(nht_index(now,count,id)>=0 && !ack->cancel_sent)continue;
  ++removed;
  if(id<32)cancelled|=cancelled_ids&(UINT32_C(1)<<id);
 }
 if(!count && !removed)return 0;
 if(cancelled && removed==ack->count && !ack->cancel_sent) {
  struct nht_packet *notice=&plan->packet[plan->count++];
  notice->bytes[0]=3;
  for(unsigned j=0;j<ack->count;++j) {
   const int id=ack->point[j].id;
   const int lost=id<32 && (cancelled&(UINT32_C(1)<<id));
   nht_entry(notice,&ack->point[j],lost ? NHT_CANCEL : NHT_DOWN);
  }
  notice->acknowledged.sequence=ack->sequence;
  notice->acknowledged.cancel_sent=1;
 }
 const int clear_cancel=ack->cancel_sent || plan->count!=0;
 struct nht_packet *first=&plan->packet[plan->count++];
 first->bytes[0]=3;
 first->acknowledged.sequence=ack->sequence;
 if(count+removed>NHT_CONTACTS || (clear_cancel && count)) {
  // Phase1: only old survivors DOWN plus every vanished old ID UP. Exactly the
  // old set (<=10) fits. Do not admit new DOWN until the old UP packet succeeds.
  if(!clear_cancel)for(unsigned i=0;i<count;++i)
   if(nht_index(ack->point,ack->count,now[i].id)>=0)nht_entry(first,&now[i],NHT_DOWN);
  for(unsigned j=0;j<ack->count;++j)
   if(clear_cancel || nht_index(now,count,ack->point[j].id)<0)nht_entry(first,&ack->point[j],NHT_UP);
  struct nht_packet *second=&plan->packet[plan->count++];
  second->bytes[0]=3;second->acknowledged.sequence=sequence;
  for(unsigned i=0;i<count;++i)nht_entry(second,&now[i],NHT_DOWN);
 } else {
  for(unsigned i=0;i<count;++i)nht_entry(first,&now[i],NHT_DOWN);
  for(unsigned j=0;j<ack->count;++j)if(nht_index(now,count,ack->point[j].id)<0)nht_entry(first,&ack->point[j],NHT_UP);
  first->acknowledged.sequence=sequence;
 }
 for(unsigned i=0;i<plan->count;++i)plan->packet[i].bytes[NHT_REPORT_BYTES-1]=(unsigned char)plan->packet[i].entries;
 return (int)plan->count;
}
static inline int nht_build(const struct nht_state *ack,const struct nht_point *now,unsigned count,uint32_t sequence,struct nht_plan *plan) {
 return nht_build_with_cancel(ack,now,count,sequence,0,plan);
}
// API success acknowledges submission, not delivery to backboardd/UIKit. Errors
// may be ambiguous; this stores successful API reports only, never exactly-once.
// A caller may rebuild from this state at its next notification after failure.
static inline int nht_commit(struct nht_state *ack,struct nht_plan *plan,unsigned index,int report_success) {
 if(!ack || !plan || index!=plan->next || index>=plan->count || !report_success)return 0;
 *ack=plan->packet[index].acknowledged;++plan->next;return 1;
}
#endif
