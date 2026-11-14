// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstddef>
// Record is the existing 72-byte TraceRec. writeAt returns 0 or an errno and
// must finish the whole write. This code is also used by the host fixture.
template<class Record,uint32_t MetadataCapacity,uint32_t EventCapacity> class NBMetalTraceFile {
 static_assert(MetadataCapacity>0 && EventCapacity>0,"nonempty trace partitions");
 uint32_t metadataCount=0,metadataSpilled=0;
 uint64_t eventCount=0;
 int error=0;
 static bool metadata(uint32_t type){
  return type==0 || type==4 || type==19 || type==41 || type==42 || type==43 || type==50 || type==51;
 }
public:
 int failure()const{return error;}
 template<class Queue,class Write> void flush(Queue &queue,Write writeAt,uint64_t now,uint32_t pid){
  Record input[128],meta[128],events[128];
  for(unsigned batch=0;batch<128 && !error;++batch){
   const size_t n=queue.pop(input,128);if(!n)break;
   size_t m=0,e=0;
   for(size_t i=0;i<n;++i){
    if(metadata(input[i].type) && metadataCount+m<MetadataCapacity)meta[m++]=input[i];
    else {if(metadata(input[i].type))++metadataSpilled;events[e++]=input[i];}
   }
   const size_t keep=std::min<size_t>(m,MetadataCapacity-metadataCount);
   if(keep)error=writeAt(meta,keep*sizeof(Record),(1ull+metadataCount)*sizeof(Record));
   if(!error){metadataCount+=(uint32_t)keep;}
   for(size_t at=0;at<e && !error;){
    const size_t take=std::min<size_t>(e-at,EventCapacity-eventCount%EventCapacity);
    error=writeAt(events+at,take*sizeof(Record),(1ull+MetadataCapacity+eventCount%EventCapacity)*sizeof(Record));
    if(!error){at+=take;eventCount+=take;}
   }
  }
  const auto stats=queue.stats();
  Record h{};h.type=40;h.idx=2;h.t0=now;h.t1=stats.offered;h.t2=stats.dropped;h.t3=eventCount;
  h.a=metadataCount;h.b=metadataSpilled;h.c=(uint32_t)error;h.d=EventCapacity;
  h.tid=pid;h.cpu0=54;h.cpu1=MetadataCapacity;h.tid2=(uint32_t)stats.pending;
  const int headerError=writeAt(&h,sizeof h,0);if(headerError)error=headerError;
 }
};
