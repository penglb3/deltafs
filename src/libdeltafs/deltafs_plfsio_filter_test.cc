/*
 * Copyright (c) 2015-2017 Carnegie Mellon University.
 *
 * All rights reserved.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file. See the AUTHORS file for names of contributors.
 */

#include "deltafs_plfsio_filter.h"
#include "deltafs_plfsio.h"

#include "pdlfs-common/coding.h"
#include "pdlfs-common/histogram.h"
#include "pdlfs-common/port.h"
#include "pdlfs-common/testharness.h"
#include "pdlfs-common/testutil.h"

#include <stdlib.h>
#ifdef PDLFS_PLATFORM_POSIX
#include <sys/resource.h>
#include <sys/time.h>
#endif

#include <algorithm>
#include <set>
#include <vector>

namespace pdlfs {
namespace plfsio {

template <typename T, FilterTester tester, size_t key_bits = 24>
class FilterTest {
 public:
  FilterTest() : key_bits_(key_bits), ft_(NULL) {
    options_.bf_bits_per_key = 10;  // Override the defaults
    options_.bm_key_bits = key_bits_;
  }

  ~FilterTest() {
    delete ft_;  // Done
  }

  void Reset(uint32_t num_keys) {
    if (ft_ == NULL) ft_ = new T(options_, 0);  // Does not reserve memory
    ft_->Reset(num_keys);
  }

  // REQUIRES: Reset() must have been called.
  void AddKey(uint32_t seq) {
    std::string key;
    PutFixed32(&key, seq);
    ft_->AddKey(key);
  }

  Slice Finish() {
    data_.clear();
    if (ft_ != NULL) {
      data_ = ft_->Finish().ToString();
      delete ft_;
      ft_ = NULL;
    }
    return data_;
  }

  // REQUIRES: Finish() must have been called.
  bool KeyMayMatch(uint32_t seq) const {
    std::string key;
    PutFixed32(&key, seq);
    return tester(key, data_);
  }

  std::string data_;  // Final filter contents
  const size_t key_bits_;
  DirOptions options_;
  T* ft_;
};

template <typename T>
static void TEST_LogAndApply(T* t, Random* rnd, uint32_t num_keys,
                             bool no_fp = true) {
  t->Reset(num_keys);
  const int key_bits = t->key_bits_;  // Obtain key space
  std::set<uint32_t> keys;
  while (keys.size() != num_keys) {
    keys.insert(rnd->Uniform(1 << key_bits));
  }
  std::set<uint32_t>::iterator it = keys.begin();
  for (; it != keys.end(); ++it) {
    t->AddKey(*it);
  }
  Slice contents = t->Finish();
  fprintf(stderr, "%8u keys (%f%% full) %27s]\t%12.2f bits/key\n", num_keys,
          100.0 * double(num_keys) / (1u << key_bits),
          PrettySize(contents.size()).c_str(),
          8.0 * double(contents.size()) / num_keys);
  // All keys previously inserted must match
  for (it = keys.begin(); it != keys.end(); ++it) {
    ASSERT_TRUE(t->KeyMayMatch(*it));
  }

  // If no_fp is true (no false-positive), all non-existent
  // keys must never match (filter is accurate)
  if (no_fp) {
    std::set<uint32_t> non_keys;  // Randomly generate non-keys
    while (non_keys.size() != keys.size()) {
      uint32_t key = rnd->Uniform(1 << key_bits);
      if (keys.count(key) == 0) {
        non_keys.insert(key);
      }
    }
    for (it = non_keys.begin(); it != non_keys.end(); ++it) {
      ASSERT_FALSE(t->KeyMayMatch(*it));
    }

    // Test keys not in the defined key space
    for (uint32_t i = 0; i < num_keys; i++) {
      ASSERT_FALSE(t->KeyMayMatch((1u << key_bits) + i));
    }
  }
}

// Bloom filter
typedef FilterTest<BloomBlock, BloomKeyMayMatch> BloomFilterTest;

TEST(BloomFilterTest, BloomFmt) {
  Random rnd(301);
  uint32_t num_keys = 0;
  while (num_keys <= (64 << 10)) {
    TEST_LogAndApply(this, &rnd, num_keys, false);
    if (num_keys == 0) {
      num_keys = 1;
    } else {
      num_keys *= 4;
    }
  }
}

// Uncompressed bitmap filter
typedef FilterTest<BitmapBlock<UncompressedFormat>, BitmapKeyMustMatch>
    UncompressedBitmapFilterTest;

TEST(UncompressedBitmapFilterTest, UncompressedBitmapFmt) {
  Random rnd(301);
  uint32_t num_keys = 0;
  while (num_keys <= (16 << 10)) {
    TEST_LogAndApply(this, &rnd, num_keys);
    if (num_keys == 0) {
      num_keys = 1;
    } else {
      num_keys *= 4;
    }
  }
}

// Varint bitmap filter
typedef FilterTest<BitmapBlock<VbFormat>, BitmapKeyMustMatch>
    VarintBitmapFilterTest;

TEST(VarintBitmapFilterTest, VarintBitmapFmt) {
  Random rnd(301);
  uint32_t num_keys = 0;
  while (num_keys <= (4 << 10)) {
    TEST_LogAndApply(this, &rnd, num_keys);
    if (num_keys == 0) {
      num_keys = 1;
    } else {
      num_keys *= 4;
    }
  }
}

// Varint plus bitmap filter
typedef FilterTest<BitmapBlock<VbPlusFormat>, BitmapKeyMustMatch>
    VarintPlusBitmapFilterTest;

TEST(VarintPlusBitmapFilterTest, VarintPlusBitmapFmt) {
  Random rnd(301);
  uint32_t num_keys = 0;
  while (num_keys <= (4 << 10)) {
    TEST_LogAndApply(this, &rnd, num_keys);
    if (num_keys == 0) {
      num_keys = 1;
    } else {
      num_keys *= 4;
    }
  }
}

// Partitioned Varint plus bitmap filter
typedef FilterTest<BitmapBlock<FastVbPlusFormat>, BitmapKeyMustMatch>
    PVarintPlusBitmapFilterTest;

TEST(PVarintPlusBitmapFilterTest, PVarintPlusBitmapFmt) {
  Random rnd(301);
  uint32_t num_keys = 0;
  while (num_keys <= (4 << 10)) {
    TEST_LogAndApply(this, &rnd, num_keys);
    if (num_keys == 0) {
      num_keys = 1;
    } else {
      num_keys *= 4;
    }
  }
}

// PForDelta bitmap filter
typedef FilterTest<BitmapBlock<PfDeltaFormat>, BitmapKeyMustMatch>
    PForDeltaBitmapFilterTest;

TEST(PForDeltaBitmapFilterTest, PForDeltaBitmapFmt) {
  Random rnd(301);
  uint32_t num_keys = 0;
  while (num_keys <= (4 << 10)) {
    TEST_LogAndApply(this, &rnd, num_keys);
    if (num_keys == 0) {
      num_keys = 1;
    } else {
      num_keys *= 4;
    }
  }
}

// Partitioned PForDelta bitmap filter
typedef FilterTest<BitmapBlock<FastPfDeltaFormat>, BitmapKeyMustMatch>
    PpForDeltaBitmapFilterTest;

TEST(PpForDeltaBitmapFilterTest, PpForDeltaBitmapFmt) {
  Random rnd(301);
  uint32_t num_keys = 0;
  while (num_keys <= (4 << 10)) {
    TEST_LogAndApply(this, &rnd, num_keys);
    if (num_keys == 0) {
      num_keys = 1;
    } else {
      num_keys *= 4;
    }
  }
}

// Roaring bitmap filter
typedef FilterTest<BitmapBlock<RoaringFormat>, BitmapKeyMustMatch>
    RoaringBitmapFilterTest;

TEST(RoaringBitmapFilterTest, RoaringBitmapFmt) {
  Random rnd(301);
  uint32_t num_keys = 0;
  while (num_keys <= (4 << 10)) {
    TEST_LogAndApply(this, &rnd, num_keys);
    if (num_keys == 0) {
      num_keys = 1;
    } else {
      num_keys *= 4;
    }
  }
}

template <typename T, size_t key_bits = 24>
class PlfsFilterBench {
 public:
  static int GetOption(const char* key, int defval) {
    const char* env = getenv(key);
    if (env == NULL) {
      return defval;
    } else if (strlen(env) == 0) {
      return defval;
    } else {
      return atoi(env);
    }
  }

  explicit PlfsFilterBench()
      : num_tables_(GetOption("TABLE_NUM", 64)), key_bits_(key_bits) {
    options_.bf_bits_per_key = 10;
    options_.bm_key_bits = key_bits_;

    fprintf(stderr, "Generating keys ... (may take a while)\n");
    keys_.reserve(1u << key_bits_);
    for (uint32_t x = 0; x < (1u << key_bits_); x++) keys_.push_back(x);
    std::random_shuffle(keys_.begin(), keys_.end());
    fprintf(stderr, "Done!\n");

    ft_ = new T(options_, 0);
  }

  ~PlfsFilterBench() { delete ft_; }

#if defined(PDLFS_PLATFORM_POSIX)
  static inline double ToSecs(const struct timeval* tv) {
    return tv->tv_sec + tv->tv_usec / 1000.0 / 1000.0;
  }
#endif

  void LogAndApply() {
    const double k = 1000.0, ki = 1024.0;
    const uint64_t start = Env::Default()->NowMicros();
#if defined(PDLFS_PLATFORM_POSIX)
    struct rusage tmp_usage;
    int r0 = getrusage(RUSAGE_SELF, &tmp_usage);
    ASSERT_EQ(r0, 0);
#endif
    fprintf(stderr, "Inserting keys ... (%d tables)\n", int(num_tables_));
    size_t size = 0;  // Total filter size
    const size_t num_keys = (1u << key_bits_) / num_tables_;  // Keys per table
    std::vector<uint32_t>::iterator it = keys_.begin();
    std::string buf;  // Buffer space for keys
    for (size_t j = 0; j < num_tables_; j++) {
      fprintf(stderr, "\r%d/%d", int(j), int(num_tables_));
      ft_->Reset(num_keys);
      for (size_t i = 0; i < num_keys; i++) {
        buf.clear();
        PutFixed32(&buf, *it);
        ft_->AddKey(buf);
        ++it;
      }
      Slice contents = ft_->Finish();
      size += contents.size();
    }
    fprintf(stderr, "\r%d/%d\n", int(num_tables_), int(num_tables_));
    fprintf(stderr, "Done!\n");
    const uint64_t end = Env::Default()->NowMicros();
    const uint64_t dura = end - start;

    fprintf(stderr, "----------------------------------------\n");
    fprintf(stderr, "  Keys per filter: %d\n", int(num_keys));
    fprintf(stderr, "      Num filters: %d\n", int(num_tables_));
    fprintf(stderr, "Total filter size: %.2f MiB\n", 1.0 * size / ki / ki);
    fprintf(stderr, "          Density: %.2f%%\n", 100.0 / num_tables_);
    fprintf(stderr, "     Storage cost: %.2f (bits per key)\n",
            8.0 * size / (num_keys * num_tables_));
    fprintf(stderr, " Memory footprint: %.2f MiB\n",
            1.0 * ft_->memory_usage() / ki / ki);

#if defined(PDLFS_PLATFORM_POSIX)
    struct rusage usage;
    int r1 = getrusage(RUSAGE_SELF, &usage);
    ASSERT_EQ(r1, 0);
    double utime = ToSecs(&usage.ru_utime) - ToSecs(&tmp_usage.ru_utime);
    double stime = ToSecs(&usage.ru_stime) - ToSecs(&tmp_usage.ru_stime);
    fprintf(stderr, "    User CPU Time: %.3f s\n", utime);
    fprintf(stderr, "  System CPU Time: %.3f s\n", stime);
#if defined(PDLFS_OS_LINUX)
    cpu_set_t cpu_set;
    CPU_ZERO(&cpu_set);
    int r2 = sched_getaffinity(getpid(), sizeof(cpu_set), &cpu_set);
    ASSERT_EQ(r2, 0);
    fprintf(stderr, "    Num CPU Cores: %d\n", CPU_COUNT(&cpu_set));
    fprintf(stderr, "        CPU Usage: %.1f%%\n",
            k * k * (utime + stime) / dura * 100);
#endif
#endif
  }

 private:
  const size_t num_tables_;  // Num tables per epoch
  const size_t key_bits_;
  std::vector<uint32_t> keys_;
  DirOptions options_;
  T* ft_;
};

template <typename T, FilterTester tester>
class PlfsFilterQueryBench {
 public:
  PlfsFilterQueryBench(size_t table_num = 64)
      : table_num_(table_num), rnd_(301) {
    options_.bf_bits_per_key = 10;  // Override the defaults
    options_.bm_key_bits = 24;
    ft_ = new T(options_, 0);  // Does not reserve memory
  }

  ~PlfsFilterQueryBench() { delete ft_; }

  void LogAndApply() {
    BuildFilter();
    RunQueries();
  }

 protected:
  void BuildFilter() {
    fprintf(stderr, "\rInserting key...");
    int key_num = (1 << 24) / table_num_;
    ft_->Reset(key_num);
    uint32_t max = 0;
    for (int i = 0; i < key_num; i++) {
      uint32_t key = rnd_.Uniform(1 << 24);  // Random 24-bit keys
      max = key > max ? key : max;
      std::string key_seq;
      PutFixed32(&key_seq, key);
      keys_.push_back(key_seq);
      ft_->AddKey(key_seq);
    }
    ft_->Finish();
    fprintf(stderr, "\rFilter construction finished! max key: %u\n", max);
  }

  void RunQueries() {
    int key_num = (1 << 24) / table_num_;
    const uint64_t start = Env::Default()->NowMicros();
    uint64_t now = start;
    uint64_t max = 0;
    uint64_t min = -1;
    fprintf(stderr, "Query keys...\n");
    int i = 0;
    for (std::vector<std::string>::iterator it = keys_.begin();
         it != keys_.end(); ++it, ++i) {
      if (i % (1 << 15) == (1 << 15) - 1) {
        fprintf(stderr, "\r%.2f%%", 100.0 * (i + 1) / key_num);
      }
      tester(*it, *(ft_->buffer_store()));
      uint64_t interval = Env::Default()->NowMicros() - now;
      if (interval < min) {
        min = interval;
      } else if (interval > max) {
        max = interval;
      }
      latency_.Add(interval);
      now = Env::Default()->NowMicros();
    }

    uint64_t dura = Env::Default()->NowMicros() - start;

    Report(dura, min, max);
  }

  void Report(uint64_t dura, double min, double max) {
    const double k = 1000.0;
    fprintf(stderr, "\n----------------------------------------\n");
    fprintf(stderr, "             Total Time: %.3f s\n", dura / k / k);
    fprintf(stderr, "          Avg Read Time: %.3f us (per key)\n",
            latency_.Average());
    fprintf(stderr, "       Median Read Time: %.3f us (per key)\n",
            latency_.Median());
    fprintf(stderr, "          Min Read Time: %.3f us (per key)\n", min);
    fprintf(stderr, "          Max Read Time: %.3f us (per key)\n", max);
  }

 private:
  size_t table_num_;
  Random rnd_;
  DirOptions options_;
  std::vector<std::string> keys_;
  T* ft_;
  Histogram latency_;
};

}  // namespace plfsio
}  // namespace pdlfs

#if defined(PDLFS_GFLAGS)
#include <gflags/gflags.h>
#endif
#if defined(PDLFS_GLOG)
#include <glog/logging.h>
#endif

static void BM_Usage() {
  fprintf(stderr,
          "Use --bench=ft,<fmt> or --bench=qu,<fmt> to run benchmark.\n\n");
  fprintf(stderr, "==Valid fmt are:\n");
  fprintf(stderr, "bf      (bloom filter)\n");
  fprintf(stderr, "bmp     (bitmap)\n");
  fprintf(stderr, "vb      (bitmap, varint)\n");
  fprintf(stderr, "vbp     (bitmap, modified varint)\n");
  fprintf(stderr, "pvbp    (bipvbptmap, partitioned varint)\n");
  fprintf(stderr, "pfdelta (bitmap, modified p-for-delta)\n");
  fprintf(stderr, "r       (bitmap, modified roaring)\n");
  fprintf(stderr, "\n");
}

static void BM_LogAndApply(bool read_or_write, const char* fmt) {
  if (fmt[0] != ',') {
    BM_Usage();
  } else if (strcmp(fmt + 1, "bf") == 0) {
    if (read_or_write) {
      typedef pdlfs::plfsio::PlfsFilterQueryBench<
          pdlfs::plfsio::BloomBlock, pdlfs::plfsio::BloomKeyMayMatch>
          PlfsBloomFilterQueryBench;
      PlfsBloomFilterQueryBench bench;
      bench.LogAndApply();
    } else {
      typedef pdlfs::plfsio::PlfsFilterBench<pdlfs::plfsio::BloomBlock>
          PlfsBloomFilterBench;
      PlfsBloomFilterBench bench;
      bench.LogAndApply();
    }
  } else if (strcmp(fmt + 1, "bmp") == 0) {
    if (read_or_write) {
      typedef pdlfs::plfsio::PlfsFilterQueryBench<
          pdlfs::plfsio::BitmapBlock<pdlfs::plfsio::UncompressedFormat>,
          pdlfs::plfsio::BitmapKeyMustMatch>
          PlfsBitmapQueryBench;
      PlfsBitmapQueryBench bench;
      bench.LogAndApply();
    } else {
      typedef pdlfs::plfsio::PlfsFilterBench<
          pdlfs::plfsio::BitmapBlock<pdlfs::plfsio::UncompressedFormat> >
          PlfsBitmapBench;
      PlfsBitmapBench bench;
      bench.LogAndApply();
    }
  } else if (strcmp(fmt + 1, "vb") == 0) {
    if (read_or_write) {
      typedef pdlfs::plfsio::PlfsFilterQueryBench<
          pdlfs::plfsio::BitmapBlock<pdlfs::plfsio::VbFormat>,
          pdlfs::plfsio::BitmapKeyMustMatch>
          PlfsVarintBitmapQueryBench;
      PlfsVarintBitmapQueryBench bench;
      bench.LogAndApply();
    } else {
      typedef pdlfs::plfsio::PlfsFilterBench<
          pdlfs::plfsio::BitmapBlock<pdlfs::plfsio::VbFormat> >
          PlfsVarintBitmapBench;
      PlfsVarintBitmapBench bench;
      bench.LogAndApply();
    }
  } else if (strcmp(fmt + 1, "vbp") == 0) {
    if (read_or_write) {
      typedef pdlfs::plfsio::PlfsFilterQueryBench<
          pdlfs::plfsio::BitmapBlock<pdlfs::plfsio::VbPlusFormat>,
          pdlfs::plfsio::BitmapKeyMustMatch>
          PlfsVarintPlusBitmapQueryBench;
      PlfsVarintPlusBitmapQueryBench bench;
      bench.LogAndApply();
    } else {
      typedef pdlfs::plfsio::PlfsFilterBench<
          pdlfs::plfsio::BitmapBlock<pdlfs::plfsio::VbPlusFormat> >
          PlfsVarintPlusBitmapBench;
      PlfsVarintPlusBitmapBench bench;
      bench.LogAndApply();
    }
  } else if (strcmp(fmt + 1, "pvbp") == 0) {
    if (read_or_write) {
      typedef pdlfs::plfsio::PlfsFilterQueryBench<
          pdlfs::plfsio::BitmapBlock<pdlfs::plfsio::FastVbPlusFormat>,
          pdlfs::plfsio::BitmapKeyMustMatch>
          PlfsPVarintPlusBitmapQueryBench;
      PlfsPVarintPlusBitmapQueryBench bench;
      bench.LogAndApply();
    } else {
      typedef pdlfs::plfsio::PlfsFilterBench<
          pdlfs::plfsio::BitmapBlock<pdlfs::plfsio::FastVbPlusFormat> >
          PlfsPVarintPlusBitmapBench;
      PlfsPVarintPlusBitmapBench bench;
      bench.LogAndApply();
    }
  } else if (strcmp(fmt + 1, "pfdelta") == 0) {
    if (read_or_write) {
      typedef pdlfs::plfsio::PlfsFilterQueryBench<
          pdlfs::plfsio::BitmapBlock<pdlfs::plfsio::PfDeltaFormat>,
          pdlfs::plfsio::BitmapKeyMustMatch>
          PlfsPForDeltaBitmapQueryBench;
      PlfsPForDeltaBitmapQueryBench bench;
      bench.LogAndApply();
    } else {
      typedef pdlfs::plfsio::PlfsFilterBench<
          pdlfs::plfsio::BitmapBlock<pdlfs::plfsio::PfDeltaFormat> >
          PlfsPForDeltaBitmapBench;
      PlfsPForDeltaBitmapBench bench;
      bench.LogAndApply();
    }
  } else if (strcmp(fmt + 1, "ppfdelta") == 0) {
    if (read_or_write) {
      typedef pdlfs::plfsio::PlfsFilterQueryBench<
          pdlfs::plfsio::BitmapBlock<pdlfs::plfsio::FastPfDeltaFormat>,
          pdlfs::plfsio::BitmapKeyMustMatch>
          PlfsPpForDeltaBitmapQueryBench;
      PlfsPpForDeltaBitmapQueryBench bench;
      bench.LogAndApply();
    } else {
      typedef pdlfs::plfsio::PlfsFilterBench<
          pdlfs::plfsio::BitmapBlock<pdlfs::plfsio::FastPfDeltaFormat> >
          PlfsPpForDeltaBitmapBench;
      PlfsPpForDeltaBitmapBench bench;
      bench.LogAndApply();
    }
  } else if (strcmp(fmt + 1, "r") == 0) {
    if (read_or_write) {
      typedef pdlfs::plfsio::PlfsFilterQueryBench<
          pdlfs::plfsio::BitmapBlock<pdlfs::plfsio::RoaringFormat>,
          pdlfs::plfsio::BitmapKeyMustMatch>
          PlfsRoaringBitmapQueryBench;
      PlfsRoaringBitmapQueryBench bench;
      bench.LogAndApply();
    } else {
      typedef pdlfs::plfsio::PlfsFilterBench<
          pdlfs::plfsio::BitmapBlock<pdlfs::plfsio::RoaringFormat> >
          PlfsRoaringBitmapBench;
      PlfsRoaringBitmapBench bench;
      bench.LogAndApply();
    }
  } else {
    BM_Usage();
  }
}

static void BM_Main(int* argc, char*** argv) {
#if defined(PDLFS_GFLAGS)
  google::ParseCommandLineFlags(argc, argv, true);
#endif
#if defined(PDLFS_GLOG)
  google::InitGoogleLogging((*argv)[0]);
  google::InstallFailureSignalHandler();
#endif
  pdlfs::Slice bench_name;
  if (*argc > 1) {
    bench_name = pdlfs::Slice((*argv)[*argc - 1]);
  }
  if (*argc <= 1) {
    BM_Usage();
  } else if (bench_name.starts_with("--bench=ft")) {
    BM_LogAndApply(false, bench_name.c_str() + strlen("--bench=ft"));
  } else if (bench_name.starts_with("--bench=qu")) {
    BM_LogAndApply(true, bench_name.c_str() + strlen("--bench=qu"));
  } else {
    BM_Usage();
  }
}

int main(int argc, char* argv[]) {
  pdlfs::Slice token;
  if (argc > 1) {
    token = pdlfs::Slice(argv[argc - 1]);
  }
  if (!token.starts_with("--bench")) {
    return pdlfs::test::RunAllTests(&argc, &argv);
  } else {
    BM_Main(&argc, &argv);
    return 0;
  }
}
