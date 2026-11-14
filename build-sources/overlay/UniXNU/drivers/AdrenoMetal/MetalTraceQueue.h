// SPDX-License-Identifier: MIT
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
// Producers publish whole records under the same lock as the consumer. No file
// I/O under this lock. Full queues drop new records and count the loss explicitly.
// This avoids the old "leave four slots behind" heuristic exposing partial records.
template<class Record, size_t Capacity> class NBMetalTraceQueue {
 static_assert(Capacity>0,"nonempty trace queue");
 std::array<Record,Capacity> data{};
 size_t head=0, tail=0, used=0;
 uint64_t offered=0, dropped=0;
 std::mutex mutex;
public:
 struct Stats {uint64_t offered,dropped,pending;};
 bool push(Record record) {
  std::lock_guard<std::mutex> lock(mutex);
  const uint64_t sequence=offered++;
  if(used==Capacity){++dropped;return false;}
  if(record.type!=1)record.idx=(uint32_t)sequence;
  data[head]=record;head=(head+1)%Capacity;++used;return true;
 }
 size_t pop(Record *out,size_t capacity) {
  std::lock_guard<std::mutex> lock(mutex);
  size_t count=used<capacity?used:capacity;
  for(size_t i=0;i<count;++i){out[i]=data[tail];tail=(tail+1)%Capacity;}
  used-=count;return count;
 }
 Stats stats(){std::lock_guard<std::mutex> lock(mutex);return {offered,dropped,used};}
};
