/*
* ============================================================================
*
* Copyright [2009] maidsafe.net limited
*
* Description:  Class that generates in thread RSA key pairs and keeps a buffer
                full
* Version:      1.0
* Created:      2010-03-18-00.23.23
* Revision:     none
* Compiler:     gcc
* Author:       Jose Cisneros
* Company:      maidsafe.net limited
*
* The following source code is property of maidsafe.net limited and is not
* meant for external use.  The use of this code is governed by the license
* file LICENSE.TXT found in the root of this directory and also on
* www.maidsafe.net.
*
* You are not free to copy, amend or otherwise use this source code without
* the explicit written permission of the board of directors of maidsafe.net.
*
* ============================================================================
*/

#include "maidsafe/client/cryptokeypairs.h"
#include "maidsafe/client/packetfactory.h"

namespace maidsafe {

CryptoKeyPairs::CryptoKeyPairs()
      : keypairs_done_(0), keypairs_todo_(0), keypairs_(),
        thrds_(kMaxCryptoThreadCount, boost::shared_ptr<boost::thread>()),
        keyslist_mutex_(), keys_done_mutex_(), keys_cond_(), start_mutex_(),
        started_(false), destroying_this_(false) {
}

CryptoKeyPairs::~CryptoKeyPairs() {
  destroying_this_ = true;
  std::vector< boost::shared_ptr<boost::thread> >::iterator it;
  for (it = thrds_.begin(); it != thrds_.end(); ++it) {
    if (*it) {
      (*it)->join();
    }
  }
}

bool CryptoKeyPairs::StartToCreateKeyPairs(const boost::int16_t
      &no_of_keypairs) {
  {
    boost::mutex::scoped_lock lock(start_mutex_);
    if (started_)
      return false;
    started_ = true;
  }
  keypairs_todo_ = no_of_keypairs;
  keypairs_done_ = keypairs_.size();
  boost::int16_t keys_needed = keypairs_todo_ - keypairs_done_;
  std::vector< boost::shared_ptr<boost::thread> >::iterator it;
  boost::int16_t i = 0;
  for (it = thrds_.begin(); it != thrds_.end() && i < keys_needed; ++it) {
    try {
      it->reset(new boost::thread(
          boost::bind(&CryptoKeyPairs::CreateKeyPair, this)));
      ++i;
    }
    catch(const boost::thread_resource_error&) {
      break;
    }
  }
  if (i == 0) {
    started_ = false;
  }
  return started_;
}

void CryptoKeyPairs::CreateKeyPair() {
  boost::this_thread::at_thread_exit(
      boost::bind(&CryptoKeyPairs::FinishedCreating, this));
  bool work_todo = true;
  while (work_todo && !destroying_this_) {
    crypto::RsaKeyPair rsakp;
    rsakp.GenerateKeys(kRsaKeySize);
    {
      boost::mutex::scoped_lock lock(keyslist_mutex_);
      keypairs_.push_back(rsakp);
    }
    keys_cond_.notify_all();
    {
      boost::mutex::scoped_lock lock(keys_done_mutex_);
      ++keypairs_done_;
      if (kMaxCryptoThreadCount - (keypairs_todo_ - keypairs_done_) > 0)
        work_todo = false;
    }
  }
}

void CryptoKeyPairs::FinishedCreating() {
  {
    boost::mutex::scoped_lock lock(keys_done_mutex_);
    if (keypairs_todo_ == keypairs_done_)
      started_ = false;
  }
  keys_cond_.notify_all();
}

bool CryptoKeyPairs::GetKeyPair(crypto::RsaKeyPair *keypair) {
  bool result;
  // All keys that were asked for have been created, all threads have finished
  if (!started_) {
    boost::mutex::scoped_lock lock(keyslist_mutex_);
    if (keypairs_.empty()) {
      result = false;
    } else {
      *keypair = keypairs_.front();
      keypairs_.pop_front();
      result = true;
    }
  } else {
    boost::mutex::scoped_lock lock(keyslist_mutex_);
    while (keypairs_.empty() && started_) {
      keys_cond_.wait(lock);
    }
    if (!keypairs_.empty()) {
      *keypair = keypairs_.front();
      keypairs_.pop_front();
      result = true;
    } else {
      result = false;
    }
  }
  return result;
}

}  // namespace maidsafe
