// Use of this source code is governed by the BSD-style license in LICENSE.
#ifndef PACKAGER_ISM_ISM_INPUT_H_
#define PACKAGER_ISM_ISM_INPUT_H_
#include <stddef.h>
#include <functional>
#include "packager/ism/ism_manifest.h"

namespace shaka {
namespace ism {
// A finite client manifest is prepared as initialized fragmented MP4. Its
// PIFF encryption data remains encrypted for the normal native decryptor.
// The caller owns the temporary file and must delete it after closing it.
struct PreparedInput {
  std::string path;
  std::vector<size_t> source_stream_indexes;
};
Status BuildInitialization(const Manifest& manifest,
                           const std::vector<size_t>& selected,
                           std::vector<uint8_t>* bytes);
Status RewriteFragment(uint32_t track_id, uint64_t start,
                       const std::vector<uint8_t>& fragment,
                       std::vector<uint8_t>* output);
Status PrepareInput(const std::string& path,
                    const std::vector<std::string>& selectors,
                    const std::function<bool()>& cancelled,
                    const std::string& temp_dir,
                    PreparedInput* input);
}  // namespace ism
}  // namespace shaka
#endif
