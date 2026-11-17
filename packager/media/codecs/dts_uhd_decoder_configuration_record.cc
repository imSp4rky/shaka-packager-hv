// Use of this source code is governed by the BSD-style license in LICENSE.
#include "packager/media/codecs/dts_uhd_decoder_configuration_record.h"

namespace shaka {
namespace media {

bool DtsUhdDecoderConfigurationRecord::Parse(const std::vector<uint8_t>& data) {
  data_.clear();
  if (data.size() < 8)
    return false;
  data_ = data;
  return true;
}

}  // namespace media
}  // namespace shaka
