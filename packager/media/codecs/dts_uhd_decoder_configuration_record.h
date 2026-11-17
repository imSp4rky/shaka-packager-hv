// Use of this source code is governed by the BSD-style license in LICENSE.
#ifndef PACKAGER_MEDIA_CODECS_DTS_UHD_DECODER_CONFIGURATION_RECORD_H_
#define PACKAGER_MEDIA_CODECS_DTS_UHD_DECODER_CONFIGURATION_RECORD_H_

#include <stdint.h>
#include <vector>

namespace shaka {
namespace media {

// DTS-UHD's udts configuration is carried unchanged when remuxing dtsx.
// The fixed record prefix is eight bytes; extension data must be preserved.
// This class validates the envelope, not the compressed audio frames.
class DtsUhdDecoderConfigurationRecord {
 public:
  bool Parse(const std::vector<uint8_t>& data);
  const std::vector<uint8_t>& data() const { return data_; }

 private:
  std::vector<uint8_t> data_;
};

}  // namespace media
}  // namespace shaka
#endif
