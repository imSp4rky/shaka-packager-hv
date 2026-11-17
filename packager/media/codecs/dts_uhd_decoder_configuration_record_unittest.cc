// Use of this source code is governed by the BSD-style license in LICENSE.
#include "packager/media/codecs/dts_uhd_decoder_configuration_record.h"
#include <gtest/gtest.h>

namespace shaka {
namespace media {
TEST(DtsUhdDecoderConfigurationRecordTest, RejectsTruncatedRecords) {
  DtsUhdDecoderConfigurationRecord config;
  for (size_t size = 0; size < 8; ++size)
    EXPECT_FALSE(config.Parse(std::vector<uint8_t>(size, 0)));
}
TEST(DtsUhdDecoderConfigurationRecordTest, PreservesExtensionsAndResetsOnFailure) {
  const std::vector<uint8_t> bytes = {
      0x01, 0x60, 0x01, 0x80, 0xa0, 0x3f, 0x80, 0x00, 0x12, 0x34};
  DtsUhdDecoderConfigurationRecord config;
  ASSERT_TRUE(config.Parse(bytes));
  EXPECT_EQ(bytes, config.data());
  EXPECT_FALSE(config.Parse({0}));
  EXPECT_TRUE(config.data().empty());
}
}  // namespace media
}  // namespace shaka
