// Copyright 2012 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_SNAPSHOT_SNAPSHOT_SOURCE_SINK_H_
#define V8_SNAPSHOT_SNAPSHOT_SOURCE_SINK_H_

#include <utility>
#include <vector>

#include "src/base/atomicops.h"
#include "src/base/logging.h"
#include "src/common/globals.h"
#include "src/utils/utils.h"

namespace v8 {
namespace internal {


/**
 * Source to read snapshot and builtins files from.
 *
 * Note: Memory ownership remains with callee.
 */
class SnapshotByteSource final {
 public:
  SnapshotByteSource(const char* data, int length)
      : data_(reinterpret_cast<const uint8_t*>(data)),
        length_(length),
        position_(0) {}

  explicit SnapshotByteSource(base::Vector<const uint8_t> payload)
      : data_(payload.begin()), length_(payload.length()), position_(0) {}

  ~SnapshotByteSource() = default;
  SnapshotByteSource(const SnapshotByteSource&) = delete;
  SnapshotByteSource& operator=(const SnapshotByteSource&) = delete;

  bool HasMore() { return position_ < length_; }

  uint8_t Get() {
    DCHECK(position_ < length_);
    return data_[position_++];
  }

  uint8_t Peek() const {
    DCHECK(position_ < length_);
    return data_[position_];
  }

  void Advance(int by) { position_ += by; }

  void CopyRaw(void* to, size_t number_of_bytes) {
    DCHECK_LE(position_ + number_of_bytes, length_);
    memcpy(to, data_ + position_, number_of_bytes);
    position_ += number_of_bytes;
  }

  void CopySlots(Address* dest, int number_of_slots) {
    // Hoist `position_` and `data_` out of the inner loop so the compiler
    // doesn't have to assume they alias with `dest` and reload them each
    // iteration. Each tagged slot is still stored with Relaxed_Store so the
    // concurrent marker continues to see word-atomic writes.
    const uint8_t* src = data_ + position_;
    base::AtomicWord* start = reinterpret_cast<base::AtomicWord*>(dest);
    for (int i = 0; i < number_of_slots; ++i) {
      base::AtomicWord val;
      memcpy(&val, src + i * sizeof(base::AtomicWord),
             sizeof(base::AtomicWord));
      base::Relaxed_Store(start + i, val);
    }
    position_ += number_of_slots * sizeof(base::AtomicWord);
  }

#ifdef V8_COMPRESS_POINTERS
  void CopySlots(Tagged_t* dest, int number_of_slots) {
    const uint8_t* src = data_ + position_;
    AtomicTagged_t* start = reinterpret_cast<AtomicTagged_t*>(dest);
    for (int i = 0; i < number_of_slots; ++i) {
      AtomicTagged_t val;
      memcpy(&val, src + i * sizeof(AtomicTagged_t),
             sizeof(AtomicTagged_t));
      base::Relaxed_Store(start + i, val);
    }
    position_ += number_of_slots * sizeof(AtomicTagged_t);
  }
#endif

  // Decode a uint30 with run-length encoding. Must have been encoded with
  // PutUint30.
  inline uint32_t GetUint30() {
    // This way of decoding variable-length encoded integers does not
    // suffer from branch mispredictions.
    DCHECK_LT(position_ + 3, length_);
    uint32_t answer = data_[position_];
    answer |= data_[position_ + 1] << 8;
    answer |= data_[position_ + 2] << 16;
    answer |= data_[position_ + 3] << 24;
    int bytes = (answer & 3) + 1;
    Advance(bytes);
    uint32_t mask = 0xffffffffu;
    mask >>= 32 - (bytes << 3);
    answer &= mask;
    answer >>= 2;
    return answer;
  }

  uint32_t GetUint32() {
    uint32_t integer;
    CopyRaw(reinterpret_cast<uint8_t*>(&integer), sizeof(integer));
    return integer;
  }

  // Returns length.
  int GetBlob(const uint8_t** data);

  size_t position() const { return position_; }
  void set_position(size_t position) { position_ = position; }

  const uint8_t* data() const { return data_; }
  size_t length() const { return length_; }

 private:
  const uint8_t* data_;
  size_t length_;
  size_t position_;
};

/**
 * Sink to write snapshot files to.
 *
 * Users must implement actual storage or i/o.
 */
class SnapshotByteSink {
 public:
  SnapshotByteSink() = default;
  explicit SnapshotByteSink(int initial_size) : data_(initial_size) {}

  ~SnapshotByteSink() = default;

  void Put(uint8_t b, const char* description) { data_.push_back(b); }

  void PutN(size_t number_of_bytes, const uint8_t v, const char* description);
  // Append a uint30 with run-length encoding. Must be decoded with GetUint30.
  void PutUint30(uint32_t integer, const char* description);
  void PutUint32(uint32_t integer, const char* description) {
    PutRaw(reinterpret_cast<uint8_t*>(&integer), sizeof(integer), description);
  }
  void PutRaw(const uint8_t* data, size_t number_of_bytes,
              const char* description);

  void Append(const SnapshotByteSink& other);
  size_t Position() const { return data_.size(); }

  const std::vector<uint8_t>* data() const { return &data_; }

 private:
  std::vector<uint8_t> data_;
};

}  // namespace internal
}  // namespace v8

#endif  // V8_SNAPSHOT_SNAPSHOT_SOURCE_SINK_H_
