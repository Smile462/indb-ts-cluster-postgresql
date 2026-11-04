#pragma once
#include <atomic>
#include <thread>
#include <chrono>
#include <cstdio>
#include <string>
#include <windows.h>
#include <psapi.h>

// Used only in separate resource probes. The baseline is taken after open,
// immediately before the engine query; no startup/open allocation is included.
class JournalQueryMemory {
  bool enabled_=false;
  std::atomic<bool> stop_{false};
  std::thread sampler_;
  unsigned long long base_ws_=0,base_private_=0,peak_ws_=0,peak_private_=0,samples_=0;
  void sample() {
    PROCESS_MEMORY_COUNTERS_EX memory{};memory.cb=sizeof(memory);
    if(GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),sizeof(memory))) {
      if(samples_==0){base_ws_=memory.WorkingSetSize;base_private_=memory.PrivateUsage;}
      if(memory.WorkingSetSize>peak_ws_)peak_ws_=memory.WorkingSetSize;
      if(memory.PrivateUsage>peak_private_)peak_private_=memory.PrivateUsage;
      ++samples_;
    }
  }
 public:
  explicit JournalQueryMemory(bool enabled):enabled_(enabled) {
    if(enabled_){sample();sampler_=std::thread([this]{while(!stop_){std::this_thread::sleep_for(std::chrono::milliseconds(10));if(!stop_)sample();}});}
  }
  void Stop() {
    if(sampler_.joinable()){stop_=true;sampler_.join();sample();}
  }
  std::string Json() {
    Stop();if(!enabled_)return "null";
    char text[640];std::snprintf(text,sizeof(text),"{\"scope\":\"engine_query_after_open\",\"interval_ms\":10,\"samples\":%llu,\"baseline_working_set\":%llu,\"baseline_private_bytes\":%llu,\"peak_working_set\":%llu,\"peak_private_bytes\":%llu,\"peak_increment_working_set\":%llu,\"peak_increment_private_bytes\":%llu,\"short_query_can_miss_peak\":true}",samples_,base_ws_,base_private_,peak_ws_,peak_private_,peak_ws_-base_ws_,peak_private_-base_private_);return text;
  }
  ~JournalQueryMemory(){Stop();}
};
