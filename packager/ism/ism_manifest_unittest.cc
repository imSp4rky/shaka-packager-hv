// Use of this source code is governed by the BSD-style license in LICENSE.
#include "packager/ism/ism_manifest.h"
#include <gtest/gtest.h>
namespace shaka {
namespace ism {
namespace {
const char kManifest[] =
    "<SmoothStreamingMedia Duration='120' TimeScale='10'>"
    "<StreamIndex Type='video' Url='QualityLevels({bitrate})/Fragments(video={start time})'>"
    "<QualityLevel Bitrate='1000' FourCC='H264' MaxWidth='640' MaxHeight='360' CodecPrivateData='0001'/>"
    "<c t='20' d='10' r='2'/><c t='70' d='5'/>"
    "</StreamIndex></SmoothStreamingMedia>";
std::string Timeline(const std::string& chunks, const std::string& attributes = "") {
  return "<SmoothStreamingMedia Duration='120' TimeScale='10'>"
      "<StreamIndex Type='video' Url='video/{start_time}' " + attributes + ">"
      "<QualityLevel Bitrate='1000' FourCC='H264' CodecPrivateData='0001'/>" +
      chunks + "</StreamIndex></SmoothStreamingMedia>";
}
std::string Encode(const std::vector<uint8_t>& bytes) {
  const std::string alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string result;
  for (size_t p = 0; p < bytes.size(); p += 3) {
    const uint32_t value = (uint32_t(bytes[p]) << 16) |
        (p+1 < bytes.size() ? uint32_t(bytes[p+1]) << 8 : 0) |
        (p+2 < bytes.size() ? bytes[p+2] : 0);
    result += alphabet[(value >> 18) & 63]; result += alphabet[(value >> 12) & 63];
    result += p+1 < bytes.size() ? alphabet[(value >> 6) & 63] : '=';
    result += p+2 < bytes.size() ? alphabet[value & 63] : '=';
  }
  return result;
}
std::string Protected(const std::string& wrm, const std::string& system =
    "{9a04f079-9840-4286-ab92-e65be0885f95}") {
  const uint32_t length = 10 + 2 * wrm.size();
  std::vector<uint8_t> pro = {uint8_t(length), uint8_t(length >> 8),
      uint8_t(length >> 16), uint8_t(length >> 24), 1,0,1,0,
      uint8_t(2 * wrm.size()), uint8_t(2 * wrm.size() >> 8)};
  for (char c : wrm) { pro.push_back(c); pro.push_back(0); }
  std::string xml = kManifest;
  xml.insert(xml.find("</SmoothStreamingMedia>"),
      "<Protection><ProtectionHeader SystemID='" + system + "'>\n" + Encode(pro) +
      "\n</ProtectionHeader></Protection>");
  return xml;
}
}
TEST(IsmManifestTest, ExpandsTimelineWithoutChangingItsTimescale) {
  Manifest manifest;
  ASSERT_TRUE(ParseManifest(kManifest, &manifest).ok());
  EXPECT_EQ(10u, manifest.timescale);
  ASSERT_EQ(1u, manifest.streams.size());
  const auto& stream = manifest.streams[0];
  ASSERT_EQ(3u, stream.chunks.size());
  EXPECT_EQ(20u, stream.chunks[0].start);
  EXPECT_EQ(30u, stream.chunks[1].start);
  EXPECT_EQ(70u, stream.chunks[2].start);
  std::string location;
  ASSERT_TRUE(ResolveFragment("media/raw.ism", stream, stream.qualities[0],
                             stream.chunks[1], &location).ok());
  EXPECT_EQ("media/QualityLevels(1000)/Fragments(video=30)", location);
}
TEST(IsmManifestTest, RejectsDtdLiveUnboundedAndOverflowingTimelines) {
  Manifest manifest;
  EXPECT_FALSE(ParseManifest("<!DOCTYPE x [<!ENTITY a SYSTEM 'file:///private'>]>"
      "<SmoothStreamingMedia Duration='1'/>", &manifest).ok());
  EXPECT_FALSE(ParseManifest("<SmoothStreamingMedia IsLive='TRUE' Duration='0'/>", &manifest).ok());
  std::string xml = kManifest;
  xml.replace(xml.find("r='2'"), 5, "r='-1'");
  EXPECT_FALSE(ParseManifest(xml, &manifest).ok());
  xml = kManifest;
  xml.replace(xml.find("t='20'"), 6, "t='18446744073709551615'");
  EXPECT_FALSE(ParseManifest(xml, &manifest).ok());
}
TEST(IsmManifestTest, RejectsEscapingOrUnresolvedFragmentLocations) {
  Stream stream; Quality quality; Chunk chunk;
  std::string location;
  for (const char* url : {"../private", "file:///private", "//host/media", "{unknown}"}) {
    stream.url = url;
    EXPECT_FALSE(ResolveFragment("raw.ism", stream, quality, chunk, &location).ok());
  }
  EXPECT_TRUE(IsManifestPath("media/RAW.ISM?token=value"));
  EXPECT_FALSE(IsManifestPath("input.mp4"));
}

TEST(IsmManifestTest, ConvertsPlayReadyGuidToCencKeyId) {
  const std::string protection =
      "<Protection><ProtectionHeader SystemID='9A04F079-9840-4286-AB92-E65BE0885F95'>"
      "DgEAAAEAAQAEATwAVwBSAE0ASABFAEEARABFAFIAPgA8AEQAQQBUAEEAPgA8AFAAUgBPAFQARQBDAFQASQBOAEYATwA+ADwASwBJAEQAUwA+ADwASwBJAEQAIABBAEwARwBJAEQAPQAiAEEARQBTAEMAVABSACIAIABWAEEATABVAEUAPQAiAE0AeQBJAFIAQQBGAFYARQBkADIAYQBJAG0AYQBxADcAegBOADMAdQAvAHcAPQA9ACIALwA+ADwALwBLAEkARABTAD4APAAvAFAAUgBPAFQARQBDAFQASQBOAEYATwA+ADwALwBEAEEAVABBAD4APAAvAFcAUgBNAEgARQBBAEQARQBSAD4A"
      "</ProtectionHeader></Protection>";
  std::string xml = kManifest;
  xml.insert(xml.find("</SmoothStreamingMedia>"), protection);
  Manifest manifest;
  ASSERT_TRUE(ParseManifest(xml, &manifest).ok());
  const std::vector<uint8_t> expected = {
      0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
      0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
  EXPECT_EQ(expected, manifest.key_id);
}
TEST(IsmManifestTest, RepeatOneProducesExactlyOneChunk) {
  Manifest manifest;
  ASSERT_TRUE(ParseManifest(Timeline("<c d='10' r='1'/><c d='5'/>"), &manifest).ok());
  ASSERT_EQ(2u, manifest.streams[0].chunks.size());
  EXPECT_EQ(10u, manifest.streams[0].chunks[1].start);
}
TEST(IsmManifestTest, RejectsZeroExcessiveAndDurationlessRepeats) {
  Manifest manifest;
  for (const char* chunk : {"<c t='0' d='10' r='0'/>",
       "<c t='0' d='10' r='1000001'/>", "<c t='0' r='2'/>",
       "<c t='0' d='9223372036854775808' r='2'/>"})
    EXPECT_FALSE(ParseManifest(Timeline(chunk), &manifest).ok());
}
TEST(IsmManifestTest, InfersStartTimeCodedDurationsInStreamTicks) {
  Manifest manifest;
  ASSERT_TRUE(ParseManifest(Timeline(
      "<c t='200'/><c t='220'/><c t='240' d='7'/><c d='9'/>",
      "TimeScale='100'"), &manifest).ok());
  const auto& stream = manifest.streams[0];
  ASSERT_EQ(4u, stream.chunks.size());
  EXPECT_EQ(100u, stream.timescale);
  EXPECT_EQ(20u, stream.chunks[0].duration);
  EXPECT_EQ(20u, stream.chunks[1].duration);
  EXPECT_EQ(7u, stream.chunks[2].duration);
  EXPECT_EQ(247u, stream.chunks[3].start);
}
TEST(IsmManifestTest, ConvertsSingleChunkDurationWithoutFloatRounding) {
  Manifest manifest;
  ASSERT_TRUE(ParseManifest(Timeline("<c t='900'/>", "TimeScale='90000'"), &manifest).ok());
  EXPECT_EQ(1080000u, manifest.streams[0].chunks[0].duration);
  std::string xml = Timeline("<c t='0'/>", "TimeScale='3'");
  xml.replace(xml.find("Duration='120'"), 14, "Duration='1'");
  EXPECT_FALSE(ParseManifest(xml, &manifest).ok());
}
TEST(IsmManifestTest, RejectsAmbiguousAndNonIncreasingChunkStarts) {
  Manifest manifest;
  for (const char* chunks : {"<c/><c d='10'/>", "<c t='0'/><c d='10'/>",
       "<c t='20' d='10'/><c t='10' d='10'/>", "<c t='0'/><c t='0' d='1'/>"})
    EXPECT_FALSE(ParseManifest(Timeline(chunks), &manifest).ok());
}
TEST(IsmManifestTest, InheritsDimensionsAndPreservesQualityOverrides) {
  Manifest manifest;
  std::string xml = Timeline("<c d='10'/>", "MaxWidth='1920' MaxHeight='1080'");
  xml.insert(xml.find("CodecPrivateData"), "MaxWidth='1280' ");
  ASSERT_TRUE(ParseManifest(xml, &manifest).ok());
  EXPECT_EQ(1280u, manifest.streams[0].qualities[0].width);
  EXPECT_EQ(1080u, manifest.streams[0].qualities[0].height);
}
TEST(IsmManifestTest, RejectsNonFiniteBooleanSpellingsAndAcceptsFalse) {
  Manifest manifest;
  for (const char* value : {"True", "1", "maybe"}) {
    std::string xml = kManifest;
    xml.insert(xml.find("Duration"), std::string("IsLive='") + value + "' ");
    EXPECT_FALSE(ParseManifest(xml, &manifest).ok());
  }
  std::string xml = kManifest;
  xml.insert(xml.find("Duration"), "IsLive='0' ");
  ASSERT_TRUE(ParseManifest(xml, &manifest).ok());
}
TEST(IsmManifestTest, AcceptsOnlySupportedNalLengthSizes) {
  Manifest manifest;
  std::string xml = kManifest;
  xml.insert(xml.find("CodecPrivateData"), "NALUnitLengthField='2' ");
  ASSERT_TRUE(ParseManifest(xml, &manifest).ok());
  EXPECT_EQ(2u, manifest.streams[0].qualities[0].nal_length_size);
  xml.replace(xml.find("LengthField='2'"), 15, "LengthField='3'");
  EXPECT_FALSE(ParseManifest(xml, &manifest).ok());
}
TEST(IsmManifestTest, ResolvesHttpRelativeRootAndAbsoluteLocations) {
  Stream stream; Quality quality; Chunk chunk(123, 10); std::string result;
  quality.bitrate = 8000;
  const std::string base = "https://origin.example/a/movie.ism/Manifest?auth=manifest-only";
  for (const auto& entry : std::vector<std::pair<std::string, std::string>>{
      {"../QualityLevels({bitrate})/Fragments(video={start_time})?token=own:token#ignored",
       "https://origin.example/a/QualityLevels(8000)/Fragments(video=123)?token=own:token"},
      {"/media/{start time}", "https://origin.example/media/123"},
      {"//cdn.example/video/123", "https://cdn.example/video/123"},
      {"http://other.example/a%20b/123", "http://other.example/a%20b/123"}}) {
    stream.url = entry.first;
    ASSERT_TRUE(ResolveFragment(base, stream, quality, chunk, &result).ok());
    EXPECT_EQ(entry.second, result);
  }
}
TEST(IsmManifestTest, NormalizesLocalDotSegmentsWithoutRejectingDotFilenames) {
  Stream stream; Quality quality; Chunk chunk; std::string result;
  stream.url = "./cache/../video/part..bin";
  ASSERT_TRUE(ResolveFragment("C:\\media\\raw.ism", stream, quality, chunk, &result).ok());
  EXPECT_EQ("C:\\media\\video/part..bin", result);
  stream.url = "cache/../../escape";
  EXPECT_FALSE(ResolveFragment("raw.ism", stream, quality, chunk, &result).ok());
}
TEST(IsmManifestTest, RejectsNonHttpSchemesAndCredentialsInRemoteTemplates) {
  Stream stream; Quality quality; Chunk chunk; std::string result;
  for (const char* path : {"file:///private", "ftp://host/file", "http://user:pass@host/file",
                          "https:///missing-host", "{unknown}"}) {
    stream.url = path;
    EXPECT_FALSE(ResolveFragment("https://example.test/a/Manifest", stream, quality, chunk, &result).ok());
  }
}
TEST(IsmManifestTest, AcceptsBracedPlayReadySystemIdAndLegacyKidText) {
  Manifest manifest;
  ASSERT_TRUE(ParseManifest(Protected("<WRMHEADER><DATA><KID>MyIRAFVEd2aImaq7zN3u/w=="
      "</KID><PROTECTINFO><ALGID>AESCTR</ALGID></PROTECTINFO></DATA></WRMHEADER>"), &manifest).ok());
  ASSERT_EQ(16u, manifest.key_id.size());
  EXPECT_EQ(0u, manifest.key_id[0]); EXPECT_EQ(0x33u, manifest.key_id[3]);
}
TEST(IsmManifestTest, RejectsCbcMultipleKidsAndNonHeaderRecords) {
  Manifest manifest;
  for (const char* wrm : {"<WRMHEADER><KID ALGID='AESCBC' VALUE='MyIRAFVEd2aImaq7zN3u/w=='/></WRMHEADER>",
      "<WRMHEADER><KID VALUE='MyIRAFVEd2aImaq7zN3u/w=='/><KID VALUE='AAAAAAAAAAAAAAAAAAAAAA=='/></WRMHEADER>",
      "<Other><KID>MyIRAFVEd2aImaq7zN3u/w==</KID></Other>",
      "<WRMHEADER><KID>MyIRAFVEd2aImaq7zN3u/w===</KID></WRMHEADER>"})
    EXPECT_FALSE(ParseManifest(Protected(wrm), &manifest).ok());
}
TEST(IsmManifestTest, KeepsSparseTextIndexesWithoutRequiringTheirMedia) {
  std::string xml = kManifest;
  xml.insert(xml.find("<StreamIndex"), "<StreamIndex Type='text' Name='markers'/>");
  Manifest manifest; ASSERT_TRUE(ParseManifest(xml, &manifest).ok());
  ASSERT_EQ(2u, manifest.streams.size());
  EXPECT_EQ("text", manifest.streams[0].type);
  EXPECT_EQ("video", manifest.streams[1].type);
}
}  // namespace ism
}  // namespace shaka
