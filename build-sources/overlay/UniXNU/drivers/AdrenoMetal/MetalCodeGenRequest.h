// SPDX-License-Identifier: MIT
#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <utility>
#include <vector>

// The private compiler is a single opaque service. Reserve it during the
// request without holding the admission mutex while running compiler code.
class NBMetalCodeGenGate {
 std::mutex mutex;
 std::condition_variable changed;
 bool busy=false,usable=true;

 struct Reservation {
  NBMetalCodeGenGate &gate;
  bool usable=false;
  ~Reservation() {
   {
    std::lock_guard<std::mutex> lock(gate.mutex);
    gate.busy=false;
    gate.usable=usable;
   }
   gate.changed.notify_all();
  }
 };

public:
 // The operation returns false when the service's observed synchronous
 // completion contract is broken. Subsequent calls then refuse that service.
 template<class Operation> bool run(Operation operation) {
  {
   std::unique_lock<std::mutex> lock(mutex);
   changed.wait(lock,[this]{return !busy || !usable;});
   if(!usable)return false;
   busy=true;
  }
  Reservation reservation{*this};
  reservation.usable=operation();
  return true;
 }
};

// A copied reply block owns both input and completion state. A duplicate or
// out-of-contract late reply cannot update the returned result.
template<class Reply> class NBMetalCodeGenExchange {
 enum class State {Waiting,Answered,Finished};
 std::mutex mutex;
 State state=State::Waiting;
 Reply result{};

public:
 const std::vector<uint8_t> bytes;
 explicit NBMetalCodeGenExchange(std::vector<uint8_t> input):bytes(std::move(input)) {}

 template<class Decode> void complete(Decode decode) {
  std::lock_guard<std::mutex> lock(mutex);
  if(state!=State::Waiting)return;
  result=decode();
  state=State::Answered;
 }

 bool finish(Reply &out) {
  std::lock_guard<std::mutex> lock(mutex);
  const bool answered=state==State::Answered;
  state=State::Finished;
  if(answered)out=std::move(result);
  return answered;
 }
};
