/*
* ============================================================================
*
* Copyright [2010] maidsafe.net limited
*
* Description:  Utility Functions
* Version:      1.0
* Created:      2010-04-29-13.26.25
* Revision:     none
* Compiler:     gcc
* Author:       Team, dev@maidsafe.net
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

#include "maidsafe/common/commonutils.h"
#include <maidsafe/kademlia/contact.h>
#include <maidsafe/base/crypto.h>

namespace maidsafe {

bool ContactHasId(const std::string &id, const kad::Contact &contact) {
  return contact.node_id().String() == id;
}

std::string SHA512File(const boost::filesystem::path &file_path) {
  crypto::Crypto file_crypto;
  file_crypto.set_hash_algorithm(crypto::SHA_512);
  return file_crypto.Hash(file_path.string(), "", crypto::FILE_STRING, false);
}

std::string SHA512String(const std::string &input) {
  crypto::Crypto string_crypto;
  string_crypto.set_hash_algorithm(crypto::SHA_512);
  return string_crypto.Hash(input, "", crypto::STRING_STRING, false);
}

std::string SHA1File(const boost::filesystem::path &file_path) {
  crypto::Crypto file_crypto;
  file_crypto.set_hash_algorithm(crypto::SHA_1);
  return file_crypto.Hash(file_path.string(), "", crypto::FILE_STRING, false);
}

std::string SHA1String(const std::string &input) {
  crypto::Crypto string_crypto;
  string_crypto.set_hash_algorithm(crypto::SHA_1);
  return string_crypto.Hash(input, "", crypto::STRING_STRING, false);
}

std::string RSASign(const std::string &input, const std::string &private_key) {
  crypto::Crypto signer;
  return signer.AsymSign(input, "", private_key, crypto::STRING_STRING);
}

bool RSACheckSignedData(const std::string &input,
                        const std::string &signature,
                        const std::string &public_key) {
  crypto::Crypto validator;
  return validator.AsymCheckSig(input, signature, public_key,
                                crypto::STRING_STRING);
}

std::string RSAEncrypt(const std::string &input,
                       const std::string &public_key) {
  crypto::Crypto string_crypto;
  return string_crypto.AsymEncrypt(input, "", public_key,
                                   crypto::STRING_STRING);
}

std::string RSADecrypt(const std::string &input,
                       const std::string &private_key) {
  crypto::Crypto string_crypto;
  return string_crypto.AsymDecrypt(input, "", private_key,
                                   crypto::STRING_STRING);
}

std::string AESEncrypt(const std::string &input,
                       const std::string &key) {
  crypto::Crypto string_crypto;
  string_crypto.set_symm_algorithm(crypto::AES_256);
  return string_crypto.SymmEncrypt(input, "", crypto::STRING_STRING, key);
}

std::string AESDecrypt(const std::string &input,
                       const std::string &key) {
  crypto::Crypto string_crypto;
  string_crypto.set_symm_algorithm(crypto::AES_256);
  return string_crypto.SymmDecrypt(input, "", crypto::STRING_STRING, key);
}

std::string SecurePassword(const std::string &password,
                           const std::string &salt,
                           const boost::uint32_t &pin) {
  crypto::Crypto pass_crypto;
  return pass_crypto.SecurePassword(password, salt, pin);
}

std::string XORObfuscate(const std::string &first,
                         const std::string &second) {
  crypto::Crypto obf_crypto;
  return obf_crypto.Obfuscate(first, second, crypto::XOR);
}

}  // namespace maidsafe
