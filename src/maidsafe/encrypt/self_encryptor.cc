﻿
/*******************************************************************************
*  Copyright 2011 MaidSafe.net limited                                         *
*                                                                              *
*  The following source code is property of MaidSafe.net limited and is not    *
*  meant for external use.  The use of this code is governed by the license    *
*  file LICENSE.TXT found in the root of this directory and also on            *
*  www.MaidSafe.net.                                                           *
*                                                                              *
*  You are not free to copy, amend or otherwise use this source code without   *
*  the explicit written permission of the board of directors of MaidSafe.net.  *
*******************************************************************************/

#include "maidsafe/encrypt/self_encryptor.h"

#include <omp.h>

#include <tuple>
#include <algorithm>
#include <limits>
#include <set>
#include <vector>
#include <utility>

#ifdef __MSVC__
#  pragma warning(push, 1)
#endif
#include "cryptopp/aes.h"
#include "cryptopp/gzip.h"
#include "cryptopp/modes.h"
#include "cryptopp/mqueue.h"
#include "cryptopp/sha.h"
#ifdef __MSVC__
#  pragma warning(pop)
#endif
#include "boost/scoped_array.hpp"
#include "maidsafe/common/crypto.h"
#include "maidsafe/common/utils.h"
#include "maidsafe/common/chunk_store.h"

#include "maidsafe/encrypt/config.h"
#include "maidsafe/encrypt/data_map.h"
#include "maidsafe/encrypt/log.h"
#include "maidsafe/encrypt/sequencer.h"


namespace fs = boost::filesystem;

namespace maidsafe {
namespace encrypt {

namespace {

const size_t kPadSize((3 * crypto::SHA512::DIGESTSIZE) -
                      crypto::AES256_KeySize - crypto::AES256_IVSize);

uint64_t TotalSize(DataMapPtr data_map, const uint32_t &normal_chunk_size) {
  if (!data_map->content.empty())
    return data_map->content.size();

  if (data_map->chunks.empty())
    return 0;

  return ((data_map->chunks.size() - 1) * normal_chunk_size) +
          (*data_map->chunks.rbegin()).size;
}

class XORFilter : public CryptoPP::Bufferless<CryptoPP::Filter> {
 public:
  XORFilter(CryptoPP::BufferedTransformation *attachment, byte *pad)
      : pad_(pad), count_(0) { CryptoPP::Filter::Detach(attachment); }
  size_t Put2(const byte* in_string,
              size_t length,
              int message_end,
              bool blocking) {
    if (length == 0) {
      return AttachedTransformation()->Put2(in_string, length, message_end,
                                            blocking);
    }
    boost::scoped_array<byte> buffer(new byte[length]);

    size_t i(0);
  // #pragma omp parallel for shared(buffer, in_string) private(i)
    for (; i != length; ++i) {
      buffer[i] = in_string[i] ^ pad_[count_ % kPadSize];
      ++count_;
    }

    return AttachedTransformation()->Put2(buffer.get(), length, message_end,
                                          blocking);
  }
  bool IsolatedFlush(bool, bool) { return false; }
 private:
  XORFilter &operator = (const XORFilter&);
  XORFilter(const XORFilter&);
  byte *pad_;
  size_t count_;
};

}  // unnamed namespace


SelfEncryptor::SelfEncryptor(DataMapPtr data_map,
                             std::shared_ptr<ChunkStore> chunk_store,
                             int num_procs)
    : data_map_(data_map ? data_map : DataMapPtr(new DataMap)),
      sequencer_(new Sequencer),
      kDefaultByteArraySize_(num_procs == 0 ?
                             kDefaultChunkSize * omp_get_num_procs() :
                             kDefaultChunkSize * num_procs),
      file_size_(0),
      last_chunk_position_(0),
      normal_chunk_size_(0),
      main_encrypt_queue_(),
      queue_start_position_(2 * kDefaultChunkSize),
      kQueueCapacity_(kDefaultByteArraySize_ + kDefaultChunkSize),
      retrievable_from_queue_(0),
      chunk0_raw_(),
      chunk1_raw_(),
      chunk_store_(chunk_store),
      current_position_(0),
      prepared_for_writing_(false),
      chunk0_modified_(true),
      chunk1_modified_(true),
      read_cache_(),
      cache_start_position_(0),
      prepared_for_reading_() {
  if (data_map) {
    if (data_map->chunks.empty()) {
      file_size_ = data_map->content.size();
      last_chunk_position_ = std::numeric_limits<uint64_t>::max();
      normal_chunk_size_ = 0;
    } else {
      file_size_ = (data_map->chunks.empty() ? data_map->content.size() : 0);
      std::for_each(data_map->chunks.begin(), --data_map->chunks.end(),
                    [=] (ChunkDetails chunk) { file_size_ += chunk.size; });
      last_chunk_position_ = file_size_;
      file_size_ += (*data_map->chunks.rbegin()).size;
      normal_chunk_size_ = (*data_map->chunks.begin()).size;
    }
  }
}

SelfEncryptor::~SelfEncryptor() {
  Flush();
}

bool SelfEncryptor::Write(const char *data,
                          uint32_t length,
                          uint64_t position) {
  if (length == 0)
    return true;

  if (PrepareToWrite() != kSuccess) {
    DLOG(ERROR) << "Failed to write " << length << "B at pos " << position;
    return false;
  }
  PutToReadCache(data, length, position);

  if (position + length > file_size_) {
    file_size_ = position + length;
    CalculateSizes(false);
  }

  uint32_t written = PutToInitialChunks(data, &length, &position);
  if (!data_map_->chunks.empty()) {
    if (chunk0_modified_)
      HandleRewrite(0);
    if (chunk1_modified_)
      HandleRewrite(1);
  }

  uint32_t data_offset(0), queue_offset(0);
  if (GetDataOffsetForEnqueuing(length, position, &data_offset,
                                &queue_offset)) {
    if (PutToEncryptQueue(data + written, length, data_offset, queue_offset) !=
        kSuccess) {
      DLOG(ERROR) << "Failed to write " << length << "B at pos " << position;
      return false;
    }
  }

  if (GetLengthForSequencer(position, &length)) {
    if (sequencer_->Add(data + written, length, position) != kSuccess) {
      DLOG(ERROR) << "Failed to write " << length << "B at pos " << position;
      return false;
    }
  }

  ByteArray extra(sequencer_->Get(current_position_));
  if (extra) {
    if (PutToEncryptQueue(reinterpret_cast<char*>(extra.get()), Size(extra),
                          0, static_cast<uint32_t>(current_position_ -
                                                   queue_start_position_)) !=
        kSuccess) {
      DLOG(ERROR) << "Failed to write " << length << "B at pos " << position;
      return false;
    }
  }

  return true;
}

int SelfEncryptor::PrepareToWrite() {
  if (prepared_for_writing_)
    return kSuccess;

  if (!main_encrypt_queue_) {
    main_encrypt_queue_ = GetNewByteArray(kQueueCapacity_);
    memset(main_encrypt_queue_.get(), 0, Size(main_encrypt_queue_));
  }

  if (!chunk0_raw_) {
    chunk0_raw_ = GetNewByteArray(kDefaultChunkSize);
    memset(chunk0_raw_.get(), 0, Size(chunk0_raw_));
  }

  if (!chunk1_raw_) {
    chunk1_raw_ = GetNewByteArray(kDefaultChunkSize);
    memset(chunk1_raw_.get(), 0, Size(chunk1_raw_));
  }

  if (!data_map_->chunks.empty()) {
    BOOST_ASSERT(data_map_->chunks.empty() || data_map_->chunks.size() >= 3);
    ByteArray temp(GetNewByteArray(kDefaultChunkSize + 1));
    uint32_t chunks_to_decrypt(2);
    if (data_map_->chunks[0].size != kDefaultChunkSize)
      chunks_to_decrypt = 3;
    bool consumed_whole_chunk(true);
    for (uint32_t i(0); i != chunks_to_decrypt; ++i) {
      int result(DecryptChunk(i, temp.get()));
      if (result != kSuccess) {
        DLOG(ERROR) << "Failed to prepare for writing.";
        return result;
      }
      uint32_t length(data_map_->chunks[i].size);
      uint64_t position(current_position_);
      uint32_t written = PutToInitialChunks(reinterpret_cast<char*>(temp.get()),
                                            &length, &position);
      consumed_whole_chunk = (length == 0);
      if (!consumed_whole_chunk) {
        result = sequencer_->Add(reinterpret_cast<char*>(temp.get()) + written,
                                 length, position);
        if (result != kSuccess) {
          DLOG(ERROR) << "Failed to prepare for writing.";
          return result;
        }
      }
      data_map_->chunks[i].size = 0;
    }
  } else {
    uint32_t length(static_cast<uint32_t>(data_map_->content.size()));
    uint64_t position(0);
    PutToInitialChunks(data_map_->content.data(), &length, &position);
    data_map_->content.clear();
  }

  chunk0_modified_ = false;
  chunk1_modified_ = false;
  prepared_for_writing_ = true;
  return kSuccess;
}

void SelfEncryptor::PutToReadCache(const char *data,
                                   const uint32_t &length,
                                   const uint64_t &position) {
  if (!prepared_for_reading_)
    return;
  if (position < cache_start_position_ + kDefaultByteArraySize_ &&
      position + length >= cache_start_position_) {
    uint32_t data_offset(0), cache_offset(0);
    uint32_t copy_size(length);
    if (position < cache_start_position_) {
      data_offset = static_cast<uint32_t>(cache_start_position_ - position);
      copy_size -= data_offset;
    } else {
      cache_offset = static_cast<uint32_t>(position - cache_start_position_);
    }
    copy_size = std::min(copy_size, kDefaultByteArraySize_ - cache_offset);
    memcpy(read_cache_.get() + cache_offset, data + data_offset, copy_size);
  }
}

void SelfEncryptor::CalculateSizes(bool force) {
  if (normal_chunk_size_ != kDefaultChunkSize || force) {
    if (file_size_ < 3 * kMinChunkSize) {
      normal_chunk_size_ = 0;
      last_chunk_position_ = std::numeric_limits<uint64_t>::max();
      return;
    } else if (file_size_ < 3 * kDefaultChunkSize) {
      normal_chunk_size_ = static_cast<uint32_t>(file_size_) / 3;
      last_chunk_position_ = 2 * normal_chunk_size_;
      return;
    }
    normal_chunk_size_ = kDefaultChunkSize;
  }
  uint32_t chunk_count_excluding_last =
      static_cast<uint32_t>(file_size_ / kDefaultChunkSize);
  if (file_size_ % kDefaultChunkSize < kMinChunkSize)
    --chunk_count_excluding_last;
  last_chunk_position_ = chunk_count_excluding_last * kDefaultChunkSize;
}

uint32_t SelfEncryptor::PutToInitialChunks(const char *data,
                                           uint32_t *length,
                                           uint64_t *position) {
  uint32_t copy_length0(0);
  // Handle Chunk 0
  if (*position < kDefaultChunkSize) {
    copy_length0 =
        std::min(*length, kDefaultChunkSize - static_cast<uint32_t>(*position));
#ifndef NDEBUG
    uint32_t copied =
#endif
        MemCopy(chunk0_raw_, static_cast<uint32_t>(*position), data,
                copy_length0);
    BOOST_ASSERT(copy_length0 == copied);
    // Don't decrease current_position_ (could be a rewrite - this shouldn't
    // change current_position_).
    if (current_position_ < *position + copy_length0)
      current_position_ = *position + copy_length0;
    *length -= copy_length0;
    *position += copy_length0;
    chunk0_modified_ = true;
  }

  // Handle Chunk 1
  uint32_t copy_length1(0);
  if ((*position >= kDefaultChunkSize) && (*position < 2 * kDefaultChunkSize)) {
    copy_length1 = std::min(*length,
        (2 * kDefaultChunkSize) - static_cast<uint32_t>(*position));
#ifndef NDEBUG
    uint32_t copied =
#endif
        MemCopy(chunk1_raw_,
                static_cast<uint32_t>(*position - kDefaultChunkSize),
                data + copy_length0, copy_length1);
    BOOST_ASSERT(copy_length1 == copied);
    // Don't decrease current_position_ (could be a rewrite - this shouldn't
    // change current_position_).
    if (current_position_ < *position + copy_length1)
      current_position_ = *position + copy_length1;
    *length -= copy_length1;
    *position += copy_length1;
    chunk1_modified_ = true;
  }

  return copy_length0 + copy_length1;
}

bool SelfEncryptor::GetDataOffsetForEnqueuing(const uint32_t &length,
                                              const uint64_t &position,
                                              uint32_t *data_offset,
                                              uint32_t *queue_offset) {
  // Cover most common case first
  if (position == current_position_) {
    *data_offset = 0;
    *queue_offset =
        static_cast<uint32_t>(current_position_ - queue_start_position_);
    return current_position_ >= queue_start_position_;
  }

  if (length == 0)
    return false;

  if (position < queue_start_position_) {
    // We don't care if this overflows as in this case we return false
    *data_offset = static_cast<uint32_t>(queue_start_position_ - position);
    *queue_offset = 0;
    return (position + length >= queue_start_position_);
  } else if (position <= queue_start_position_ + retrievable_from_queue_) {
    *data_offset = 0;
    *queue_offset = static_cast<uint32_t>(position - queue_start_position_);
    return true;
  }
  return false;
}

int SelfEncryptor::PutToEncryptQueue(const char *data,
                                     uint32_t length,
                                     uint32_t data_offset,
                                     uint32_t queue_offset) {
  length -= data_offset;
  uint32_t copy_length =
      std::min(length, kQueueCapacity_ - retrievable_from_queue_);
  uint32_t copied(0);
  while (copy_length != 0) {
    copied = MemCopy(main_encrypt_queue_, queue_offset, data + data_offset,
                     copy_length);
    BOOST_ASSERT(copy_length == copied);
    current_position_ = std::max(queue_start_position_ + copied + queue_offset,
                                 current_position_);
    retrievable_from_queue_ = static_cast<uint32_t>(current_position_ -
                                                    queue_start_position_);
    if (retrievable_from_queue_ == kQueueCapacity_) {
      int result(ProcessMainQueue());
      if (result != kSuccess)
        return result;
      queue_offset = retrievable_from_queue_;
    } else {
      queue_offset += copy_length;
    }
    data_offset += copy_length;
    length -= copy_length;
    copy_length = std::min(length, kDefaultByteArraySize_);
  }
  return kSuccess;
}

bool SelfEncryptor::GetLengthForSequencer(const uint64_t &position,
                                          uint32_t *length) {
  if (*length == 0)
    return false;
  BOOST_ASSERT(position >= 2 * kDefaultChunkSize);
  if (position + *length < queue_start_position_) {
    *length = static_cast<uint32_t>(std::min(static_cast<uint64_t>(*length),
                                             queue_start_position_ - position));
    return true;
  }
  return (position > queue_start_position_ + retrievable_from_queue_);
}

int SelfEncryptor::DecryptChunk(const uint32_t &chunk_num, byte *data) {
  if (data_map_->chunks.size() <= chunk_num) {
    DLOG(WARNING) << "Can't decrypt chunk " << chunk_num << " of "
                  << data_map_->chunks.size();
    return kInvalidChunkIndex;
  }

  uint32_t length = data_map_->chunks[chunk_num].size;
  ByteArray pad(GetNewByteArray(kPadSize));
  ByteArray key(GetNewByteArray(crypto::AES256_KeySize));
  ByteArray iv(GetNewByteArray(crypto::AES256_IVSize));
  GetPadIvKey(chunk_num, key, iv, pad, false);
  std::string content;
#pragma omp critical
  {  // NOLINT (Fraser)
    content = chunk_store_->Get(data_map_->chunks[chunk_num].hash);
  }

  if (content.empty()) {
    DLOG(ERROR) << "Could not find chunk number " << chunk_num
                << ", hash " << EncodeToHex(data_map_->chunks[chunk_num].hash);
    return kMissingChunk;
  }

  try {
    CryptoPP::CFB_Mode<CryptoPP::AES>::Decryption decryptor(
        key.get(), crypto::AES256_KeySize, iv.get());
    CryptoPP::StringSource filter(content, true,
        new XORFilter(
            new CryptoPP::StreamTransformationFilter(decryptor,
                new CryptoPP::Gunzip(new CryptoPP::MessageQueue)), pad.get()));
    filter.Get(data, length);
  }
  catch(const std::exception &e) {
    DLOG(ERROR) << e.what();
    return kDecryptionException;
  }
  return kSuccess;
}

void SelfEncryptor::GetPadIvKey(uint32_t this_chunk_num,
                                ByteArray key,
                                ByteArray iv,
                                ByteArray pad,
                                bool writing) {
  uint32_t num_chunks = static_cast<uint32_t>(data_map_->chunks.size());
  uint32_t n_1_chunk = (this_chunk_num + num_chunks - 1) % num_chunks;
  uint32_t n_2_chunk = (this_chunk_num + num_chunks - 2) % num_chunks;
  // Chunks 0 and 1 aren't encrypted until all others are done - we need to get
  // their pre-encryption hashes here if required.
  if (prepared_for_writing_) {
    if (n_1_chunk == 0 || n_2_chunk == 0) {
      CryptoPP::SHA512().CalculateDigest(data_map_->chunks[0].pre_hash,
                                         chunk0_raw_.get(), normal_chunk_size_);
    }
    if (n_1_chunk == 1 || n_2_chunk == 1) {
      if (normal_chunk_size_ == kDefaultChunkSize) {
        CryptoPP::SHA512().CalculateDigest(data_map_->chunks[1].pre_hash,
                                           chunk1_raw_.get(),
                                           kDefaultChunkSize);
      } else if (normal_chunk_size_ * 2 <= kDefaultChunkSize) {
        // All of chunk 0 and chunk 1 data in chunk0_raw_.
        CryptoPP::SHA512().CalculateDigest(data_map_->chunks[1].pre_hash,
            chunk0_raw_.get() + normal_chunk_size_, normal_chunk_size_);
      } else {
        // Some at end of chunk0_raw_ and rest in start of chunk1_raw_.
        ByteArray temp(GetNewByteArray(normal_chunk_size_));
        uint32_t size_chunk0(kDefaultChunkSize - normal_chunk_size_);
        uint32_t size_chunk1(normal_chunk_size_ - size_chunk0);
        uint32_t copied = MemCopy(temp, 0,
                                  chunk0_raw_.get() + normal_chunk_size_,
                                  size_chunk0);
        BOOST_ASSERT(size_chunk0 == copied);
        copied = MemCopy(temp, size_chunk0, chunk1_raw_.get(), size_chunk1);
        BOOST_ASSERT(size_chunk1 == copied);
        CryptoPP::SHA512().CalculateDigest(data_map_->chunks[1].pre_hash,
                                            temp.get(), normal_chunk_size_);
      }
    }
  }

  const byte* n_1_pre_hash =
      data_map_->chunks[this_chunk_num].old_n1_pre_hash.get();
  const byte* n_2_pre_hash =
      data_map_->chunks[this_chunk_num].old_n2_pre_hash.get();
  if (writing) {
    if (n_1_pre_hash) {
      BOOST_ASSERT(n_2_pre_hash);
      data_map_->chunks[this_chunk_num].old_n1_pre_hash.reset();
      data_map_->chunks[this_chunk_num].old_n2_pre_hash.reset();
    }
    n_1_pre_hash = &data_map_->chunks[n_1_chunk].pre_hash[0];
    n_2_pre_hash = &data_map_->chunks[n_2_chunk].pre_hash[0];
  } else {
    if (!n_1_pre_hash) {
      BOOST_ASSERT(!n_2_pre_hash);
      n_1_pre_hash = &data_map_->chunks[n_1_chunk].pre_hash[0];
      n_2_pre_hash = &data_map_->chunks[n_2_chunk].pre_hash[0];
    }
  }

  uint32_t copied = MemCopy(key, 0, n_2_pre_hash, crypto::AES256_KeySize);
  BOOST_ASSERT(crypto::AES256_KeySize == copied);
  copied = MemCopy(iv, 0, n_2_pre_hash + crypto::AES256_KeySize,
                   crypto::AES256_IVSize);
  BOOST_ASSERT(crypto::AES256_IVSize == copied);
  copied = MemCopy(pad, 0, n_1_pre_hash, crypto::SHA512::DIGESTSIZE);
  BOOST_ASSERT(crypto::SHA512::DIGESTSIZE == copied);
  copied = MemCopy(pad, crypto::SHA512::DIGESTSIZE,
                   &data_map_->chunks[this_chunk_num].pre_hash[0],
                   crypto::SHA512::DIGESTSIZE);
  BOOST_ASSERT(crypto::SHA512::DIGESTSIZE == copied);
  uint32_t hash_offset(crypto::AES256_KeySize + crypto::AES256_IVSize);
  copied = MemCopy(pad, (2 * crypto::SHA512::DIGESTSIZE),
                   n_2_pre_hash + hash_offset,
                   crypto::SHA512::DIGESTSIZE - hash_offset);
  BOOST_ASSERT(crypto::SHA512::DIGESTSIZE - hash_offset == copied);
}

int SelfEncryptor::ProcessMainQueue() {
  if (retrievable_from_queue_ < kDefaultChunkSize)
    return kSuccess;

  uint32_t chunks_to_process(retrievable_from_queue_ / kDefaultChunkSize);
  if ((retrievable_from_queue_ % kDefaultChunkSize) < kMinChunkSize)
    --chunks_to_process;

  BOOST_ASSERT((last_chunk_position_ - queue_start_position_) %
               kDefaultChunkSize == 0);

  uint32_t first_queue_chunk_index =
      static_cast<uint32_t>(queue_start_position_ / kDefaultChunkSize);
  data_map_->chunks.resize(first_queue_chunk_index + chunks_to_process);
#pragma omp parallel for
  for (int64_t i = 0; i < chunks_to_process; ++i) {
    CryptoPP::SHA512().CalculateDigest(
        data_map_->chunks[first_queue_chunk_index +
                          static_cast<uint32_t>(i)].pre_hash,
        main_encrypt_queue_.get() +
            (static_cast<uint32_t>(i) * kDefaultChunkSize),
        kDefaultChunkSize);
  }

  int result(kSuccess);
#pragma omp parallel for
  for (int64_t i = 0; i < chunks_to_process; ++i) {
    int res(EncryptChunk(first_queue_chunk_index + static_cast<uint32_t>(i),
                         main_encrypt_queue_.get() + (i * kDefaultChunkSize),
                         kDefaultChunkSize));
    if (res != kSuccess) {
      DLOG(ERROR) << "Failed processing main queue at chunk "
                  << first_queue_chunk_index + i;
#pragma omp critical
      {  // NOLINT (Fraser)
        result = res;
      }
    }
  }

  if (chunks_to_process != 0 && result == kSuccess) {
    uint32_t move_size(retrievable_from_queue_ -
                       (chunks_to_process * kDefaultChunkSize));
#ifndef NDEBUG
    uint32_t copied =
#endif
        MemCopy(main_encrypt_queue_, 0,
                main_encrypt_queue_.get() +
                    (chunks_to_process * kDefaultChunkSize),
                move_size);
    BOOST_ASSERT(move_size == copied);
    queue_start_position_ += (chunks_to_process * kDefaultChunkSize);
    retrievable_from_queue_ -= (chunks_to_process * kDefaultChunkSize);
  }
  return kSuccess;
}

int SelfEncryptor::EncryptChunk(const uint32_t &chunk_num,
                                byte *data,
                                const uint32_t &length) {
  BOOST_ASSERT(data_map_->chunks.size() > chunk_num);

  if (!data_map_->chunks[chunk_num].hash.empty()) {
#pragma omp critical
    {  // NOLINT (Fraser)
      HandleRewrite(chunk_num);
    }
  } else {
#pragma omp critical
    {  // NOLINT (Fraser)
      data_map_->chunks[chunk_num].hash.resize(crypto::SHA512::DIGESTSIZE);
    }
  }

  ByteArray pad(GetNewByteArray(kPadSize));
  ByteArray key(GetNewByteArray(crypto::AES256_KeySize));
  ByteArray iv(GetNewByteArray(crypto::AES256_IVSize));
  GetPadIvKey(chunk_num, key, iv, pad, true);
  int result(kSuccess);
  try {
    CryptoPP::CFB_Mode<CryptoPP::AES>::Encryption encryptor(
        key.get(), crypto::AES256_KeySize, iv.get());

    std::string chunk_content;
    chunk_content.reserve(length);
    CryptoPP::Gzip aes_filter(
        new CryptoPP::StreamTransformationFilter(encryptor,
            new XORFilter(
              new CryptoPP::StringSink(chunk_content), pad.get())), 6);
    aes_filter.Put2(data, length, -1, true);

    CryptoPP::SHA512().CalculateDigest(
        const_cast<byte*>(reinterpret_cast<const byte*>(
            data_map_->chunks[chunk_num].hash.data())),
        reinterpret_cast<const byte*>(chunk_content.data()),
        chunk_content.size());

#pragma omp critical
    {  // NOLINT (Fraser)
      if (!chunk_store_->Store(data_map_->chunks[chunk_num].hash,
                               chunk_content)) {
        DLOG(ERROR) << "Could not store "
                    << EncodeToHex(data_map_->chunks[chunk_num].hash);
        result = kFailedToStoreChunk;
      }
    }
  }
  catch(const std::exception &e) {
    DLOG(ERROR) << e.what();
    result = kEncryptionException;
  }

  data_map_->chunks[chunk_num].size = length;  // keep pre-compressed length
  return result;
}

void SelfEncryptor::HandleRewrite(const uint32_t &chunk_num) {
  if (!data_map_->chunks[chunk_num].hash.empty() &&
      !chunk_store_->Delete(data_map_->chunks[chunk_num].hash)) {
    DLOG(WARNING) << "Failed to delete chunk " << chunk_num << ": "
                  << EncodeToHex(data_map_->chunks[chunk_num].hash);
  }
  uint32_t num_chunks = static_cast<uint32_t>(data_map_->chunks.size());
  uint32_t n_minus_1_chunk = (chunk_num + num_chunks - 1) % num_chunks;
  uint32_t n_plus_1_chunk = (chunk_num + 1) % num_chunks;
  uint32_t n_plus_2_chunk = (chunk_num + 2) % num_chunks;
  if (!data_map_->chunks[n_plus_1_chunk].old_n1_pre_hash) {
    data_map_->chunks[n_plus_1_chunk].old_n1_pre_hash.reset(
        new byte[crypto::SHA512::DIGESTSIZE]);
    data_map_->chunks[n_plus_1_chunk].old_n2_pre_hash.reset(
        new byte[crypto::SHA512::DIGESTSIZE]);
    memcpy(data_map_->chunks[n_plus_1_chunk].old_n1_pre_hash.get(),
           &data_map_->chunks[chunk_num].pre_hash[0],
           crypto::SHA512::DIGESTSIZE);
    memcpy(data_map_->chunks[n_plus_1_chunk].old_n2_pre_hash.get(),
           &data_map_->chunks[n_minus_1_chunk].pre_hash[0],
           crypto::SHA512::DIGESTSIZE);
  }
  if (!data_map_->chunks[n_plus_2_chunk].old_n1_pre_hash) {
    data_map_->chunks[n_plus_2_chunk].old_n1_pre_hash.reset(
        new byte[crypto::SHA512::DIGESTSIZE]);
    data_map_->chunks[n_plus_2_chunk].old_n2_pre_hash.reset(
        new byte[crypto::SHA512::DIGESTSIZE]);
    memcpy(data_map_->chunks[n_plus_2_chunk].old_n1_pre_hash.get(),
           &data_map_->chunks[n_plus_1_chunk].pre_hash[0],
           crypto::SHA512::DIGESTSIZE);
    memcpy(data_map_->chunks[n_plus_2_chunk].old_n2_pre_hash.get(),
           &data_map_->chunks[chunk_num].pre_hash[0],
           crypto::SHA512::DIGESTSIZE);
  }
}

bool SelfEncryptor::Flush() {
  if (!prepared_for_writing_)
    return true;

  if (file_size_ < 3 * kMinChunkSize) {
    data_map_->content.assign(reinterpret_cast<char*>(chunk0_raw_.get()),
                              static_cast<size_t>(file_size_));
    return true;
  }

  // Re-calculate normal_chunk_size_ and last_chunk_position_
  uint32_t normal_chunk_size_before_flush(normal_chunk_size_);
  uint64_t last_chunk_position_before_flush(last_chunk_position_);
  CalculateSizes(true);

  // Empty queue (after this call it will contain 0 or 1 chunks).
  int result(ProcessMainQueue());
  if (result != kSuccess) {
    DLOG(ERROR) << "Failed in Flush.";
    return false;
  }

  uint64_t flush_position(2 * normal_chunk_size_);
  uint32_t chunk_index(2);
  bool pre_pre_chunk_modified(chunk0_modified_);
  bool pre_chunk_modified(chunk1_modified_);
  bool this_chunk_modified(false);
  bool this_chunk_has_data_in_sequencer(false);
  bool this_chunk_has_data_in_queue(false);
  bool this_chunk_has_data_in_c0_or_c1(false);

  std::pair<uint64_t, ByteArray> sequence_block(sequencer_->GetFirst());
  uint64_t sequence_block_position(sequence_block.first);
  ByteArray sequence_block_data(sequence_block.second);
  uint32_t sequence_block_size(Size(sequence_block.second));
  uint32_t sequence_block_copied(0);

  ByteArray chunk_array(GetNewByteArray(kDefaultChunkSize + kMinChunkSize));
  const uint32_t kOldChunkCount(
      static_cast<uint32_t>(data_map_->chunks.size()));
  data_map_->chunks.resize(
      static_cast<uint32_t>(last_chunk_position_ / normal_chunk_size_) + 1);

  uint32_t this_chunk_size(normal_chunk_size_);
  while (flush_position <= last_chunk_position_) {
    if (chunk_index == data_map_->chunks.size() - 1) {  // on last chunk
      this_chunk_size =
          static_cast<uint32_t>(file_size_ - last_chunk_position_);
    }

    memset(chunk_array.get(), 0, Size(chunk_array));
    if (sequence_block_position < flush_position + this_chunk_size) {
      this_chunk_has_data_in_sequencer = true;
      this_chunk_modified = true;
    }

    if (flush_position <= queue_start_position_ &&
        flush_position + this_chunk_size > queue_start_position_) {
      this_chunk_has_data_in_queue = true;
      this_chunk_modified = true;
    }

    if (flush_position < 2 * kDefaultChunkSize) {
      this_chunk_has_data_in_c0_or_c1 = true;
      this_chunk_modified = true;
    }

    // Read in any data from previously-encrypted chunk
    if (chunk_index < kOldChunkCount &&
        (pre_pre_chunk_modified || pre_chunk_modified || this_chunk_modified)) {
      int result(DecryptChunk(chunk_index, chunk_array.get()));
      if (result == kSuccess)
        chunk_store_->Delete(data_map_->chunks[chunk_index].hash);
    }

    // Overwrite with any data in chunk0_raw_ and/or chunk1_raw_
    uint32_t copied(0);
    if (this_chunk_has_data_in_c0_or_c1) {
      uint32_t offset(static_cast<uint32_t>(flush_position));
      uint32_t size_in_chunk0(0), c1_offset(0);
      if (offset < kDefaultChunkSize) {  // in chunk 0
        size_in_chunk0 = std::min(kDefaultChunkSize - offset, this_chunk_size);
        copied = MemCopy(chunk_array, 0, chunk0_raw_.get() + offset,
                         size_in_chunk0);
        BOOST_ASSERT(size_in_chunk0 == copied);
      } else if (offset < 2 * kDefaultChunkSize) {
        c1_offset = offset - kDefaultChunkSize;
      }
      uint32_t size_in_chunk1(std::min(this_chunk_size - size_in_chunk0,
                                       kDefaultChunkSize - c1_offset));
      if (size_in_chunk1 != 0) {  // in chunk 1
        copied += MemCopy(chunk_array, size_in_chunk0,
                          chunk1_raw_.get() + c1_offset, size_in_chunk1);
        BOOST_ASSERT(size_in_chunk0 + size_in_chunk1 == copied);
      }
    }

    // Overwrite with any data in queue
    if (this_chunk_has_data_in_queue) {
      copied = MemCopy(chunk_array, copied, main_encrypt_queue_.get(),
                       retrievable_from_queue_);
      BOOST_ASSERT(retrievable_from_queue_ == copied);
    }

    // Overwrite with any data from sequencer
    if (this_chunk_has_data_in_sequencer) {
      while (sequence_block_position + sequence_block_copied <
             flush_position + this_chunk_size) {
        uint32_t copy_size(std::min(sequence_block_size - sequence_block_copied,
            static_cast<uint32_t>(flush_position + this_chunk_size - (
                sequence_block_position + sequence_block_copied))));
        uint32_t copy_offset(0);
        if (sequence_block_position > flush_position)
          copy_offset = std::min(this_chunk_size - copy_size,
             static_cast<uint32_t>(sequence_block_position - flush_position));
        copied = MemCopy(chunk_array, copy_offset,
                         sequence_block_data.get() + sequence_block_copied,
                         copy_size);
        BOOST_ASSERT(copy_size == copied);
        if (sequence_block_copied + copy_size == sequence_block_size) {
          sequence_block = sequencer_->GetFirst();
          sequence_block_position = sequence_block.first;
          sequence_block_data = sequence_block.second;
          sequence_block_size = Size(sequence_block.second);
          sequence_block_copied = 0;
        } else {
          sequence_block_copied += copy_size;
        }
      }
    }

    if (chunk_index == data_map_->chunks.size() - 1) {
      CryptoPP::SHA512().CalculateDigest(
          data_map_->chunks[chunk_index].pre_hash, chunk_array.get(),
          this_chunk_size);
    }

    if (pre_pre_chunk_modified || pre_chunk_modified || this_chunk_modified) {
      result = EncryptChunk(chunk_index, chunk_array.get(), this_chunk_size);
      if (result != kSuccess) {
        DLOG(ERROR) << "Failed in Flush.";
        return false;
      }
    }

    flush_position += this_chunk_size;
    ++chunk_index;
    pre_pre_chunk_modified = pre_chunk_modified;
    pre_chunk_modified = this_chunk_modified;
    this_chunk_modified = false;
  }

  BOOST_ASSERT(flush_position == file_size_);

  if (pre_pre_chunk_modified || pre_chunk_modified || chunk0_modified_) {
    result = EncryptChunk(0, chunk0_raw_.get(), normal_chunk_size_);
    if (result != kSuccess) {
      DLOG(ERROR) << "Failed in Flush.";
      return false;
    }
  }

  pre_pre_chunk_modified = pre_chunk_modified;
  pre_chunk_modified = chunk0_modified_;

  if (pre_pre_chunk_modified || pre_chunk_modified || chunk1_modified_) {
    if (normal_chunk_size_ == kDefaultChunkSize) {
      result = EncryptChunk(1, chunk1_raw_.get(), normal_chunk_size_);
      if (result != kSuccess) {
        DLOG(ERROR) << "Failed in Flush.";
        return false;
      }
    } else if (normal_chunk_size_ * 2 <= kDefaultChunkSize) {
      // All of chunk 0 and chunk 1 data in chunk0_raw_
      result = EncryptChunk(1, chunk0_raw_.get() + normal_chunk_size_,
                            normal_chunk_size_);
      if (result != kSuccess) {
        DLOG(ERROR) << "Failed in Flush.";
        return false;
      }
    } else {
      // Some at end of chunk0_raw_ and rest in start of chunk1_raw_
      ByteArray temp(GetNewByteArray(normal_chunk_size_));
      uint32_t size_chunk0(kDefaultChunkSize - normal_chunk_size_);
      uint32_t size_chunk1(normal_chunk_size_ - size_chunk0);
      uint32_t copied = MemCopy(temp, 0, chunk0_raw_.get() + normal_chunk_size_,
                                size_chunk0);
      BOOST_ASSERT(size_chunk0 == copied);
      copied = MemCopy(temp, size_chunk0, chunk1_raw_.get(), size_chunk1);
      BOOST_ASSERT(size_chunk1 == copied);
      result = EncryptChunk(1, temp.get(), normal_chunk_size_);
      if (result != kSuccess) {
        DLOG(ERROR) << "Failed in Flush.";
        return false;
      }
    }
  }

  // Restore sizes, in case of further writes.
  normal_chunk_size_ = normal_chunk_size_before_flush;
  last_chunk_position_ = last_chunk_position_before_flush;
  return true;
}

bool SelfEncryptor::Read(char* data,
                         const uint32_t &length,
                         const uint64_t &position) {
  if (length == 0)
    return true;

  PrepareToRead();

  if (length < kDefaultByteArraySize_) {
    //  required -
    //  requested position not less than cache start and
    //  requested position + length not greater than cache end
    if (position < cache_start_position_ ||
        position + length > cache_start_position_ + kDefaultByteArraySize_) {
      // populate read_cache_.
      if (Transmogrify(read_cache_.get(), kDefaultByteArraySize_, position) !=
          kSuccess) {
        DLOG(ERROR) << "Failed to read " << length << "B at pos " << position;
        return false;
      }
      cache_start_position_ = position;
    }
    memcpy(data, read_cache_.get() + static_cast<uint32_t>(position -
           cache_start_position_), length);
  } else {
    // length requested larger than cache size, just go ahead and read
    if (Transmogrify(data, length, position) != kSuccess) {
      DLOG(ERROR) << "Failed to read " << length << "B at pos " << position;
      return false;
    }
  }
  return true;
}

void SelfEncryptor::PrepareToRead() {
  if (prepared_for_reading_)
    return;

  read_cache_.reset(new char[kDefaultByteArraySize_]);
  cache_start_position_ = std::numeric_limits<uint64_t>::max();
  prepared_for_reading_ = true;
}

int SelfEncryptor::Transmogrify(char *data,
                                const uint32_t &length,
                                const uint64_t &position) {
  memset(data, 0, length);

  // For tiny files, all data is in data_map_->content or chunk0_raw_.
  if (file_size_ < 3 * kMinChunkSize) {
    if (position >= 3 * kMinChunkSize) {
      DLOG(ERROR) << "Failed to transmogrify " << length << "B at position "
                  << position << " with file size of " << file_size_ << "B";
      return kInvalidPosition;
    }
    if (prepared_for_writing_) {
      uint32_t copy_size = std::min(length, (3 * kMinChunkSize) -
                                    static_cast<uint32_t>(position));
      memcpy(data, chunk0_raw_.get() + position, copy_size);
    } else {
      uint32_t copy_size = std::min(length,
                           static_cast<uint32_t>(data_map_->content.size()));
      memcpy(data, data_map_->content.data() + position, copy_size);
    }
    return kSuccess;
  }

  int result(ReadDataMapChunks(data, length, position));
  if (result != kSuccess) {
    DLOG(ERROR) << "Failed to read DM chunks during transmogrification of "
                << length << "B at position " << position;
    return result;
  }

  if (!prepared_for_writing_)
    return kSuccess;

  ReadInProcessData(data, length, position);
  return kSuccess;
}

int SelfEncryptor::ReadDataMapChunks(char *data,
                                     const uint32_t &length,
                                     const uint64_t &position) {
  if (data_map_->chunks.empty())
    return kSuccess;

  uint32_t num_chunks = static_cast<uint32_t>(data_map_->chunks.size());
  uint32_t start_chunk = static_cast<uint32_t>(position / normal_chunk_size_);
  uint32_t end_chunk = std::min(num_chunks - 1, static_cast<uint32_t>(
                                (position + length - 1) / normal_chunk_size_));
  BOOST_ASSERT(start_chunk < num_chunks);
  BOOST_ASSERT(end_chunk < num_chunks);
  uint32_t start_offset(position % normal_chunk_size_);
  uint32_t end_cut(0);
  uint64_t total_data_map_size(TotalSize(data_map_, normal_chunk_size_));
  if (position + length >= total_data_map_size) {
    end_cut = (*data_map_->chunks.rbegin()).size;
  } else {
    end_cut = static_cast<uint32_t>(position + length -
                                    (normal_chunk_size_ * end_chunk));
  }

  int result(kSuccess);
  if (start_chunk == end_chunk && data_map_->chunks[start_chunk].size != 0) {
    ByteArray chunk_data(GetNewByteArray(data_map_->chunks[start_chunk].size));
    result = DecryptChunk(start_chunk, chunk_data.get());
    if (result != kSuccess) {
      DLOG(ERROR) << "Failed to decrypt chunk " << start_chunk;
      return result;
    }
    for (uint32_t i = start_offset; i != length + start_offset; ++i)
      data[i - start_offset] = static_cast<char>(*(chunk_data.get() + i));
    return kSuccess;
  }

#pragma omp parallel for shared(data)
  for (int64_t i = start_chunk; i <= end_chunk; ++i) {
    uint32_t this_chunk_size(data_map_->chunks[static_cast<uint32_t>(i)].size);
    if (this_chunk_size != 0) {
      if (i == start_chunk) {
        if (start_offset != 0) {
          // Create temp array as we don't need data before "start_offset".
          ByteArray temp(GetNewByteArray(this_chunk_size));
          int res = DecryptChunk(start_chunk, temp.get());
          if (res != kSuccess) {
            DLOG(ERROR) << "Failed to decrypt chunk " << start_chunk;
#pragma omp critical
            {  // NOLINT (Fraser)
              result = res;
            }
          }
          memcpy(data, temp.get() + start_offset,
                 this_chunk_size - start_offset);
        } else {
          int res = DecryptChunk(static_cast<uint32_t>(i),
                                 reinterpret_cast<byte*>(&data[0]));
          if (res != kSuccess) {
            DLOG(ERROR) << "Failed to decrypt chunk " << i;
#pragma omp critical
            {  // NOLINT (Fraser)
              result = res;
            }
          }
        }
      } else {
        uint64_t pos(static_cast<uint32_t>(i) * normal_chunk_size_);
        if (i == end_chunk && end_cut != data_map_->chunks[end_chunk].size) {
          // Create temp array as we'll possibly have to read beyond the end of
          // what's available in variable "data".
          ByteArray temp(GetNewByteArray(this_chunk_size));
          int res = DecryptChunk(end_chunk, temp.get());
          if (res != kSuccess) {
            DLOG(ERROR) << "Failed to decrypt chunk " << end_chunk;
#pragma omp critical
            {  // NOLINT (Fraser)
              result = res;
            }
          }
          memcpy(data + pos - position, temp.get(), end_cut);
        } else {
          int res = DecryptChunk(static_cast<uint32_t>(i),
                    reinterpret_cast<byte*>(&data[pos - position]));
          if (res != kSuccess) {
            DLOG(ERROR) << "Failed to decrypt chunk " << i;
#pragma omp critical
            {  // NOLINT (Fraser)
              result = res;
            }
          }
        }
      }
    }
  }
  return result;
}

void SelfEncryptor::ReadInProcessData(char *data,
                                      uint32_t length,
                                      uint64_t position) {
  uint32_t copy_size(0), bytes_read(0);
  uint64_t read_position(position);
  // Get data from chunk 0 if required.
  if (read_position < kDefaultChunkSize) {
    copy_size = std::min(length, kDefaultChunkSize -
                         static_cast<uint32_t>(read_position));
    memcpy(data, chunk0_raw_.get() + read_position, copy_size);
    bytes_read += copy_size;
    read_position += copy_size;
    if (bytes_read == length)
      return;
  }
  // Get data from chunk 1 if required.
  if (read_position < 2 * kDefaultChunkSize) {
    copy_size = std::min(length - bytes_read, (2 * kDefaultChunkSize) -
                         static_cast<uint32_t>(read_position));
    memcpy(data + bytes_read,
           chunk1_raw_.get() + read_position - kDefaultChunkSize,
           copy_size);
    bytes_read += copy_size;
    read_position += copy_size;
    if (bytes_read == length)
      return;
  }

  // Get data from queue if required.
  uint32_t data_offset(0), queue_offset(0), copy_length(0);
  if (retrievable_from_queue_ != 0)  {
    if ((position < queue_start_position_ + retrievable_from_queue_) &&
        (position + length > queue_start_position_)) {
      if (position < queue_start_position_)
        data_offset = static_cast<uint32_t>(queue_start_position_ - position);
      else
        queue_offset = static_cast<uint32_t>(position - queue_start_position_);
      copy_length = std::min(length - data_offset,
                             retrievable_from_queue_ - queue_offset);
    }
    memcpy(data + data_offset, &*main_encrypt_queue_ + queue_offset,
           copy_length);
  }

  // Get data from sequencer if required.
  std::pair<uint64_t, ByteArray> sequence_block(sequencer_->Peek(position));
  uint64_t sequence_block_position(sequence_block.first);
  ByteArray sequence_block_data(sequence_block.second);
  uint32_t sequence_block_size(Size(sequence_block.second));
  uint64_t seq_position(position);
  uint32_t sequence_block_offset(0);

  while (position < sequence_block_position + sequence_block_size &&
         position + length >= sequence_block_position) {
    if (position < sequence_block_position) {
      data_offset = static_cast<uint32_t>(sequence_block_position - position);
      sequence_block_offset = 0;
    } else {
      data_offset = 0;
      sequence_block_offset =
          static_cast<uint32_t>(position - sequence_block_position);
    }
    copy_length = std::min(length - data_offset, static_cast<uint32_t>(
                           sequence_block_position + sequence_block_size -
                           position));

    memcpy(data + data_offset,
           sequence_block_data.get() + sequence_block_offset, copy_length);

    seq_position = sequence_block_position + sequence_block_size;
    sequence_block = sequencer_->Peek(seq_position);
    sequence_block_position = sequence_block.first;
    sequence_block_data = sequence_block.second;
    sequence_block_size = Size(sequence_block.second);
  }
}

bool SelfEncryptor::DeleteAllChunks() {
  for (uint32_t i(0); i != data_map_->chunks.size(); ++i) {
    if (!chunk_store_->Delete(data_map_->chunks[i].hash)) {
      DLOG(WARNING) << "Failed to delete chunk " << i;
      return false;
    }
  }
  data_map_->chunks.clear();
  return true;
}

bool SelfEncryptor::Truncate(const uint64_t &length) {
  uint64_t byte_count(0);
  uint32_t number_of_chunks(static_cast<uint32_t>(data_map_->chunks.size()));
//  bool delete_remainder(false), found_end(false);
  // if (data_map_->complete) {
    // Assume size < data_map.size
    for (uint32_t i = 0; i != number_of_chunks; ++i) {
      uint32_t chunk_size = data_map_->chunks[i].size;
      byte_count += chunk_size;
      if (byte_count > length) {
        // Found chunk with data at position 'size'.
        if (retrievable_from_queue_ != 0)
//          main_encrypt_queue_.SkipAll();
        sequencer_->clear();
        for (uint32_t j = i + 1; j != number_of_chunks; ++j) {
          if (!chunk_store_->Delete(data_map_->chunks[j].hash)) {
            DLOG(ERROR) << "Failed to delete chunk";
            return false;
          }
          data_map_->chunks.pop_back();
        }
        if (byte_count - length == chunk_size) {
          if (!chunk_store_->Delete(data_map_->chunks[i].hash)) {
            DLOG(ERROR) << "Failed to delete chunk";
            return false;
          }
          data_map_->chunks.pop_back();
        } else {
          ByteArray data(GetNewByteArray(chunk_size));
          DecryptChunk(i, data.get());
          BOOST_ASSERT(byte_count - length <= chunk_size);
//          uint32_t bytes_to_queue(chunk_size -
//                                  static_cast<uint32_t>(byte_count - length));
//          main_encrypt_queue_.Put2(data.get(), bytes_to_queue, 0, true);
          if (!chunk_store_->Delete(data_map_->chunks[i].hash)) {
            DLOG(ERROR) << "Failed to delete chunk";
            return false;
          }
          data_map_->chunks.pop_back();
        }
        current_position_ = length;
        data_map_->content.erase();
        return true;
      }
    }
    // Check data map content.

  // } else {
//    if (delete_remainder == true) {
//      sequencer_->EraseAll();
//      main_encrypt_queue_.SkipAll();
//    } else {
//      // check content
//    else
//      // check queue;
//    else
//      // check sequencer
//      if (length <= retrievable_from_queue_) {
//
//      }
//    }
  // }
  return true;
}

}  // namespace encrypt
}  // namespace maidsafe
