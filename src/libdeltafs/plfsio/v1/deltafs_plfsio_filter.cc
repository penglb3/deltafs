/*
 * Copyright (c) 2015-2017 Carnegie Mellon University.
 *
 * All rights reserved.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file. See the AUTHORS file for names of contributors.
 */

#include "deltafs_plfsio_filter.h"
#include "deltafs_plfsio_format.h"
#include "deltafs_plfsio_types.h"

#include "pdlfs-common/logging.h"

#include <assert.h>
#include <algorithm>
#include <typeinfo>  // For operator typeid

namespace pdlfs {
namespace plfsio {

// Return the position of the left most "1" bit, such that
//   LeftMostBit(0x00) = 0
//   LeftMostBit(0x01) = 1
//   LeftMostBit(0x02) = 2
//   LeftMostBit(0x04) = 3
//   ...
static unsigned char LeftMostBit(uint32_t i) {
  if (i == 0) return 0;  // Special case
  unsigned char result;
#if defined(__GNUC__)
  result = static_cast<unsigned char>(32 - __builtin_clz(i));
#else
  unsigned int n = 1;
  if (i >> 16 == 0) {
    n += 16;
    i <<= 16;
  }
  if (i >> 24 == 0) {
    n += 8;
    i <<= 8;
  }
  if (i >> 28 == 0) {
    n += 4;
    i <<= 4;
  }
  if (i >> 30 == 0) {
    n += 2;
    i <<= 2;
  }
  n -= i >> 31;
  result = static_cast<unsigned char>(32 - n);
#endif
  return result;
}

BloomBlock::BloomBlock(const DirOptions& options, size_t bytes_to_reserve)
    : bits_per_key_(options.bf_bits_per_key) {
  // Round down to reduce probing cost a little bit
  k_ = static_cast<uint32_t>(bits_per_key_ * 0.69);  // 0.69 =~ ln(2)
  if (k_ < 1) k_ = 1;
  if (k_ > 30) k_ = 30;
  // Reserve an extra byte for storing the k
  if (bytes_to_reserve != 0) {
    space_.reserve(bytes_to_reserve + 1);
  }
  finished_ = true;  // Pending further initialization
  bits_ = 0;
}

BloomBlock::~BloomBlock() {}

int BloomBlock::chunk_type() {
  return static_cast<int>(kSbfChunk);  // Standard bloom filter
}

void BloomBlock::Reset(uint32_t num_keys) {
  bits_ = static_cast<uint32_t>(num_keys * bits_per_key_);
  // For small n, we can see a very high false positive rate.
  // Fix it by enforcing a minimum bloom filter length.
  if (bits_ < 64) {
    bits_ = 64;
  }
  uint32_t bytes = (bits_ + 7) / 8;
  finished_ = false;
  space_.clear();
  space_.resize(bytes, 0);
  // Remember # of probes in filter
  space_.push_back(static_cast<char>(k_));
  // Finalize # bits
  bits_ = bytes * 8;
}

void BloomBlock::AddKey(const Slice& key) {
  assert(!finished_);  // Finish() has not been called
  // Use double-hashing to generate a sequence of hash values.
  uint32_t h = BloomHash(key);
  const uint32_t delta = (h >> 17) | (h << 15);  // Rotate right 17 bits
  for (size_t j = 0; j < k_; j++) {
    const uint32_t b = h % bits_;
    space_[b / 8] |= (1 << (b % 8));
    h += delta;
  }
}

Slice BloomBlock::Finish() {
  assert(!finished_);
  finished_ = true;
  return space_;
}

bool BloomKeyMayMatch(const Slice& key, const Slice& input) {
  const size_t len = input.size();
  if (len < 2) {
    return true;  // Consider it a match
  }
  const uint32_t bits = static_cast<uint32_t>((len - 1) * 8);

  const char* array = input.data();
  // Use the encoded k so that we can read filters generated by
  // bloom filters created using different parameters.
  const uint32_t k = static_cast<unsigned char>(array[len - 1]);
  if (k > 30) {
    // Reserved for potentially new encodings for short bloom filters.
    // Consider it a match.
    return true;
  }

  uint32_t h = BloomHash(key);
  const uint32_t delta = (h >> 17) | (h << 15);  // Rotate right 17 bits
  for (size_t j = 0; j < k; j++) {
    const uint32_t b = h % bits;
    if ((array[b / 8] & (1 << (b % 8))) == 0) {
      return false;
    }
    h += delta;
  }

  return true;
}

// Encoding a bitmap as-is, uncompressed. Used for debugging only.
// Not intended for production.
class UncompressedFormat {
 public:
  UncompressedFormat(const DirOptions& options, std::string* space)
      : key_bits_(options.bm_key_bits), space_(space) {
    bits_ = 1u << key_bits_;  // Logic domain space (total # unique keys)
  }

  void Reset(uint32_t num_keys) {
    space_->clear();
    const size_t bytes = (bits_ + 7) / 8;  // Bitmap size (uncompressed)
    space_->resize(bytes, 0);
  }

  // Set the i-th bit to "1". If the i-th bit is already set,
  // no action needs to be taken.
  void Set(uint32_t i) {
    assert(i < bits_);  // Must not flow out of the key space
    (*space_)[i / 8] |= 1 << (i % 8);
  }

  // Finalize the bitmap representation.
  // Return the final buffer size.
  size_t Finish() { return space_->size(); }

  // Return true iff the i-th bit is set in the given bitmap.
  static bool Test(uint32_t i, size_t key_bits, const Slice& input) {
    const size_t bits = input.size() * 8;
    if (i < bits) {
      return 0 != (input[i / 8] & (1 << (i % 8)));
    } else {
      return false;
    }
  }

  // Report total memory consumption.
  size_t memory_usage() const {
    size_t result = space_->capacity();
    return result;
  }

 private:
  // Key size in bits
  const size_t key_bits_;
  // Underlying space for the bitmap
  std::string* const space_;
  // Total bits in the bitmap
  size_t bits_;
};

// Encoding a bitmap in-memory using a roaring-like bucketized bitmap
// representation for fast accesses. Final storage representation
// is not implemented, and is the job of the subclasses.
class CompressedFormat {
 public:
  CompressedFormat(const DirOptions& options, std::string* space)
      : bytes_per_bucket_(0),
        estimated_bucket_size_(0),
        num_keys_(0),
        key_bits_(options.bm_key_bits),
        space_(space) {
    bits_ = 1u << key_bits_;  // Logic domain space (total num of unique keys)
    num_buckets_ = 1u << (key_bits_ - 8);  // Each bucket manages 256 keys
  }

  // Reset filter state and resize the underlying buffer space.
  // Use num_keys to estimate bitmap density.
  void Reset(uint32_t num_keys) {
    num_keys_ = num_keys;
    extra_keys_.clear();
    working_space_.clear();
    // Estimated number of user keys per bucket. The actual number
    // for each bucket may differ. Works best when user keys
    // are uniformly distributed.
    // Each key only takes 1 byte to store.
    estimated_bucket_size_ = (num_keys + num_buckets_ - 1) / num_buckets_;
    // Use an extra byte to store the actual number
    // of user keys inserted at each bucket.
    bytes_per_bucket_ = estimated_bucket_size_ + 1;
    working_space_.resize(bytes_per_bucket_ * num_buckets_, 0);
    space_->clear();
  }

  // Set the i-th bit to "1". If the i-th bit is already set,
  // no action needs to be taken.
  void Set(uint32_t i) {
    const uint32_t bucket_index = i >> 8;
    // Obtain current bucket size
    assert(bytes_per_bucket_ == estimated_bucket_size_ + 1);
    size_t bucket_size = static_cast<unsigned char>(
        working_space_[bucket_index * bytes_per_bucket_]);
    // Update bucket size
    working_space_[bucket_index * bytes_per_bucket_] =
        static_cast<char>(bucket_size + 1);
    if (bucket_size < estimated_bucket_size_) {
      working_space_[bucket_index * bytes_per_bucket_ + 1 + bucket_size] =
          static_cast<char>(i & 255);
    } else {
      extra_keys_.push_back(i);
    }
  }

  // Report total memory consumption.
  size_t memory_usage() const {
    size_t result = 0;
    result += working_space_.capacity();
    result += extra_keys_.capacity() * sizeof(uint32_t);
    result += space_->capacity();
    return result;
  }

 protected:
  class Iter {  // Iterate through all bitmap buckets in working_space_
   public:
    // REQUIRES: parent.extra_keys is sorted
    explicit Iter(const CompressedFormat& parent)
        : bytes_per_bucket_(parent.bytes_per_bucket_),
          estimated_bucket_size_(parent.estimated_bucket_size_),
          num_buckets_(parent.num_buckets_),
          working_space_(parent.working_space_.data()) {
      bucket_keys_.reserve(16);
      iter_end_ = parent.extra_keys_.end();
      iter_ = parent.extra_keys_.begin();
      bucket_index_ = 0;  // Seek to the first bucket
      if (Valid()) {
        Fetch();
      }
    }

    // Return the current bucket index.
    size_t index() const { return bucket_index_; }

    // Return a pointer to the bucket keys
    // Contents valid until the next Next() call.
    std::vector<uint32_t>* keys() { return &bucket_keys_; }

    bool Valid() const {  // True iff bucket exists
      return bucket_index_ < num_buckets_;
    }

    void Next() {
      bucket_index_++;
      if (Valid()) {
        Fetch();
      }
    }

   private:
    // Retrieve all keys belonging to the current bucket.
    // Results are not sorted.
    void Fetch() {
      bucket_keys_.clear();
      const uint32_t bucket_size = static_cast<unsigned char>(
          working_space_[bytes_per_bucket_ * bucket_index_]);
      for (uint32_t i = 0; i < bucket_size; i++) {
        if (i < estimated_bucket_size_) {
          uint32_t key_offset = static_cast<unsigned char>(
              working_space_[bytes_per_bucket_ * bucket_index_ + 1 + i]);
          bucket_keys_.push_back(key_offset + (bucket_index_ << 8));
        } else {
          assert(iter_ != iter_end_);
          bucket_keys_.push_back(*iter_);
          ++iter_;
        }
      }
    }

    // Constant after construction
    const size_t bytes_per_bucket_;
    const size_t estimated_bucket_size_;
    const size_t num_buckets_;

    const char* working_space_;

    // Temp storage for all keys in the current bucket
    std::vector<uint32_t> bucket_keys_;
    // Cursor to the extra keys
    std::vector<uint32_t>::const_iterator iter_end_;
    std::vector<uint32_t>::const_iterator iter_;
    // Current bucket index
    uint32_t bucket_index_;
  };

  // Finalize the bitmap representation.
  // Return the final buffer size.
  // Not a virtual function.
  size_t Finish() {
    std::sort(extra_keys_.begin(), extra_keys_.end());
    // To be overridden by subclasses...
    return 0;
  }

  // Number of user keys for each lookup entry
  static const size_t partition_size_ = 1024;

  // Helper class for building auxiliary lookup tables to speedup bitmap
  // queries. Each lookup entry takes 8 bytes to store.
  class LookupTableBuilder {
   public:
    LookupTableBuilder(std::string* space, size_t num_keys)
        : partition_dta_prefix_(0),
          partition_num_keys_(0),
          partition_index_(0),
          space_(space) {
      size_t num_partitions =  // Total number of partitions to create
          (num_keys + partition_size_ - 1) / partition_size_;
      if (num_partitions == 0) {
        num_partitions = 1;  // Ensure at least 1 partition
      }
      // Each lookup entry takes 8 bytes to store. The first 4 bytes represent
      // the delta prefix of a partition. The last 4 bytes represent
      // the partition storage offset.
      space_->resize(num_partitions * 8, 0);
      EncodeFixed32(&(*space_)[4], static_cast<uint32_t>(space_->size()));
    }

    void Add(uint32_t dta) {
      // If a partition becomes full
      if (partition_num_keys_ == partition_size_) {
        EncodeFixed32(&(*space_)[partition_index_++ * 8],
                      partition_dta_prefix_);  // Finalize the current partition
        EncodeFixed32(
            &(*space_)[partition_index_ * 8 + 4],  // Initialize the next
            static_cast<uint32_t>(space_->size()));
        partition_num_keys_ = 0;
      }
      partition_dta_prefix_ += dta;
      partition_num_keys_++;
    }

    void Finish() {
      if (partition_num_keys_ != 0) {  // Finalize the last partition
        EncodeFixed32(&(*space_)[partition_index_ * 8], partition_dta_prefix_);
      }
    }

   private:
    // Accumulated delta sum covering deltas both before and in
    // the current partition
    uint32_t partition_dta_prefix_;
    // Number of user keys in the current partition
    size_t partition_num_keys_;
    size_t partition_index_;

    // Final bitmap storage.
    // The lookup table is pre-allocated at the beginning of the buffer space,
    // followed by the actual bitmap representation.
    std::string* space_;
  };

  // Helper class for accessing lookup tables.
  class LookupTable {
   public:
    explicit LookupTable(const Slice& bitmap) : bitmap_(bitmap) {}

    // Return false if the lookup must not exist.
    // Otherwise, stores the new input and base in *input and *base.
    bool Lookup(uint32_t bit, Slice* input, uint32_t* base) {
      *input = bitmap_;
      *base = 0;
      if (input->size() < 8) {
        return false;  // Too short for a lookup entry
      }
      size_t num_partitions = DecodeFixed32(&(*input)[4]) / 8;
      for (size_t partition_index = 0; partition_index < num_partitions;
           partition_index++) {
        const uint32_t partition_dta_prefix =
            DecodeFixed32(&(*input)[partition_index * 8]);
        if (bit <= partition_dta_prefix) {
          size_t size = DecodeFixed32(&(*input)[partition_index * 8 + 4]);
          input->remove_prefix(size);
          return true;
        } else {
          *base = partition_dta_prefix;
        }
      }

      return false;
    }

   private:
    const Slice& bitmap_;
  };

  // In-memory bitmap storage where the entire bitmap key space
  // is divided into a set of fixed-sized buckets.
  // Each bucket is responsible for a range of 256 keys.
  // User keys inserted are assumed to be uniformly distributed over
  // the key space so each bucket will get approximately
  // bucket_size_ (= num_keys / num_buckets_) keys.
  // |<-               key space               ->|  // [0, 2**key_bits_)
  //    bucket-0,   bucket-1,   bucket-2, ...  // num_buckets_
  std::string working_space_;  // Temp bitmap storage
  // For keys that cannot fit into the statically allocated buckets
  std::vector<uint32_t> extra_keys_;
  size_t bytes_per_bucket_;  // Use an extra byte for bucket size
  // Estimated number of keys per bucket
  size_t estimated_bucket_size_;
  // Total number of buckets
  size_t num_buckets_;

  // Number of user keys in the bitmap
  size_t num_keys_;

  // Key size in bits
  const size_t key_bits_;  // Domain space
  // Space for the final representation
  std::string* const space_;
  // Logic bits in the bitmap. The actual memory or storage
  // used may differ due to compression and formatting.
  size_t bits_;
};

// VbFormat: encode each bitmap using a varint-based scheme.
// Varint is also named VByte, or VB.
class VbFormat : public CompressedFormat {
 public:
  VbFormat(const DirOptions& options, std::string* space)
      : CompressedFormat(options, space) {}

  static void VbEnc(std::string* output, uint32_t value) {
    // While more than 7 bits of data are left, consume the least 7 bits
    // and set the next-byte flag
    while (value > 127) {
      // |128: Set the next byte flag
      output->push_back((value & 127) | 128);
      // Remove the 7 bits we just wrote
      value >>= 7;
    }
    output->push_back(value & 127);
  }

  // Convert the in-memory bitmap representation to an on-storage
  // representation. The in-memory version is stored at working_space_.
  // The on-storage version will be stored in *space_.
  size_t Finish() {
    uint32_t last_key = 0;
    CompressedFormat::Finish();  // Sort extra keys
    Iter bucket_iter(*this);
    for (; bucket_iter.Valid(); bucket_iter.Next()) {
      std::vector<uint32_t>* bucket_keys = bucket_iter.keys();
      std::sort(bucket_keys->begin(), bucket_keys->end());
      for (std::vector<uint32_t>::iterator it = bucket_keys->begin();
           it != bucket_keys->end(); ++it) {
        uint32_t dta = *it - last_key;
        VbEnc(space_, dta);
        last_key = *it;
      }
    }

    return space_->size();
  }

  static uint32_t VbDec(Slice* input) {
    uint32_t result = 0;
    for (size_t i = 0; !input->empty(); i++) {
      unsigned char b = static_cast<unsigned char>((*input)[0]);
      input->remove_prefix(1);
      result |= (b & 127) << (7 * i);
      // If the next-byte flag is set
      if (!(b & 128)) {
        break;
      }
    }
    return result;
  }

  static bool Test(uint32_t bit, size_t key_bits, const Slice& bitmap) {
    uint32_t base = 0;
    Slice input = bitmap;
    while (!input.empty()) {
      base += VbDec(&input);
      if (base == bit) {
        return true;
      } else if (base > bit) {
        return false;
      }
    }
    return false;
  }
};

// Very similar to VbFormat except that the first byte is
// encoded as a special full byte.
class VbPlusFormat : public VbFormat {
 public:
  VbPlusFormat(const DirOptions& options, std::string* space)
      : VbFormat(options, space) {}

  static void VbPlusEnc(std::string* output, uint32_t value) {
    if (value < 255) {
      output->push_back(value);  // Encode the byte as-is
    } else {
      output->push_back(static_cast<char>(255));
      value -= 254;  // Continue with varint
      VbEnc(output, value);
    }
  }

  // Convert the in-memory bitmap representation to an on-storage
  // representation. The in-memory version is stored at working_space_.
  // The on-storage version will be stored in *space_.
  size_t Finish() {
    uint32_t last_key = 0;
    CompressedFormat::Finish();  // Sort extra keys
    Iter bucket_iter(*this);
    for (; bucket_iter.Valid(); bucket_iter.Next()) {
      std::vector<uint32_t>* bucket_keys = bucket_iter.keys();
      std::sort(bucket_keys->begin(), bucket_keys->end());
      for (std::vector<uint32_t>::iterator it = bucket_keys->begin();
           it != bucket_keys->end(); ++it) {
        uint32_t dta = *it - last_key;
        VbPlusEnc(space_, dta);
        last_key = *it;
      }
    }

    return space_->size();
  }

  static uint32_t VbPlusDec(Slice* input) {
    uint32_t result = 0;
    if (!input->empty()) {
      unsigned char b = static_cast<unsigned char>((*input)[0]);
      input->remove_prefix(1);
      if (b == 255) {
        result = VbDec(input) + 254;
      } else {
        result = b;
      }
    }
    return result;
  }

  static bool Test(uint32_t bit, size_t key_bits, const Slice& bitmap) {
    uint32_t base = 0;
    Slice input = bitmap;
    while (!input.empty()) {
      base += VbPlusDec(&input);
      if (base == bit) {
        return true;
      } else if (base > bit) {
        return false;
      }
    }
    return false;
  }
};

// Similar to VbPlusFormat but with an extra lookup table for faster queries.
// A lookup entry is inserted for each partition_size_ keys.
// Each lookup entry costs 8 bytes, using 4 bytes for an offset,
// and another 4 bytes for a delta prefix.
class FastVbPlusFormat : public VbPlusFormat {
 public:
  FastVbPlusFormat(const DirOptions& options, std::string* space)
      : VbPlusFormat(options, space) {}

  // Convert the in-memory bitmap representation to an on-storage
  // representation. The in-memory version is stored at working_space_.
  // The on-storage version will be stored in *space_.
  size_t Finish() {
    uint32_t last_key = 0;
    LookupTableBuilder table(space_, num_keys_);
    CompressedFormat::Finish();  // Sort extra keys
    Iter bucket_iter(*this);
    for (; bucket_iter.Valid(); bucket_iter.Next()) {
      std::vector<uint32_t>* bucket_keys = bucket_iter.keys();
      std::sort(bucket_keys->begin(), bucket_keys->end());
      for (std::vector<uint32_t>::iterator it = bucket_keys->begin();
           it != bucket_keys->end(); ++it) {
        uint32_t dta = *it - last_key;
        table.Add(dta);  // Must go before the encoding
        VbPlusEnc(space_, dta);
        last_key = *it;
      }
    }
    // Finalize the lookup table
    table.Finish();

    return space_->size();
  }

  static bool Test(uint32_t bit, size_t key_bits, const Slice& bitmap) {
    Slice input = bitmap;
    uint32_t base = 0;
    LookupTable table(bitmap);
    if (table.Lookup(bit, &input, &base)) {
      while (!input.empty()) {
        base += VbPlusDec(&input);
        if (base == bit) {
          return true;
        } else if (base > bit) {
          return false;
        }
      }
    }

    return false;
  }
};

// PfDeltaFormat: encode each bitmap using a p-for-delta-based compression
// scheme for a higher compression rate by doing bit-level encoding.
class PfDeltaFormat : public CompressedFormat {
 public:
  PfDeltaFormat(const DirOptions& options, std::string* space)
      : CompressedFormat(options, space) {}

  // Convert the in-memory bitmap representation to an on-storage
  // representation. The in-memory version is stored at working_space_.
  // The on-storage version will be stored in *space_.
  size_t Finish() {
    uint32_t cohort_max = 0;  // No less than the max in a cohort
    // A group of input keys to compress together
    std::vector<uint32_t> cohort;
    cohort.reserve(cohort_size_);
    uint32_t last_key = 0;
    CompressedFormat::Finish();  // Sort extra keys
    Iter bucket_iter(*this);
    for (; bucket_iter.Valid(); bucket_iter.Next()) {
      std::vector<uint32_t>* bucket_keys = bucket_iter.keys();
      std::sort(bucket_keys->begin(), bucket_keys->end());
      for (std::vector<uint32_t>::iterator it = bucket_keys->begin();
           it != bucket_keys->end(); ++it) {
        uint32_t dta = *it - last_key;
        cohort.push_back(dta);
        cohort_max |= dta;
        if (cohort.size() == cohort_size_) {
          PfDtaEnc(space_, cohort, cohort_max);
          cohort.clear();
          cohort_max = 0;
        }
        last_key = *it;
      }
    }

    if (!cohort.empty()) {
      PfDtaEnc(space_, cohort, cohort_max);
    }

    return space_->size();
  }

  static bool Test(uint32_t bit, size_t key_bits, const Slice& bitmap) {
    uint32_t base = 0;
    std::vector<uint32_t> cohort;
    cohort.reserve(cohort_size_);
    Slice input = bitmap;
    while (!input.empty()) {
      size_t num_keys = PfDtaDec(&input, &cohort);
      for (size_t i = 0; i < num_keys; i++) {
        base += cohort[i];
        if (base == bit) {
          return true;
        } else if (base > bit) {
          return false;
        }
      }
    }

    return false;
  }

 protected:
  // Number of user keys per cohort (compression group)
  // REQUIRES: must be a multiple of 8.
  static const size_t cohort_size_ = 128;

  static size_t PfDtaDec(Slice* input, std::vector<uint32_t>* cohort) {
    cohort->clear();
    if (input->empty()) return 0;
    unsigned char num_bits = static_cast<unsigned char>((*input)[0]);
    input->remove_prefix(1);
    unsigned char b = 0;  // Tmp byte to consume encoded bits
    size_t num_keys = cohort_size_;
    // Will never overflow the buffer space, but may return garbage, though all
    // garbage keys will be zero, which won't impact correctness.
    if (8 * input->size() / num_bits < num_keys) {
      num_keys = 8 * input->size() / num_bits;
    }
    int bit_index = -1;  // Pending restart from the most significant bit
    for (size_t i = 0; i < num_keys; i++) {
      uint32_t dta = 0;
      int remaining_bits = num_bits - 1;
      while (remaining_bits >= 0) {
        if (bit_index < 0) {
          b = static_cast<unsigned char>((*input)[0]);
          input->remove_prefix(1);
          bit_index = 7;
        }
        dta |= (b & (1 << bit_index--)) > 0 ? (1 << remaining_bits) : 0;
        remaining_bits--;
      }
      // It is possible to filter out garbage keys here.
      // This is left as future work.
      cohort->push_back(dta);
    }

    return cohort->size();
  }

  static void PfDtaEnc(std::string* output, const std::vector<uint32_t>& cohort,
                       uint32_t cohort_max) {
    unsigned char num_bits = LeftMostBit(cohort_max);  // Bits per key
    output->push_back(num_bits);
    unsigned char b = 0;  // Tmp byte to fill bits
    int bit_index = 7;    // Start from the most significant bit
    // Encoding cohort
    for (std::vector<uint32_t>::const_iterator it = cohort.begin();
         it != cohort.end(); ++it) {
      int remaining_bits = num_bits - 1;
      while (remaining_bits >= 0) {
        b |= (*it & (1 << remaining_bits--)) >= 1 ? (1 << bit_index) : 0;
        if (bit_index-- == 0) {
          output->push_back(b);
          bit_index = 7;
          b = 0;
        }
      }
    }
    if (bit_index != 7) {
      output->push_back(b);
    }
  }
};

// Similar to PfDeltaFormat but with an extra lookup table for faster queries.
// A lookup entry is inserted for each partition_size_ keys. Each
// lookup entry costs 8 bytes, using 4 bytes for an offset,
// and another 4 bytes for a delta prefix.
class FastPfDeltaFormat : public PfDeltaFormat {
 public:
  FastPfDeltaFormat(const DirOptions& options, std::string* space)
      : PfDeltaFormat(options, space) {}

  // Convert the in-memory bitmap representation to an on-storage
  // representation. The in-memory version is stored at working_space_.
  // The on-storage version will be stored in *space_.
  size_t Finish() {
    uint32_t cohort_max = 0;  // No less than the max in a cohort
    // A group of input keys to compress together
    std::vector<uint32_t> cohort;
    cohort.reserve(cohort_size_);
    uint32_t last_key = 0;
    LookupTableBuilder table(space_, num_keys_);
    CompressedFormat::Finish();  // Sort extra keys
    Iter bucket_iter(*this);
    for (; bucket_iter.Valid(); bucket_iter.Next()) {
      std::vector<uint32_t>* bucket_keys = bucket_iter.keys();
      std::sort(bucket_keys->begin(), bucket_keys->end());
      for (std::vector<uint32_t>::iterator it = bucket_keys->begin();
           it != bucket_keys->end(); ++it) {
        uint32_t dta = *it - last_key;
        table.Add(dta);  // Must go before the encoding
        cohort.push_back(dta);
        cohort_max |= dta;
        if (cohort.size() == cohort_size_) {
          PfDtaEnc(space_, cohort, cohort_max);
          cohort.clear();
          cohort_max = 0;
        }
        last_key = *it;
      }
    }

    table.Finish();  // Finalize the lookup table
    // Complete and write out the last cohort
    if (!cohort.empty()) {
      PfDtaEnc(space_, cohort, cohort_max);
    }

    return space_->size();
  }

  static bool Test(uint32_t bit, size_t key_bits, const Slice& bitmap) {
    uint32_t base = 0;
    std::vector<uint32_t> cohort;
    cohort.reserve(cohort_size_);
    Slice input = bitmap;
    LookupTable table(bitmap);
    if (table.Lookup(bit, &input, &base)) {
      while (!input.empty()) {
        size_t num_keys = PfDtaDec(&input, &cohort);
        for (size_t i = 0; i < num_keys; i++) {
          base += cohort[i];
          if (base == bit) {
            return true;
          } else if (base > bit) {
            return false;
          }
        }
      }
    }

    return false;
  }
};

// RoaringFormat: encode each bitmap using a fast, lightly-compressed,
// bucketized bitmap representation. Each bitmap bucket manages a fixed key
// range consisting of 256 potential keys, and is paired with an 1-byte header
// indicating the actual number of keys stored in that bucket.
class RoaringFormat : public CompressedFormat {
 public:
  RoaringFormat(const DirOptions& options, std::string* space)
      : CompressedFormat(options, space) {}

  // Convert the in-memory bitmap representation to an on-storage
  // representation using the fast roaring format. The in-memory
  // version is stored at working_space_. The on-storage
  // version will be written into *space_.
  size_t Finish() {
    // Remember total number of buckets
    PutFixed32(space_, num_buckets_);
    // Reserve enough buffer space for bucket headers
    space_->resize(4 + num_buckets_, 0);  // 1 byte per header
    // Finalize all buckets
    CompressedFormat::Finish();  // Sort extra keys
    Iter bucket_iter(*this);
    for (; bucket_iter.Valid(); bucket_iter.Next()) {
      std::vector<uint32_t>* bucket_keys = bucket_iter.keys();
      (*space_)[4 + bucket_iter.index()] =
          static_cast<char>(bucket_keys->size());
      std::sort(bucket_keys->begin(), bucket_keys->end());
      for (std::vector<uint32_t>::iterator it = bucket_keys->begin();
           it != bucket_keys->end(); ++it) {
        space_->push_back(*it & 255);
      }
    }

    return space_->size();
  }

  static bool Test(uint32_t bit, size_t key_bits, const Slice& bitmap) {
    Slice input = bitmap;
    if (input.size() < 4) {
      return false;  // Too short to be valid
    }
    // Recover bucket count
    const size_t num_buckets = DecodeFixed32(input.data());
    input.remove_prefix(4);
    if (input.size() < num_buckets) {  // Pre-mature end of buffer space
      return false;
    }
    const size_t bucket_index = bit >> 8;  // Target bucket
    size_t bucket_start = 0;
    size_t bucket_end = 0;
    for (size_t i = 0; i <= bucket_index; i++) {
      if (i < num_buckets) {
        bucket_start = bucket_end;
        bucket_end += static_cast<unsigned char>(input[i]);
      } else {  // No such bucket!
        return false;
      }
    }

    // Search within the target bucket
    input.remove_prefix(num_buckets);
    if (input.size() >= bucket_end) {
      unsigned char target = bit & 255;
      for (size_t i = bucket_start; i < bucket_end; i++) {
        unsigned char key = static_cast<unsigned char>(input[i]);
        if (key > target) {
          return false;
        } else if (key == target) {
          return true;
        }
      }
    }

    return false;
  }
};

template <typename T>
int BitmapBlock<T>::chunk_type() {
  return static_cast<int>(kBmpChunk);
}

template <typename T>
BitmapBlock<T>::BitmapBlock(const DirOptions& options, size_t bytes_to_reserve)
    : key_bits_(options.bm_key_bits), bm_fmt_(int(options.bm_fmt)) {
  // Reserve extra 2 bytes for storing key_bits and the compression type
  if (bytes_to_reserve != 0) {
    space_.reserve(bytes_to_reserve + 2);
  }
  fmt_ = new T(options, &space_);
  finished_ = true;  // Pending further initialization
  mask_ = ~static_cast<uint32_t>(0) << key_bits_;
  mask_ = ~mask_;
}

template <typename T>
void BitmapBlock<T>::Reset(uint32_t num_keys) {
  assert(fmt_ != NULL);
  fmt_->Reset(num_keys);
  finished_ = false;
}

// To convert a key into an int, the first 4 bytes of the key is interpreted
// as the little-endian representation of a 32-bit int. As illustrated below,
// the conversion may be seen as using the "first" 32 bits of the byte array
// to construct an int.
//
// [07.06.05.04.03.02.01.00]  [15.14.13.12.11.10.09.08] [...] [...]
//  <------------ byte 0 ->    <------------ byte 1 ->
//
static uint32_t BitmapIndex(const Slice& key) {
  char tmp[4];
  memset(tmp, 0, sizeof(tmp));
  memcpy(tmp, key.data(), std::min(key.size(), sizeof(tmp)));
  return DecodeFixed32(tmp);
}

// Insert a key (1-4 bytes) into the bitmap filter. If the key has more than 4
// bytes, the rest bytes are ignored. If a key has less than 4 bytes, it will
// be zero-padded to 4 bytes. Inserting a key is achieved by first converting
// the key into an int, i, and then setting the i-th bit of the bitmap to "1".
template <typename T>
void BitmapBlock<T>::AddKey(const Slice& key) {
  assert(!finished_);  // Finish() has not been called
  uint32_t i = BitmapIndex(key);
  i &= mask_;
  assert(fmt_ != NULL);
  fmt_->Set(i);
}

template <typename T>
Slice BitmapBlock<T>::Finish() {
  assert(fmt_ != NULL);
  finished_ = true;
  size_t len = fmt_->Finish();
  space_.resize(len);
  // Remember the size of the domain space
  space_.push_back(static_cast<char>(key_bits_));
  // Remember the bitmap format
  const int fmt = BitmapFormatFromType<BitmapBlock<T> >();
#ifndef NDEBUG
  if (fmt != bm_fmt_) {
    Warn(__LOG_ARGS__, "Bitmap format option does not match class type");
  }
#endif
  space_.push_back(static_cast<char>(fmt));
  return space_;
}

template <typename T>
size_t BitmapBlock<T>::memory_usage() const {
  return fmt_->memory_usage();
}

template <typename T>
BitmapBlock<T>::~BitmapBlock() {
  delete fmt_;
}

// Initialize all supported bitmap format templates
template class BitmapBlock<UncompressedFormat>;
template class BitmapBlock<FastVbPlusFormat>;
template class BitmapBlock<VbPlusFormat>;
template class BitmapBlock<VbFormat>;

template class BitmapBlock<FastPfDeltaFormat>;
template class BitmapBlock<PfDeltaFormat>;

template class BitmapBlock<RoaringFormat>;

// Return true if the target key is present in the given bitmap by
// checking its binary representation.
static bool BitmapTestKey(int fmt, uint32_t k, size_t key_bits,
                          const Slice& rep) {
#define BMP_TESTKEY(T) T::Test(k, key_bits, rep)
  if (fmt == kFmtUncompressed) {
    return BMP_TESTKEY(UncompressedFormat);
  } else if (fmt == kFmtFastVarintPlus) {
    return BMP_TESTKEY(FastVbPlusFormat);
  } else if (fmt == kFmtVarintPlus) {
    return BMP_TESTKEY(VbPlusFormat);
  } else if (fmt == kFmtVarint) {
    return BMP_TESTKEY(VbFormat);
  } else if (fmt == kFmtFastPfDelta) {
    return BMP_TESTKEY(FastPfDeltaFormat);
  } else if (fmt == kFmtPfDelta) {
    return BMP_TESTKEY(PfDeltaFormat);
  } else if (fmt == kFmtRoaring) {
    return BMP_TESTKEY(RoaringFormat);
  } else {  // Consider it a match for unknown formats
    return true;
  }
#undef BMP_TESTKEY
}

// Return true if the target key matches a given bitmap filter input. Note that
// unlike bloom filters, bitmap filters are designed with no false positives.
bool BitmapKeyMustMatch(const Slice& key, const Slice& input) {
  const size_t len = input.size();
  if (len < 2) {
    return false;  // Empty bitmap
  }

  Slice bitmap =
      input;  // Net bitmap representation (maybe in a compressed form)
  bitmap.remove_suffix(2);
  uint32_t k = BitmapIndex(key);

  // Recover the domain space
  const size_t key_bits = static_cast<unsigned char>(input[input.size() - 2]);

  size_t bits = 1u << key_bits;
  if (k >= bits) {
    return false;  // Out of bound
  }

  const int fmt = input[input.size() - 1];
  return BitmapTestKey(fmt, k, key_bits, bitmap);
}

// Return the corresponding bitmap format according to the given
// filter block type. Return -1 for non-bitmap-oriented types.
template <typename T>
int BitmapFormatFromType() {
  if (typeid(T) == typeid(BitmapBlock<UncompressedFormat>)) {
    return static_cast<int>(kFmtUncompressed);
  } else if (typeid(T) == typeid(BitmapBlock<FastVbPlusFormat>)) {
    return static_cast<int>(kFmtFastVarintPlus);
  } else if (typeid(T) == typeid(BitmapBlock<VbPlusFormat>)) {
    return static_cast<int>(kFmtVarintPlus);
  } else if (typeid(T) == typeid(BitmapBlock<VbFormat>)) {
    return static_cast<int>(kFmtVarint);
  } else if (typeid(T) == typeid(BitmapBlock<FastPfDeltaFormat>)) {
    return static_cast<int>(kFmtFastPfDelta);
  } else if (typeid(T) == typeid(BitmapBlock<PfDeltaFormat>)) {
    return static_cast<int>(kFmtPfDelta);
  } else if (typeid(T) == typeid(BitmapBlock<RoaringFormat>)) {
    return static_cast<int>(kFmtRoaring);
  } else {
    return -1;
  }
}

// Instantiate for all potential filter block types.
template int BitmapFormatFromType<BitmapBlock<UncompressedFormat> >();
template int BitmapFormatFromType<BitmapBlock<FastVbPlusFormat> >();
template int BitmapFormatFromType<BitmapBlock<VbPlusFormat> >();
template int BitmapFormatFromType<BitmapBlock<VbFormat> >();

template int BitmapFormatFromType<BitmapBlock<FastPfDeltaFormat> >();
template int BitmapFormatFromType<BitmapBlock<PfDeltaFormat> >();

template int BitmapFormatFromType<BitmapBlock<RoaringFormat> >();
template int BitmapFormatFromType<EmptyFilterBlock>();
template int BitmapFormatFromType<BloomBlock>();

int EmptyFilterBlock::chunk_type() {
  return static_cast<int>(kUnknown);  // Dummy block type
}

EmptyFilterBlock::EmptyFilterBlock(const DirOptions& o, size_t b) {
  space_.resize(0);
#if __cplusplus >= 201103L
  space_.shrink_to_fit();  // Not available unitl c++11
#endif
}

}  // namespace plfsio
}  // namespace pdlfs