// Use of this source code is governed by the BSD-style license in LICENSE.
#ifndef PACKAGER_ISM_ISM_MANIFEST_H_
#define PACKAGER_ISM_ISM_MANIFEST_H_
#include <stdint.h>
#include <string>
#include <vector>
#include "packager/status.h"

namespace shaka {
namespace ism {
struct Chunk {
  Chunk() = default;
  Chunk(uint64_t time, uint64_t length) : start(time), duration(length) {}
  uint64_t start = 0;
  uint64_t duration = 0;
};
struct Quality {
  uint32_t bitrate = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t sampling_rate = 0;
  uint16_t channels = 0;
  uint16_t bits_per_sample = 16;
  uint8_t nal_length_size = 4;
  std::string fourcc;
  std::vector<uint8_t> codec_private_data;
};
struct Stream {
  std::string type;
  std::string name;
  std::string language;
  std::string url;
  uint32_t timescale = 10000000;
  std::vector<Quality> qualities;
  std::vector<Chunk> chunks;
};
struct Manifest {
  uint32_t timescale = 10000000;
  uint64_t duration = 0;
  std::vector<Stream> streams;
  // PlayReady GUID byte order is normalized to the CENC key ID byte order.
  std::vector<uint8_t> key_id;
};

bool IsManifestPath(const std::string& path);
Status ParseManifest(const std::string& xml, Manifest* manifest);
Status ResolveFragment(const std::string& manifest_path, const Stream& stream,
                       const Quality& quality, const Chunk& chunk,
                       std::string* location);
}  // namespace ism
}  // namespace shaka
#endif
