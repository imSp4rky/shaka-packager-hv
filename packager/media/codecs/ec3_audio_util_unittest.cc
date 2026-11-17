// Copyright 2016 Google Inc. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include <algorithm>

#include <gtest/gtest.h>

#include "packager/media/codecs/ec3_audio_util.h"

namespace shaka {
namespace media {

TEST(EC3AudioUtilTest, ChannelTest1) {
  // audio_coding_mode is 7, which is Left, Center, Right, Left surround, Right
  // surround. No dependent substreams. LFE channel on.
  const std::vector<uint8_t> ec3_data = {0, 0, 0, 0x0f, 0, 0x02, 0x10};

  uint32_t channel_map;
  uint32_t ec3_channel_mpeg_value;
  uint32_t ec3_joc_complexity;
  EXPECT_TRUE(CalculateEC3ChannelMap(ec3_data, &channel_map));
  EXPECT_EQ(0xF801u, channel_map);
  EXPECT_EQ(6u, GetEc3NumChannels(ec3_data));
  EXPECT_TRUE(CalculateEC3ChannelMPEGValue(ec3_data, &ec3_channel_mpeg_value));
  EXPECT_EQ(6u, ec3_channel_mpeg_value);
  EXPECT_TRUE(GetEc3JocComplexity(ec3_data, &ec3_joc_complexity));
  EXPECT_EQ(0u, ec3_joc_complexity);
}

TEST(EC3AudioUtilTest, ChannelTest2) {
  // audio_coding_mode is 2, which is Left and Right. No dependent substreams.
  // LFE channel off.
  const std::vector<uint8_t> ec3_data = {0, 0, 0, 0x04, 0};

  uint32_t channel_map;
  uint32_t ec3_channel_mpeg_value;
  uint32_t ec3_joc_complexity;
  EXPECT_TRUE(CalculateEC3ChannelMap(ec3_data, &channel_map));
  EXPECT_EQ(0xA000u, channel_map);
  EXPECT_EQ(2u, GetEc3NumChannels(ec3_data));
  EXPECT_TRUE(CalculateEC3ChannelMPEGValue(ec3_data, &ec3_channel_mpeg_value));
  EXPECT_EQ(2u, ec3_channel_mpeg_value);
  EXPECT_TRUE(GetEc3JocComplexity(ec3_data, &ec3_joc_complexity));
  EXPECT_EQ(0u, ec3_joc_complexity);
}

TEST(EC3AudioUtilTest, ChannelTest3) {
  // audio_coding_mode is 3, which is Left, Center, and Right. Dependent
  // substreams layout is 0b100000011, which is Left center/ Right center pair,
  // Left rear surround/ Right rear surround pair, LFE2 on. LFE channel on.
  const std::vector<uint8_t> ec3_data = {0, 0, 0, 0x07, 0x07, 0x03};

  uint32_t channel_map;
  uint32_t ec3_channel_mpeg_value;
  uint32_t ec3_joc_complexity;
  EXPECT_TRUE(CalculateEC3ChannelMap(ec3_data, &channel_map));
  EXPECT_EQ(0xE603u, channel_map);
  EXPECT_EQ(9u, GetEc3NumChannels(ec3_data));
  EXPECT_TRUE(CalculateEC3ChannelMPEGValue(ec3_data, &ec3_channel_mpeg_value));
  EXPECT_EQ(0xFFFFFFFFu, ec3_channel_mpeg_value);
  EXPECT_TRUE(GetEc3JocComplexity(ec3_data, &ec3_joc_complexity));
  EXPECT_EQ(0u, ec3_joc_complexity);
}

TEST(EC3AudioUtilTest, ChannelTest4) {
  // audio_coding_mode is 7, which is Left, Center, Right, Left surround and
  // Right surround. LFE channel on.
  const std::vector<uint8_t> ec3_data = {0x14, 0x00, 0x20, 0x0f, 0x00, 0x01,
                                         0x10};

  uint32_t channel_map;
  uint32_t ec3_channel_mpeg_value;
  uint32_t ec3_joc_complexity;
  EXPECT_TRUE(CalculateEC3ChannelMap(ec3_data, &channel_map));
  EXPECT_EQ(0xF801u, channel_map);
  EXPECT_EQ(6u, GetEc3NumChannels(ec3_data));
  EXPECT_TRUE(CalculateEC3ChannelMPEGValue(ec3_data, &ec3_channel_mpeg_value));
  EXPECT_EQ(6u, ec3_channel_mpeg_value);
  EXPECT_TRUE(GetEc3JocComplexity(ec3_data, &ec3_joc_complexity));
  EXPECT_EQ(16u, ec3_joc_complexity);
}

TEST(EC3AudioUtilTest, ReadsJocComplexityFromIndependentFrameBsi) {
  // Six-channel independent E-AC-3 frame, addbsi = {type A, complexity 16}.
  const uint8_t bsi[] = {0x0b, 0x77, 0x03, 0x7f, 0x3f, 0x86, 0x7f,
                         0xe8, 0xc2, 0x08, 0x08, 0x80, 0x0d, 0x40};
  std::vector<uint8_t> frame(1792, 0);
  std::copy(bsi, bsi + sizeof(bsi), frame.begin());
  uint8_t complexity = 0;
  ASSERT_TRUE(GetEc3JocComplexityFromFrame(frame.data(), frame.size(),
                                         &complexity));
  EXPECT_EQ(16, complexity);
  for (size_t size = 0; size < frame.size(); ++size) {
    EXPECT_FALSE(GetEc3JocComplexityFromFrame(frame.data(), size, &complexity));
  }
  frame[8] &= ~2;  // addbsie is absent; trailing bytes are not scanned.
  ASSERT_TRUE(GetEc3JocComplexityFromFrame(frame.data(), frame.size(),
                                         &complexity));
  EXPECT_EQ(0, complexity);
  frame[0] = 0;
  EXPECT_FALSE(GetEc3JocComplexityFromFrame(frame.data(), frame.size(),
                                          &complexity));
  EXPECT_FALSE(GetEc3JocComplexityFromFrame(nullptr, 0, &complexity));
}

}  // namespace media
}  // namespace shaka
