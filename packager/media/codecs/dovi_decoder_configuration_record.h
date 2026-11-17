// Copyright 2019 Google LLC. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#ifndef PACKAGER_MEDIA_CODECS_DOVI_DECODER_CONFIGURATION_RECORD_H_
#define PACKAGER_MEDIA_CODECS_DOVI_DECODER_CONFIGURATION_RECORD_H_

#include <stdint.h>
#include <string>
#include <vector>

#include "packager/media/base/fourccs.h"

namespace shaka {
namespace media {

/// Class for parsing Dolby Vision decoder configuration record.
// Reads the shared profile/level prefix of record versions 1.0, 2.0 and 3.0.
// The full record, including any extensions, is preserved by the MP4 muxer.
// Implemented according to Dolby Vision Streams Within the ISO Base Media File
// Format Version 2.0:
// https://www.dolby.com/us/en/technologies/dolby-vision/dolby-vision-bitstreams-within-the-iso-base-media-file-format-v2.0.pdf
// and Dolby Vision Streams within the HTTP Live Streaming format Version 2.0:
// https://www.dolby.com/us/en/technologies/dolby-vision/dolby-vision-streams-within-the-http-live-streaming-format-v2.0.pdf
class DOVIDecoderConfigurationRecord {
 public:
  DOVIDecoderConfigurationRecord() = default;
  ~DOVIDecoderConfigurationRecord() = default;

  /// Parses input to extract decoder configuration record.
  /// @return false if there are parsing errors.
  bool Parse(const std::vector<uint8_t>& data);

  /// Recognize a single-layer profile 5 HEVC RPU from its bitstream header.
  /// Includes emulation-prevention removal and MPEG-2 CRC verification.
  /// Other profiles and incomplete/corrupt RPUs are not inferred.
  static bool IsProfile5Rpu(const uint8_t* nalu, size_t size);

  /// Select a conservative profile-5 level for a validated single-layer stream.
  /// Uses coded size and the exact timescale/sample-duration ratio, without
  /// floating-point frame-rate rounding. Covers common formats through UHD60;
  /// unsupported dimensions/rates require an existing configuration record.
  static bool GetProfile5Level(uint32_t width,
                              uint32_t height,
                              uint32_t time_scale,
                              int64_t sample_duration,
                              uint8_t* level);

  /// @return The codec string in the format defined by RFC6381. It is used in
  ///         DASH and HLS manifests.
  std::string GetCodecString(FourCC codec_fourcc) const;

 private:
  DOVIDecoderConfigurationRecord(const DOVIDecoderConfigurationRecord&) =
      delete;
  DOVIDecoderConfigurationRecord& operator=(
      const DOVIDecoderConfigurationRecord&) = delete;

  uint8_t profile_ = 0;
  uint8_t level_ = 0;
};

}  // namespace media
}  // namespace shaka

#endif  // PACKAGER_MEDIA_CODECS_DOVI_DECODER_CONFIGURATION_RECORD_H_
