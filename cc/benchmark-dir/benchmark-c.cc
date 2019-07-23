// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT license.

#include <atomic>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <deque>
#include <map>
#include <experimental/filesystem>

#include "file.h"

#include "core/auto_ptr.h"
#include "core/faster-c.h"
#include "device/null_disk.h"

using namespace std::chrono_literals;
using namespace FASTER::core;

/// Basic YCSB benchmark.

enum class Op : uint8_t {
  Insert = 0,
  Read = 1,
  Upsert = 2,
  Scan = 3,
  ReadModifyWrite = 4,
};

enum class Workload {
  A_50_50 = 0,
  RMW_100 = 1,
  UPSERT_100 = 2,
  READ_100 = 3,
};

static constexpr uint64_t kInitCount = 250000000;
static constexpr uint64_t kTxnCount = 1000000000;
static constexpr uint64_t kChunkSize = 3200 * 8;
static constexpr uint64_t kRefreshInterval = 64 * 8;
static constexpr uint64_t kCompletePendingInterval = 1600 * 8;

static_assert(kInitCount * 8 % kChunkSize == 0, "kInitCount % kChunkSize != 0");
static_assert(kTxnCount * 8 % kChunkSize == 0, "kTxnCount % kChunkSize != 0");
static_assert(kCompletePendingInterval % kRefreshInterval == 0,
              "kCompletePendingInterval % kRefreshInterval != 0");

static constexpr uint64_t kNanosPerSecond = 1000000000;

aligned_unique_ptr_t<uint8_t> init_keys_;
aligned_unique_ptr_t<uint8_t> txn_keys_;
std::atomic<uint64_t> idx_{ 0 };
std::atomic<size_t> threads_waiting_{ 0 };
std::atomic<uint64_t> total_duration_{ 0 };
std::atomic<uint64_t> total_reads_done_{ 0 };
std::atomic<uint64_t> total_writes_done_{ 0 };

std::map<size_t, std::vector<double>> results_;

inline Op ycsb_a_50_50(std::mt19937& rng) {
  if(rng() % 100 < 50) {
    return Op::Read;
  } else {
    return Op::Upsert;
  }
}

inline Op ycsb_rmw_100(std::mt19937& rng) {
  return Op::ReadModifyWrite;
}

inline Op ycsb_upsert_100(std::mt19937& rng) {
  return Op::Upsert;
}

inline Op ycsb_read_100(std::mt19937& rng) {
  return Op::Read;
}

inline void read_cb(void* target, const uint8_t* buffer, uint64_t length, faster_status status) {
}

uint64_t rmw_cb(const uint8_t* current, uint64_t length, uint8_t* modification, uint64_t modification_length, uint8_t* dst) {
  assert(length == 8);
  assert(modification_length == 8);
  if (dst != NULL) {
    uint8_t val[8];
    val[0] = {static_cast<uint8_t>(current[0] + modification[0])};
    memcpy(dst, val, 8);
  }
  return 8;
}

extern "C" {
  void deallocate_vec(uint8_t* vec, uint64_t length) {
  }
}

/// Affinitize to hardware threads on the same core first, before
/// moving on to the next core.
void SetThreadAffinity(size_t core) {
  // For now, assume 36 cores. (Set this correctly for your test system.)
  constexpr size_t kCoreCount = 36;
#ifdef _WIN32
  HANDLE thread_handle = ::GetCurrentThread();
  GROUP_AFFINITY group;
  group.Group = WORD(core / kCoreCount);
  group.Mask = KAFFINITY(0x1llu << (core - kCoreCount * group.Group));
  ::SetThreadGroupAffinity(thread_handle, &group, nullptr);
#else
  // On our 28-core test system, we see CPU 0, Core 0 assigned to 0, 28;
  //                                    CPU 1, Core 0 assigned to 1, 29; etc.
  cpu_set_t mask;
  CPU_ZERO(&mask);
#ifdef NUMA
  switch(core % 4) {
  case 0:
    // 0 |-> 0
    // 4 |-> 2
    // 8 |-> 4
    core = core / 2;
    break;
  case 1:
    // 1 |-> 28
    // 5 |-> 30
    // 9 |-> 32
    core = kCoreCount + (core - 1) / 2;
    break;
  case 2:
    // 2  |-> 1
    // 6  |-> 3
    // 10 |-> 5
    core = core / 2;
    break;
  case 3:
    // 3  |-> 29
    // 7  |-> 31
    // 11 |-> 33
    core = kCoreCount + (core - 1) / 2;
    break;
  }
#else
  switch(core % 2) {
  case 0:
    // 0 |-> 0
    // 2 |-> 2
    // 4 |-> 4
    core = core;
    break;
  case 1:
    // 1 |-> 28
    // 3 |-> 30
    // 5 |-> 32
    core = (core - 1) + kCoreCount;
    break;
  }
#endif
  CPU_SET(core, &mask);

  ::sched_setaffinity(0, sizeof(mask), &mask);
#endif
}

void load_files(const std::string& load_filename, const std::string& run_filename) {
  constexpr size_t kFileChunkSize = 131072;

  auto chunk_guard = alloc_aligned<uint8_t>(512, kFileChunkSize);
  uint8_t* chunk = chunk_guard.get();

  FASTER::benchmark::File init_file{ load_filename };
  printf("loading keys from %s into memory...\n", load_filename.c_str());

  init_keys_ = alloc_aligned<uint8_t>(64, kInitCount * sizeof(uint64_t));
  uint64_t count = 0;
  uint64_t offset = 0;
  while(true) {
    uint64_t size = init_file.Read(chunk, kFileChunkSize, offset);
    for(uint64_t idx = 0; idx < size; ++idx) {
      init_keys_.get()[count] = chunk[idx];
      ++count;
    }
    if(size == kFileChunkSize) {
      offset += kFileChunkSize;
    } else {
      break;
    }
  }
  if(kInitCount != count / 8) {
    printf("Init file load fail!\n");
    exit(1);
  }

  printf("loaded %" PRIu64 " keys.\n", count / 8);

  FASTER::benchmark::File txn_file{ run_filename };

  printf("loading txns from %s into memory...\n", run_filename.c_str());

  txn_keys_ = alloc_aligned<uint8_t>(64, kTxnCount * sizeof(uint64_t));

  count = 0;
  offset = 0;

  while(true) {
    uint64_t size = txn_file.Read(chunk, kFileChunkSize, offset);
    for(uint64_t idx = 0; idx < size; ++idx) {
      txn_keys_.get()[count] = chunk[idx];
      ++count;
    }
    if(size == kFileChunkSize) {
      offset += kFileChunkSize;
    } else {
      break;
    }
  }
  if(kTxnCount != count / 8) {
    printf("Txn file load fail!\n");
    exit(1);
  }
  printf("loaded %" PRIu64 " txns.\n", count / 8);
}

void thread_setup_store(faster_t* store, size_t thread_idx) {
  SetThreadAffinity(thread_idx);

  const char* guid = faster_start_session(store);

  uint8_t value = 42;
  for(uint64_t chunk_idx = idx_.fetch_add(kChunkSize); chunk_idx < kInitCount * 8;
      chunk_idx = idx_.fetch_add(kChunkSize)) {
    for(uint64_t idx = chunk_idx; idx < chunk_idx + kChunkSize; idx += 8) {
      if(idx % kRefreshInterval == 0) {
          faster_refresh_session(store);
        if(idx % kCompletePendingInterval == 0) {
          faster_complete_pending(store, false);
        }
      }

      uint8_t val[8];
      val[0] = value;
      faster_upsert(store, &init_keys_.get()[idx], 8, val, 8, 1);
    }
  }

  faster_complete_pending(store, true);
  faster_stop_session(store);
}

void setup_store(faster_t* store, size_t num_threads) {
  idx_ = 0;
  std::deque<std::thread> threads;
  for(size_t thread_idx = 0; thread_idx < num_threads; ++thread_idx) {
    threads.emplace_back(&thread_setup_store, store, thread_idx);
  }
  for(auto& thread : threads) {
    thread.join();
  }
  printf("Finished populating store: contains ?? elements.\n");
}


static std::atomic<int64_t> async_reads_done{ 0 };
static std::atomic<int64_t> async_writes_done{ 0 };

template <Op(*FN)(std::mt19937&)>
void thread_run_benchmark(faster_t* store, size_t thread_idx) {
  SetThreadAffinity(thread_idx);

  std::random_device rd{};
  std::mt19937 rng{ rd() };

  auto start_time = std::chrono::high_resolution_clock::now();

  uint8_t upsert_value = 42;
  int64_t reads_done = 0;
  int64_t writes_done = 0;

  const char* guid = faster_start_session(store);

  while(true) {
    uint64_t chunk_idx = idx_.fetch_add(kChunkSize);
    if((chunk_idx / 8) >= kTxnCount) {
      threads_waiting_.fetch_sub(1);
      break;
    }
    for(uint64_t idx = chunk_idx; idx < chunk_idx + kChunkSize; idx += 8) {
      if(idx % kRefreshInterval == 0) {
        faster_refresh_session(store);
        if(idx % kCompletePendingInterval == 0) {
          faster_complete_pending(store, false);
        }
      }
      switch(FN(rng)) {
      case Op::Insert:
      case Op::Upsert: {
        uint8_t val[8];
        val[0] = upsert_value;
        faster_upsert(store, &txn_keys_.get()[idx], 8, val, 8, 1);
        ++writes_done;
        break;
      }
      case Op::Scan:
        printf("Scan currently not supported!\n");
        exit(1);
        break;
      case Op::Read: {
        faster_read(store, &txn_keys_.get()[idx], 8, 1, read_cb, NULL);
        ++reads_done;
        break;
      }
      case Op::ReadModifyWrite:
        uint8_t modification[8];
        modification[0] = 0;
        uint8_t result = faster_rmw(store, &txn_keys_.get()[idx], 8, modification, 1, 1, rmw_cb);
        if(result == 0) {
          ++writes_done;
        }
        break;
      }
    }
  }

  faster_complete_pending(store, true);
  faster_stop_session(store);

  auto end_time = std::chrono::high_resolution_clock::now();
  std::chrono::nanoseconds duration = end_time - start_time;
  total_duration_ += duration.count();
  total_reads_done_ += reads_done;
  total_writes_done_ += writes_done;
  printf("Finished thread %" PRIu64 " : %" PRIu64 " reads, %" PRIu64 " writes, in %.2f seconds.\n",
         thread_idx, reads_done, writes_done, (double)duration.count() / kNanosPerSecond);
}

template <Op(*FN)(std::mt19937&)>
double run_benchmark(faster_t* store, size_t num_threads) {
  idx_ = 0;
  total_duration_ = 0;
  total_reads_done_ = 0;
  total_writes_done_ = 0;
  threads_waiting_ = num_threads;
  std::deque<std::thread> threads;
  for(size_t thread_idx = 0; thread_idx < num_threads; ++thread_idx) {
    threads.emplace_back(&thread_run_benchmark<FN>, store, thread_idx);
  }


  do {
    std::this_thread::sleep_for(std::chrono::seconds(30));
  } while(threads_waiting_.load() > 0);

  for(auto& thread : threads) {
    thread.join();
  }

  double ops_per_second_per_thread = ((double)total_reads_done_ + (double)total_writes_done_) / ((double)total_duration_ /
             kNanosPerSecond);

  printf("Finished benchmark: %" PRIu64 " threads;  %.2f ops/second/thread\n",
         num_threads, ops_per_second_per_thread);
  return ops_per_second_per_thread;
}

int main(int argc, char* argv[]) {
  constexpr size_t kNumArgs = 4;
  if(argc != kNumArgs + 1) {
    printf("Usage: benchmark.exe <workload> <# threads> <load_filename> <run_filename>\n");
    exit(0);
  }

  Workload workload = static_cast<Workload>(std::atol(argv[1]));
  size_t num_threads = ::atol(argv[2]);
  std::string load_filename{ argv[3] };
  std::string run_filename{ argv[4] };

  load_files(load_filename, run_filename);

  std::vector<size_t> thread_configurations = std::vector<size_t>();
  if (num_threads == 0) {
    thread_configurations.push_back(1);
    thread_configurations.push_back(2);
    thread_configurations.push_back(4);
    thread_configurations.push_back(8);
    thread_configurations.push_back(16);
    thread_configurations.push_back(32);
    thread_configurations.push_back(48);
  } else {
    thread_configurations.push_back(num_threads);
  }

  for (auto const& num_benchmark_threads: thread_configurations) {
    results_[num_threads] = std::vector<double>();
    for (int j = 0; j < 3; j++) {
        size_t init_size = next_power_of_two(kInitCount / 2);
        faster_t* store = faster_open_with_disk(init_size, 34359738368, "storage");

        printf("Populating the store...\n");

        setup_store(store, 48);
        faster_dump_distribution(store);
        printf("Store size: %" PRIu64 "\n", faster_size(store));

        printf("Running benchmark on %" PRIu64 " threads...\n", num_threads);
        double result;
        switch(workload) {
          case Workload::A_50_50:
            result = run_benchmark<ycsb_a_50_50>(store, num_threads);
            break;
          case Workload::RMW_100:
            result = run_benchmark<ycsb_rmw_100>(store, num_threads);
            break;
          case Workload::UPSERT_100:
            result = run_benchmark<ycsb_upsert_100>(store, num_threads);
            break;
          case Workload::READ_100:
            result = run_benchmark<ycsb_read_100>(store, num_threads);
            break;
          default:
            printf("Unknown workload!\n");
            exit(1);
        }

        faster_destroy(store);
        std::experimental::filesystem::remove_all("storage");
        std::vector<double> v = results_[num_threads];
        v.push_back(result);
        results_[num_threads] = v;
    }
  }
  for( auto const& x : results_ ) {
    printf("%d threads %.2f %.2f %.2f ops/second/thread\n", x.first, x.second[0], x.second[1], x.second[2]);
  }


  return 0;
}
