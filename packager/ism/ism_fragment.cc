// Use of this source code is governed by the BSD-style license in LICENSE.
#include "packager/ism/ism_input.h"

#include <algorithm>
#include <limits>
#include "packager/media/base/fourccs.h"

namespace shaka {
namespace ism {
namespace {
using namespace media;
Status Invalid(const std::string& message) {
  return Status(error::PARSER_FAILURE, "ISM fragment: " + message);
}
uint32_t U32(const std::vector<uint8_t>& data, size_t p) {
  return (uint32_t(data[p]) << 24) | (uint32_t(data[p+1]) << 16) |
         (uint32_t(data[p+2]) << 8) | data[p+3];
}
uint64_t U64(const std::vector<uint8_t>& data, size_t p) {
  return (uint64_t(U32(data, p)) << 32) | U32(data, p + 4);
}
void Put32(uint32_t n, size_t p, std::vector<uint8_t>* data) {
  for (int i = 3; i >= 0; --i) { (*data)[p+i] = n; n >>= 8; }
}
void Put64(uint64_t n, size_t p, std::vector<uint8_t>* data) {
  Put32(n >> 32, p, data); Put32(n, p + 4, data);
}
struct Box { size_t pos, size, header; uint32_t type; };
bool Boxes(const std::vector<uint8_t>& data, size_t begin, size_t end,
           std::vector<Box>* boxes) {
  for (size_t p = begin; p < end;) {
    if (end - p < 8) return false;
    uint64_t size = U32(data, p);
    size_t header = 8;
    if (size == 1) {
      if (end - p < 16) return false;
      size = U64(data, p + 8); header = 16;
    }
    if (size == 0) size = end - p;
    if (size < header || size > end - p) return false;
    boxes->push_back(Box{p, static_cast<size_t>(size), header, U32(data, p+4)});
    p += static_cast<size_t>(size);
  }
  return true;
}
bool One(const std::vector<Box>& boxes, uint32_t type, Box* result) {
  size_t count = 0;
  for (const auto& box : boxes) if (box.type == type) { *result = box; ++count; }
  return count == 1;
}
void SetSize(const Box& box, size_t size, std::vector<uint8_t>* data) {
  if (box.header == 16) Put64(size, box.pos + 8, data);
  else Put32(static_cast<uint32_t>(size), box.pos, data);
}
struct Edit {
  size_t pos, size;
  std::vector<uint8_t> bytes;
};
// Translate original response positions through all structural edits. Auxiliary
// data inside an edited header is invalid; unchanged senc/UUID/mdat bytes map
// exactly, even when a preceding tfhd shrinks and several truns grow.
bool Map(size_t original, const std::vector<Edit>& edits, size_t* result) {
  int64_t delta = 0;
  for (const auto& edit : edits) {
    if (original >= edit.pos + edit.size)
      delta += static_cast<int64_t>(edit.bytes.size()) - edit.size;
    else if (original > edit.pos)
      return false;
    else
      break;
  }
  if (delta < 0 && original < static_cast<uint64_t>(-delta)) return false;
  *result = static_cast<size_t>(original + delta);
  return true;
}
bool Target(uint64_t base, int64_t offset, uint64_t* target) {
  if (offset < 0) {
    if (base < static_cast<uint64_t>(-offset)) return false;
    *target = base - static_cast<uint64_t>(-offset);
  } else {
    if (base > UINT64_MAX - static_cast<uint64_t>(offset)) return false;
    *target = base + offset;
  }
  return true;
}
struct Run {
  Box box;
  size_t target;
};
}  // namespace

Status RewriteFragment(uint32_t track_id, uint64_t start,
                       const std::vector<uint8_t>& fragment,
                       std::vector<uint8_t>* output) {
  if (!output || !track_id) return Invalid("invalid output or track ID");
  std::vector<Box> roots;
  Box moof{}, mdat{};
  if (!Boxes(fragment, 0, fragment.size(), &roots) ||
      !One(roots, FOURCC_moof, &moof) || !One(roots, FOURCC_mdat, &mdat) ||
      mdat.pos < moof.pos + moof.size)
    return Invalid("expected one bounded moof followed by one mdat");
  const size_t payload = mdat.pos + mdat.header, payload_end = mdat.pos + mdat.size;
  std::vector<Box> children;
  Box traf{}, tfhd{};
  if (!Boxes(fragment, moof.pos + moof.header, moof.pos + moof.size, &children) ||
      !One(children, FOURCC_traf, &traf))
    return Invalid("expected one track per fragment");
  children.clear();
  if (!Boxes(fragment, traf.pos + traf.header, traf.pos + traf.size, &children) ||
      !One(children, FOURCC_tfhd, &tfhd) || tfhd.size < tfhd.header + 8)
    return Invalid("missing or malformed tfhd");

  const uint32_t header_flags = U32(fragment, tfhd.pos + tfhd.header);
  if (header_flags & ~uint32_t{0x03003b}) return Invalid("unsupported tfhd flags/version");
  size_t p = tfhd.pos + tfhd.header + 8;
  uint64_t base = moof.pos;
  const bool absolute_base = header_flags & 1;
  if (absolute_base) {
    if (p + 8 > tfhd.pos + tfhd.size) return Invalid("short base data offset");
    base = U64(fragment, p); p += 8;
  }
  uint32_t default_size = 0;
  for (uint32_t flag : {2u, 8u, 16u, 32u}) {
    if (header_flags & flag) {
      if (p + 4 > tfhd.pos + tfhd.size) return Invalid("short tfhd defaults");
      if (flag == 16) default_size = U32(fragment, p);
      p += 4;
    }
  }
  if (p != tfhd.pos + tfhd.size) return Invalid("invalid tfhd length");
  std::vector<Edit> edits;
  Edit header{tfhd.pos, tfhd.size,
              {fragment.begin() + tfhd.pos, fragment.begin() + tfhd.pos + tfhd.size}};
  if (absolute_base)
    header.bytes.erase(header.bytes.begin() + tfhd.header + 8,
                       header.bytes.begin() + tfhd.header + 16);
  const Box header_view{0, header.bytes.size(), tfhd.header, FOURCC_tfhd};
  SetSize(header_view, header.bytes.size(), &header.bytes);
  Put32((header_flags & ~1u) | 0x020000, tfhd.header, &header.bytes);
  Put32(track_id, tfhd.header + 4, &header.bytes);
  // Initialization contains the selected quality's single sample entry.
  if (header_flags & 2) Put32(1, tfhd.header + 8, &header.bytes);
  edits.push_back(std::move(header));

  size_t tfdt_count = 0, run_count = 0;
  for (const auto& box : children) {
    if (box.type == FOURCC_trun) ++run_count;
    if (box.type != FOURCC_tfdt) continue;
    ++tfdt_count;
    if (box.size < box.header + 8) return Invalid("short tfdt");
    const uint32_t vf = U32(fragment, box.pos + box.header);
    if ((vf != 0 && vf != 0x01000000) ||
        box.size != box.header + (vf ? 12 : 8))
      return Invalid("invalid tfdt version or length");
  }
  if (tfdt_count > 1 || !run_count) return Invalid("duplicate tfdt or missing trun");
  if (!tfdt_count) {
    Edit clock{traf.pos + traf.size, 0, std::vector<uint8_t>(20)};
    Put32(20, 0, &clock.bytes); Put32(FOURCC_tfdt, 4, &clock.bytes);
    Put32(0x01000000, 8, &clock.bytes); Put64(start, 12, &clock.bytes);
    edits.push_back(std::move(clock));
  }
  std::vector<Run> runs;
  uint64_t previous_end = 0;
  for (const auto& box : children) {
    if (box.type != FOURCC_trun) continue;
    p = box.pos + box.header;
    if (box.size < box.header + 8) return Invalid("short trun");
    const uint32_t vf = U32(fragment, p), count = U32(fragment, p + 4);
    if (count > 1000000 || (vf >> 24) > 1 ||
        (vf & 0xffffff & ~uint32_t{0x000f05}))
      return Invalid("unsupported trun version/flags");
    p += 8;
    uint64_t target = runs.empty() ? base : previous_end;
    if (vf & 1) {
      if (p + 4 > box.pos + box.size) return Invalid("short trun offset");
      const uint32_t raw_offset = U32(fragment, p);
      const int64_t offset = raw_offset <= INT32_MAX ? raw_offset :
                            static_cast<int64_t>(raw_offset) - 0x100000000LL;
      if (!Target(base, offset, &target)) return Invalid("overflowing trun target");
      p += 4;
    }
    if (vf & 4) p += 4;  // first_sample_flags.
    const size_t fields = ((vf >> 8) & 1) + ((vf >> 9) & 1) +
                          ((vf >> 10) & 1) + ((vf >> 11) & 1);
    if (p > box.pos + box.size ||
        (fields && count > (box.pos + box.size - p) / (4 * fields)) ||
        box.pos + box.size - p != uint64_t(count) * fields * 4)
      return Invalid("truncated trun samples");
    uint64_t sample_bytes = uint64_t(count) * default_size;
    if (vf & 0x200) {
      sample_bytes = 0;
      const size_t size_at = p + ((vf & 0x100) ? 4 : 0);
      for (size_t n = 0; n < count; ++n)
        sample_bytes += U32(fragment, size_at + n * fields * 4);
    }
    if (count && !sample_bytes) return Invalid("missing sample sizes");
    // Some PIFF responses omit both an effective base and trun.data_offset.
    // A single run consuming the complete mdat is unambiguous. Never guess
    // a start for multiple runs or for payload mixed with auxiliary bytes.
    if (!(vf & 1) && runs.empty() && (base == 0 || base == moof.pos) &&
        run_count == 1 && sample_bytes == payload_end - payload)
      target = payload;
    if (target < payload || target > payload_end ||
        sample_bytes > payload_end - target ||
        (!runs.empty() && target < previous_end))
      return Invalid("sample run does not fit its mdat");
    previous_end = target + sample_bytes;
    runs.push_back(Run{box, static_cast<size_t>(target)});
    if (!(vf & 1)) {
      Edit run{box.pos, box.size,
               {fragment.begin() + box.pos, fragment.begin() + box.pos + box.size}};
      run.bytes.insert(run.bytes.begin() + box.header + 8, 4, 0);
      const Box view{0, run.bytes.size(), box.header, FOURCC_trun};
      SetSize(view, run.bytes.size(), &run.bytes);
      Put32(vf | 1, box.header, &run.bytes);
      edits.push_back(std::move(run));
    }
  }
  std::sort(edits.begin(), edits.end(),
      [](const Edit& a, const Edit& b) { return a.pos < b.pos; });
  std::vector<uint8_t> result(fragment.begin(), fragment.begin() + payload_end);
  for (auto it = edits.rbegin(); it != edits.rend(); ++it) {
    result.erase(result.begin() + it->pos, result.begin() + it->pos + it->size);
    result.insert(result.begin() + it->pos, it->bytes.begin(), it->bytes.end());
  }
  size_t moof_end, traf_end;
  if (!Map(moof.pos + moof.size, edits, &moof_end) ||
      !Map(traf.pos + traf.size, edits, &traf_end)) return Invalid("box mapping failed");
  SetSize(moof, moof_end - moof.pos, &result);
  SetSize(traf, traf_end - traf.pos, &result);
  for (const auto& run : runs) {
    size_t box_at, target;
    if (!Map(run.box.pos, edits, &box_at) || !Map(run.target, edits, &target) ||
        target < moof.pos || target - moof.pos > INT32_MAX)
      return Invalid("trun offset overflow");
    Put32(static_cast<uint32_t>(target - moof.pos), box_at + run.box.header + 8, &result);
  }
  for (const auto& box : children) {
    if (box.type != FOURCC_saio) continue;
    p = box.pos + box.header;
    if (box.size < box.header + 8) return Invalid("short saio");
    const uint32_t vf = U32(fragment, p);
    if ((vf >> 24) > 1 || (vf & 0xffffff & ~1u)) return Invalid("invalid saio version/flags");
    p += 4 + ((vf & 1) ? 8 : 0);
    const size_t width = (vf >> 24) ? 8 : 4;
    if (p + 4 > box.pos + box.size) return Invalid("short saio count");
    const uint32_t count = U32(fragment, p); p += 4;
    if (count > (box.pos + box.size - p) / width ||
        p + uint64_t(count) * width != box.pos + box.size)
      return Invalid("invalid saio length");
    for (size_t n = 0; n < count; ++n, p += width) {
      const uint64_t offset = width == 8 ? U64(fragment, p) : U32(fragment, p);
      if (offset > UINT64_MAX - base || base + offset < moof.pos ||
          base + offset >= payload_end) return Invalid("auxiliary offset outside fragment");
      size_t field, target;
      if (!Map(p, edits, &field) || !Map(static_cast<size_t>(base + offset), edits, &target) ||
          target < moof.pos || (width == 4 && target - moof.pos > UINT32_MAX))
        return Invalid("auxiliary offset cannot be relocated");
      if (width == 8) Put64(target - moof.pos, field, &result);
      else Put32(static_cast<uint32_t>(target - moof.pos), field, &result);
    }
  }
  size_t mdat_at;
  if (!Map(mdat.pos, edits, &mdat_at)) return Invalid("mdat mapping failed");
  if (U32(fragment, mdat.pos) == 0) Put32(static_cast<uint32_t>(mdat.size), mdat_at, &result);
  // Keep any padding between moof and mdat; offsets may include that padding.
  result.erase(result.begin(), result.begin() + moof.pos);
  output->swap(result);
  return Status::OK;
}
}  // namespace ism
}  // namespace shaka
