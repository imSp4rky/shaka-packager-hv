// Use of this source code is governed by the BSD-style license in LICENSE.
#include "packager/ism/ism_input.h"
#include "packager/media/base/fourccs.h"
#include <gtest/gtest.h>
#include <algorithm>

namespace shaka {
namespace ism {
namespace {
using Bytes = std::vector<uint8_t>;
uint32_t U32(const Bytes& v, size_t p) {
  return (uint32_t(v[p]) << 24) | (uint32_t(v[p+1]) << 16) |
         (uint32_t(v[p+2]) << 8) | v[p+3];
}
void Put32(uint32_t n, size_t p, Bytes* v) {
  for (int i = 3; i >= 0; --i) { (*v)[p+i] = n; n >>= 8; }
}
void Add32(uint32_t n, Bytes* v) {
  const auto p = v->size(); v->resize(p + 4); Put32(n, p, v);
}
void Add64(uint64_t n, Bytes* v) { Add32(n >> 32, v); Add32(n, v); }
void Append(const Bytes& src, Bytes* dst) { dst->insert(dst->end(), src.begin(), src.end()); }
Bytes Box(uint32_t type, const Bytes& body, bool wide = false) {
  Bytes result; Add32(wide ? 1 : body.size() + 8, &result); Add32(type, &result);
  if (wide) Add64(body.size() + 16, &result);
  Append(body, &result); return result;
}
size_t Find(const Bytes& v, const char* type) {
  const auto it = std::search(v.begin(), v.end(), type, type + 4);
  return it == v.end() ? v.size() : static_cast<size_t>(it - v.begin()) - 4;
}
struct Options {
  bool absolute = false, implicit = false, wide = false, padding = false;
  bool prefix = false, tfdt = false, two_runs = false, implicit_second = false;
  bool auxiliary = false, external_auxiliary = false, aux64 = false;
  bool negative_offset = false, default_size = false;
};
Bytes Fragment(const Options& o = Options()) {
  Bytes tfhd;
  Add32(0x020000 | (o.absolute ? 1 : 0) | (o.default_size ? 16 : 0), &tfhd);
  Add32(7, &tfhd); if (o.absolute) Add64(0, &tfhd);
  if (o.default_size) Add32(1, &tfhd);
  Bytes traf = Box(media::FOURCC_tfhd, tfhd);
  const unsigned runs = o.two_runs ? 2 : 1;
  for (unsigned i = 0; i < runs; ++i) {
    const bool implicit = i ? o.implicit_second : o.implicit;
    Bytes trun;
    Add32(0x100 | (o.default_size ? 0 : 0x200) | (implicit ? 0 : 1), &trun);
    Add32(1, &trun); if (!implicit) Add32(0, &trun);
    Add32(3600, &trun); if (!o.default_size) Add32(1, &trun);
    Append(Box(media::FOURCC_trun, trun), &traf);
  }
  if (o.auxiliary) {
    Bytes saio;
    Add32(o.aux64 ? 0x01000001 : 0, &saio);
    if (o.aux64) { Add32(media::FOURCC_cenc, &saio); Add32(0, &saio); }
    Add32(1, &saio); if (o.aux64) Add64(0, &saio); else Add32(0, &saio);
    Append(Box(media::FOURCC_saio, saio), &traf);
    if (!o.external_auxiliary) {
      Bytes senc; Add32(0, &senc); Add32(1, &senc); Add64(0x123456789abcdef0ULL, &senc);
      Append(Box(media::FOURCC_senc, senc), &traf);
    }
  }
  if (o.tfdt) {
    Bytes clock; Add32(0x01000000, &clock); Add64(123456789, &clock);
    Append(Box(media::FOURCC_tfdt, clock), &traf);
  }
  Bytes moof = Box(media::FOURCC_traf, traf, o.wide);
  moof = Box(media::FOURCC_moof, moof, o.wide);
  Bytes result;
  if (o.prefix) Append(Box(media::FOURCC_free, Bytes(8)), &result);
  const size_t moof_start = result.size();
  Append(moof, &result);
  if (o.padding) Append(Box(media::FOURCC_free, Bytes(8, 0xaa)), &result);
  Bytes payload;
  if (o.auxiliary && o.external_auxiliary) Add64(0x123456789abcdef0ULL, &payload);
  payload.push_back(0x9a); if (o.two_runs) payload.push_back(0x7b);
  Append(Box(media::FOURCC_mdat, payload, o.wide), &result);
  const size_t data = Find(result, "mdat") + (o.wide ? 16 : 8);
  const size_t samples = data + (o.auxiliary && o.external_auxiliary ? 8 : 0);
  const uint64_t base = !o.absolute ? moof_start : o.negative_offset ? samples + 10 : 0;
  if (o.absolute) {
    const size_t field = Find(result, "tfhd") + 16;
    Put32(base >> 32, field, &result); Put32(base, field + 4, &result);
  }
  size_t pos = 0;
  for (unsigned i = 0; i < runs; ++i) {
    const auto it = std::search(result.begin() + pos, result.end(), "trun", "trun" + 4);
    pos = static_cast<size_t>(it - result.begin());
    if (!(i ? o.implicit_second : o.implicit))
      Put32(static_cast<uint32_t>(samples + i - base), pos + 12, &result);
    pos += 4;
  }
  if (o.auxiliary) {
    const size_t target = o.external_auxiliary ? data : Find(result, "senc") + 16;
    const size_t field = Find(result, "saio") + (o.aux64 ? 24 : 16);
    const uint64_t offset = target - base;
    if (o.aux64) { Put32(offset >> 32, field, &result); Put32(offset, field+4, &result); }
    else Put32(offset, field, &result);
  }
  return result;
}
void CheckSample(const Bytes& result, uint8_t value = 0x9a) {
  const auto run = Find(result, "trun");
  ASSERT_LT(run + 20, result.size());
  const auto offset = U32(result, run + 16);
  ASSERT_LT(offset, result.size()); EXPECT_EQ(value, result[offset]);
}
}  // namespace

TEST(IsmFragmentTest, RelocatesAbsoluteZeroBaseAndKeepsSampleBytes) {
  Options o; o.absolute = true;
  Bytes result; ASSERT_TRUE(RewriteFragment(3, 90000, Fragment(o), &result).ok());
  CheckSample(result);
  const auto header = Find(result, "tfhd");
  EXPECT_EQ(16u, U32(result, header)); EXPECT_EQ(0x020000u, U32(result, header+8));
  EXPECT_EQ(3u, U32(result, header+12));
}
TEST(IsmFragmentTest, ConvertsSignedOffsetsFromAnExplicitBase) {
  Options o; o.absolute = o.negative_offset = true;
  Bytes result; ASSERT_TRUE(RewriteFragment(1, 0, Fragment(o), &result).ok());
  CheckSample(result);
}
TEST(IsmFragmentTest, AddsAnOffsetToAnUnambiguousImplicitPiffRun) {
  Options o; o.absolute = o.implicit = true;
  Bytes result; ASSERT_TRUE(RewriteFragment(1, 0, Fragment(o), &result).ok());
  CheckSample(result);
  EXPECT_TRUE(U32(result, Find(result, "trun")+8) & 1);
}
TEST(IsmFragmentTest, PreservesPaddingAndHandlesAResponsePrefix) {
  Options o; o.padding = o.prefix = o.absolute = true;
  Bytes result; ASSERT_TRUE(RewriteFragment(1, 0, Fragment(o), &result).ok());
  CheckSample(result);
  EXPECT_EQ(0u, Find(result, "moof"));
  EXPECT_NE(result.size(), Find(result, "free"));
}
TEST(IsmFragmentTest, SupportsExtendedSizeContainersAndMdat) {
  Options o; o.wide = o.padding = o.implicit = true;
  Bytes result; ASSERT_TRUE(RewriteFragment(1, 0, Fragment(o), &result).ok());
  CheckSample(result);
  EXPECT_EQ(1u, U32(result, 0)); EXPECT_EQ(1u, U32(result, Find(result, "mdat")));
}
TEST(IsmFragmentTest, RetainsAnExistingDecodeTimeInsteadOfRebasingIt) {
  Options o; o.tfdt = o.absolute = true;
  Bytes result; ASSERT_TRUE(RewriteFragment(1, 999, Fragment(o), &result).ok());
  CheckSample(result);
  const auto clock = Find(result, "tfdt");
  EXPECT_EQ(123456789u, U32(result, clock+16));
}
TEST(IsmFragmentTest, ResolvesFollowingImplicitRunsFromPreviousRunEnd) {
  Options o; o.two_runs = o.implicit_second = o.default_size = true;
  Bytes result; ASSERT_TRUE(RewriteFragment(1, 0, Fragment(o), &result).ok());
  CheckSample(result);
  const auto first = Find(result, "trun");
  const auto second = first + U32(result, first);
  const auto offset = U32(result, second+16);
  ASSERT_LT(offset, result.size()); EXPECT_EQ(0x7bu, result[offset]);
}
TEST(IsmFragmentTest, RelocatesInlineAuxiliaryOffsetsWhenTheHeaderShrinks) {
  Options o; o.absolute = o.auxiliary = true;
  Bytes result; ASSERT_TRUE(RewriteFragment(1, 0, Fragment(o), &result).ok());
  CheckSample(result);
  const auto offset = U32(result, Find(result, "saio")+16);
  EXPECT_EQ(Find(result, "senc")+16, offset);
  EXPECT_EQ(0x12345678u, U32(result, offset));
}
TEST(IsmFragmentTest, RelocatesExternalAuxiliaryOffsetsAndTheirTypeFields) {
  for (bool wide : {false, true}) {
    Options o; o.auxiliary = o.external_auxiliary = true; o.aux64 = wide;
    Bytes result; ASSERT_TRUE(RewriteFragment(1, 0, Fragment(o), &result).ok());
    CheckSample(result);
    const auto field = Find(result, "saio") + (wide ? 28 : 16);
    const auto offset = U32(result, field);
    EXPECT_EQ(Find(result, "mdat")+8, offset);
    EXPECT_EQ(0x12345678u, U32(result, offset));
  }
}
TEST(IsmFragmentTest, RejectsAmbiguousImplicitOffsetsAndForeignFileBases) {
  Options o; o.two_runs = o.implicit = true;
  Bytes result; EXPECT_FALSE(RewriteFragment(1, 0, Fragment(o), &result).ok());
  o = Options(); o.absolute = true;
  auto source = Fragment(o); Put32(1, Find(source, "tfhd")+16, &source);
  EXPECT_FALSE(RewriteFragment(1, 0, source, &result).ok());
}
TEST(IsmFragmentTest, RejectsSampleCountsAndPayloadRangesThatCannotFit) {
  Bytes result, source = Fragment();
  Put32(1000001, Find(source, "trun")+12, &source);
  EXPECT_FALSE(RewriteFragment(1, 0, source, &result).ok());
  source = Fragment(); Put32(2, Find(source, "trun")+24, &source);
  EXPECT_FALSE(RewriteFragment(1, 0, source, &result).ok());
}
TEST(IsmFragmentTest, RejectsMalformedDecodeTimesAndAuxiliaryOffsets) {
  Options o; o.tfdt = true; Bytes result;
  auto source = Fragment(o); Put32(0x02000000, Find(source, "tfdt")+8, &source);
  EXPECT_FALSE(RewriteFragment(1, 0, source, &result).ok());
  o = Options(); o.auxiliary = true;
  source = Fragment(o); Put32(0xffffffff, Find(source, "saio")+16, &source);
  EXPECT_FALSE(RewriteFragment(1, 0, source, &result).ok());
  source = Fragment(o); Put32(2, Find(source, "saio")+12, &source);
  EXPECT_FALSE(RewriteFragment(1, 0, source, &result).ok());
}
TEST(IsmFragmentTest, MapsTheSelectedQualityToItsSingleSampleDescription) {
  auto source = Fragment(); const auto header = Find(source, "tfhd");
  source.insert(source.begin()+header+16, 4, 0);
  Put32(2, header+16, &source); Put32(U32(source,header+8)|2, header+8, &source);
  Put32(U32(source,header)+4, header, &source);
  Put32(U32(source,0)+4, 0, &source); Put32(U32(source,8)+4, 8, &source);
  const auto run = Find(source, "trun"); Put32(U32(source,run+16)+4, run+16, &source);
  Bytes result; ASSERT_TRUE(RewriteFragment(1, 0, source, &result).ok());
  CheckSample(result); EXPECT_EQ(1u, U32(result, Find(result,"tfhd")+16));
}
TEST(IsmFragmentTest, PreservesSignedCompositionOffsetsAndSampleDurations) {
  auto source = Fragment(); const auto run = Find(source, "trun");
  source.insert(source.begin()+run+U32(source,run), 4, 0);
  Put32(0xfffff1f0, run+28, &source);  // -3600 composition offset.
  Put32(0x01000b01, run+8, &source);
  Put32(U32(source,run)+4, run, &source);
  Put32(U32(source,0)+4, 0, &source); Put32(U32(source,8)+4, 8, &source);
  Put32(U32(source,run+16)+4, run+16, &source);
  Bytes result; ASSERT_TRUE(RewriteFragment(1, 0, source, &result).ok());
  CheckSample(result);
  const auto new_run = Find(result, "trun");
  EXPECT_EQ(3600u, U32(result,new_run+20));
  EXPECT_EQ(0xfffff1f0u, U32(result,new_run+28));
}
}  // namespace ism
}  // namespace shaka
