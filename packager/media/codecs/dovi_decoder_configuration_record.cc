// Copyright 2019 Google LLC. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include "packager/media/codecs/dovi_decoder_configuration_record.h"

#include "packager/base/strings/stringprintf.h"
#include "packager/media/base/bit_reader.h"
#include "packager/media/base/rcheck.h"

namespace shaka {
namespace media {

bool DOVIDecoderConfigurationRecord::GetProfile5Level(
    uint32_t width,
    uint32_t height,
    uint32_t time_scale,
    int64_t sample_duration,
    uint8_t* level) {
  if (!level)
    return false;
  *level = 0;
  if (width == 0 || height == 0 || width > 3840 || height > 2160 ||
      time_scale == 0 || sample_duration <= 0) {
    return false;
  }
  struct LevelLimit {
    uint32_t max_width;
    uint32_t max_height;
    uint32_t max_pixels_per_second;
  };
  // Conservative picture-size bounds for common HD, FHD, QHD and UHD formats.
  // Pixel-rate limits retain fractional rates such as 24000/1001 exactly.
  const LevelLimit limits[] = {
      {1280, 720, 22118400},   {1280, 720, 27648000},
      {1920, 1080, 49766400},  {1920, 1080, 62208000},
      {2560, 1440, 124416000}, {3840, 2160, 199065600},
      {3840, 2160, 248832000}, {3840, 2160, 398131200},
      {3840, 2160, 497664000},
  };
  // The size bounds above keep this product below UINT64_MAX even for the
  // largest uint32 timescale. Ceiling division avoids multiplying by duration.
  const uint64_t pixels = static_cast<uint64_t>(width) * height * time_scale;
  for (size_t i = 0; i < sizeof(limits) / sizeof(limits[0]); ++i) {
    const LevelLimit& limit = limits[i];
    if (width <= limit.max_width && height <= limit.max_height &&
        (pixels + limit.max_pixels_per_second - 1) /
                limit.max_pixels_per_second <=
            static_cast<uint64_t>(sample_duration)) {
      *level = static_cast<uint8_t>(i + 1);
      return true;
    }
  }
  return false;
}

bool DOVIDecoderConfigurationRecord::IsProfile5Rpu(const uint8_t* nalu,
                                                  size_t size) {
  if (!nalu || size < 10 || nalu[0] != 0x7c ||
      (nalu[1] & 0xf8) != 0 || (nalu[1] & 7) == 0) {
    return false;
  }
  std::vector<uint8_t> rbsp;
  rbsp.reserve(size - 2);
  unsigned zero_count = 0;
  for (size_t i = 2; i < size; ++i) {
    if (zero_count >= 2 && nalu[i] == 3) {
      if (i + 1 == size || nalu[i + 1] > 3)
        return false;
      zero_count = 0;
      continue;
    }
    rbsp.push_back(nalu[i]);
    zero_count = nalu[i] == 0 ? zero_count + 1 : 0;
  }
  while (!rbsp.empty() && rbsp.back() == 0)
    rbsp.pop_back();
  if (rbsp.size() < 10 || rbsp.front() != 0x19 || rbsp.back() != 0x80)
    return false;

  const size_t crc_offset = rbsp.size() - 5;
  uint32_t crc = 0xffffffff;
  for (size_t i = 1; i < crc_offset; ++i) {
    crc ^= static_cast<uint32_t>(rbsp[i]) << 24;
    for (int bit = 0; bit < 8; ++bit)
      crc = (crc << 1) ^ ((crc & 0x80000000) ? 0x04c11db7 : 0);
  }
  const uint32_t expected =
      (static_cast<uint32_t>(rbsp[crc_offset]) << 24) |
      (static_cast<uint32_t>(rbsp[crc_offset + 1]) << 16) |
      (static_cast<uint32_t>(rbsp[crc_offset + 2]) << 8) |
      rbsp[crc_offset + 3];
  if (crc != expected)
    return false;

  BitReader reader(rbsp.data(), crc_offset);
  uint8_t prefix, type, profile, level, coefficient_type, normalized;
  uint16_t format;
  bool sequence, chroma_filter, full_range;
  if (!(reader.ReadBits(8, &prefix) && reader.ReadBits(6, &type) &&
        reader.ReadBits(11, &format) && reader.ReadBits(4, &profile) &&
        reader.ReadBits(4, &level) && reader.ReadBits(1, &sequence)) ||
      prefix != 0x19 || type != 2 || format != 18 || profile != 0 ||
      level != 0 || !sequence) {
    return false;
  }
  if (!(reader.ReadBits(1, &chroma_filter) &&
        reader.ReadBits(2, &coefficient_type)) || coefficient_type != 0) {
    return false;
  }
  unsigned leading_zeros = 0;
  bool bit;
  do {
    if (!reader.ReadBits(1, &bit) || leading_zeros > 30)
      return false;
    if (!bit)
      ++leading_zeros;
  } while (!bit);
  uint32_t suffix = 0;
  if (leading_zeros && !reader.ReadBits(leading_zeros, &suffix))
    return false;
  const uint32_t denominator = (uint32_t{1} << leading_zeros) - 1 + suffix;
  return denominator == 23 && reader.ReadBits(2, &normalized) &&
         normalized == 1 && reader.ReadBits(1, &full_range) && full_range;
}

bool DOVIDecoderConfigurationRecord::Parse(const std::vector<uint8_t>& data) {
  // Check before constructing BitReader, which requires a non-empty buffer.
  RCHECK(data.size() >= 4);
  BitReader reader(data.data(), data.size());

  // Versions 1.0, 2.0 and 3.0 share the version/profile/level prefix used
  // here. In particular, profile 20 MV-HEVC streams can use version 3.0.
  // The complete configuration record is passed through unchanged by the
  // MP4 parser/muxer; it must not be rewritten as a version 1.0 record.
  uint8_t major_version = 0;
  uint8_t minor_version = 0;
  RCHECK(reader.ReadBits(8, &major_version) && major_version >= 1 &&
         major_version <= 3 &&
         reader.ReadBits(8, &minor_version) && minor_version == 0 &&
         reader.ReadBits(7, &profile_) && reader.ReadBits(6, &level_));
  return true;
}

std::string DOVIDecoderConfigurationRecord::GetCodecString(
    FourCC codec_fourcc) const {
  // Dolby Vision Streams within the HTTP Live Streaming format Version 2.0:
  // https://www.dolby.com/us/en/technologies/dolby-vision/dolby-vision-streams-within-the-http-live-streaming-format-v2.0.pdf
  return base::StringPrintf(
      "%s.%02d.%02d", FourCCToString(codec_fourcc).c_str(), profile_, level_);
}

}  // namespace media
}  // namespace shaka
