// Use of this source code is governed by the BSD-style license in LICENSE.

#ifndef PACKAGER_MEDIA_FORMATS_MP2T_BBTS_DECRYPTOR_H_
#define PACKAGER_MEDIA_FORMATS_MP2T_BBTS_DECRYPTOR_H_

#include <stddef.h>
#include <stdint.h>
#include <array>
#include <vector>
#include <openssl/aes.h>

namespace shaka {
namespace media {
class KeySource;
namespace mp2t {

// MDCM/BBTS transport signaling and its video NAL cipher. Transport packet
// assembly and timestamps remain the responsibility of the native TS parser.
class BbtsDecryptor {
 public:
  using Iv = std::array<uint8_t, 16>;
  explicit BbtsDecryptor(KeySource* key_source);
  bool ParseSdt(const uint8_t* section, size_t size);
  bool active() const { return active_; }
  const Iv& iv() const { return iv_; }

  // The IV is snapshotted at the beginning of a PES, before later signaling.
  bool DecryptEs(const uint8_t* data, size_t size, size_t nalu_header_size,
                 const Iv& iv, std::vector<uint8_t>* clear) const;

 private:
  bool Configure(const uint8_t* service, size_t size);
  bool DecryptNalu(const uint8_t* data, size_t size, size_t header,
                   const Iv& iv, std::vector<uint8_t>* clear) const;
  KeySource* key_source_;
  bool active_ = false;
  bool has_key_ = false;
  Iv iv_{};
  AES_KEY aes_key_;
};

}  // namespace mp2t
}  // namespace media
}  // namespace shaka
#endif
