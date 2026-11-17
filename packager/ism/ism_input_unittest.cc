// Use of this source code is governed by the BSD-style license in LICENSE.
#include "packager/ism/ism_input.h"
#include "packager/media/base/buffer_writer.h"
#include "packager/media/formats/mp4/box_definitions.h"
#include "packager/media/formats/mp4/box_reader.h"
#include "packager/media/formats/mp4/track_run_iterator.h"
#include "packager/media/codecs/aac_audio_specific_config.h"
#include "packager/media/codecs/avc_decoder_configuration_record.h"
#include <gtest/gtest.h>
#include <memory>

namespace shaka {
namespace ism {
namespace {
using media::BufferWriter;
std::vector<uint8_t> Fragment() {
  BufferWriter w;
  w.AppendInt(uint32_t{76}); w.AppendInt(media::FOURCC_moof);
  w.AppendInt(uint32_t{16}); w.AppendInt(media::FOURCC_mfhd);
  w.AppendInt(uint32_t{0}); w.AppendInt(uint32_t{1});
  w.AppendInt(uint32_t{52}); w.AppendInt(media::FOURCC_traf);
  w.AppendInt(uint32_t{16}); w.AppendInt(media::FOURCC_tfhd);
  w.AppendInt(uint32_t{0x020000}); w.AppendInt(uint32_t{7});
  w.AppendInt(uint32_t{28}); w.AppendInt(media::FOURCC_trun);
  w.AppendInt(uint32_t{0x0301}); w.AppendInt(uint32_t{1});
  w.AppendInt(uint32_t{84}); w.AppendInt(uint32_t{3600}); w.AppendInt(uint32_t{1});
  w.AppendInt(uint32_t{9}); w.AppendInt(media::FOURCC_mdat);
  w.AppendInt(uint8_t{0x9a});
  std::vector<uint8_t> bytes;
  w.SwapBuffer(&bytes);
  return bytes;
}
uint32_t U32(const std::vector<uint8_t>& v, size_t p) {
  return (uint32_t(v[p]) << 24) | (uint32_t(v[p+1]) << 16) |
         (uint32_t(v[p+2]) << 8) | v[p+3];
}
bool ReadInitialization(const std::vector<uint8_t>& bytes, media::mp4::Movie* movie) {
  BufferWriter wrapper;
  wrapper.AppendInt(static_cast<uint32_t>(bytes.size() + 8));
  wrapper.AppendInt(media::FOURCC_skip); wrapper.AppendVector(bytes);
  bool error = false;
  std::unique_ptr<media::mp4::BoxReader> reader(
      media::mp4::BoxReader::ReadBox(wrapper.Buffer(), wrapper.Size(), &error));
  return reader && !error && reader->ScanChildren() && reader->ReadChild(movie);
}
Manifest Audio(const std::string& codec) {
  Manifest manifest; manifest.streams.resize(1);
  auto& stream = manifest.streams[0]; stream.type = "audio";
  Quality q; q.fourcc = codec; q.channels = 2; q.sampling_rate = 48000;
  stream.qualities.push_back(q); return manifest;
}
Manifest Video() {
  Manifest manifest; manifest.streams.resize(1);
  auto& stream = manifest.streams[0]; stream.type = "video";
  Quality q; q.fourcc = "H264"; q.width = 320; q.height = 240;
  q.codec_private_data = {0,0,0,1,0x27,0x4d,0x40,0x0d,0xa9,0x18,0x28,0x3e,
      0x60,0x0d,0x41,0x80,0x41,0xad,0xb0,0xad,0x7b,0xdf,0x01,0x01,
      0,0,0,1,0x28,0xde,0x09,0x88};
  stream.qualities.push_back(q); return manifest;
}
}
TEST(IsmInputTest, AddsTfdtWithoutMovingTrunAwayFromItsSample) {
  const auto source = Fragment();
  std::vector<uint8_t> rewritten;
  ASSERT_TRUE(RewriteFragment(3, uint64_t{1} << 40, source, &rewritten).ok());
  ASSERT_EQ(source.size() + 20, rewritten.size());
  EXPECT_EQ(96u, U32(rewritten, 0));
  EXPECT_EQ(72u, U32(rewritten, 24));
  EXPECT_EQ(3u, U32(rewritten, 44));
  const uint32_t data_offset = U32(rewritten, 64);
  EXPECT_EQ(104u, data_offset);
  EXPECT_EQ(0x9a, rewritten[data_offset]);
  EXPECT_EQ(static_cast<uint32_t>(media::FOURCC_tfdt), U32(rewritten, 80));
  EXPECT_EQ(256u, U32(rewritten, 88));
  EXPECT_EQ(0u, U32(rewritten, 92));
}
TEST(IsmInputTest, RejectsTruncatedHeadersAndIncompleteBaseOffsets) {
  auto source = Fragment(); std::vector<uint8_t> rewritten;
  source.pop_back();
  EXPECT_FALSE(RewriteFragment(1, 0, source, &rewritten).ok());
  source = Fragment(); source[43] |= 1;
  EXPECT_FALSE(RewriteFragment(1, 0, source, &rewritten).ok());
}
TEST(IsmInputTest, CreatesOnlyTheSelectedAudioTrack) {
  Manifest manifest;
  manifest.streams.resize(3);
  auto& stream = manifest.streams[2];
  stream.type = "audio";
  Quality q; q.fourcc = "AACL"; q.channels = 2; q.sampling_rate = 48000;
  q.codec_private_data = {0x11, 0x90}; stream.qualities.push_back(q);
  std::vector<uint8_t> init;
  ASSERT_TRUE(BuildInitialization(manifest, {2}, &init).ok());
  std::string bytes(init.begin(), init.end());
  EXPECT_NE(std::string::npos, bytes.find("mp4a"));
  EXPECT_EQ(std::string::npos, bytes.find("hvc1"));
  EXPECT_EQ(std::string::npos, bytes.find("avc1"));
}
TEST(IsmInputTest, BoundsZeroSizedMdatToItsOriginalChunk) {
  auto source = Fragment();
  std::fill(source.begin() + 76, source.begin() + 80, 0);
  std::vector<uint8_t> rewritten;
  ASSERT_TRUE(RewriteFragment(1, 0, source, &rewritten).ok());
  EXPECT_EQ(9u, U32(rewritten, 96));
  EXPECT_EQ(0x9a, rewritten.back());
}
TEST(IsmInputTest, BuildsMissingLcConfigurationWithTheDeclaredChannelLayout) {
  for (uint16_t channels : {2, 6, 8}) {
    Manifest manifest = Audio("aacL");
    manifest.streams[0].qualities[0].channels = channels;
    std::vector<uint8_t> init;
    ASSERT_TRUE(BuildInitialization(manifest, {0}, &init).ok());
    media::mp4::Movie movie;
    ASSERT_TRUE(ReadInitialization(init, &movie));
    ASSERT_EQ(1u, movie.tracks.size());
    const auto& entry = movie.tracks[0].media.information.sample_table.description.audio_entries[0];
    media::AACAudioSpecificConfig asc;
    ASSERT_TRUE(asc.Parse(entry.esds.es_descriptor.decoder_config_descriptor()
        .decoder_specific_info_descriptor().data()));
    EXPECT_EQ(48000u, asc.GetSamplesPerSecond()); EXPECT_EQ(channels, asc.GetNumChannels());
  }
}
TEST(IsmInputTest, DoesNotInventHeAacExtensionsOrAnUnknownChannelConfiguration) {
  std::vector<uint8_t> init;
  EXPECT_FALSE(BuildInitialization(Audio("AACH"), {0}, &init).ok());
  EXPECT_FALSE(BuildInitialization(Audio("AACP"), {0}, &init).ok());
  auto manifest = Audio("AACL"); manifest.streams[0].qualities[0].channels = 7;
  EXPECT_FALSE(BuildInitialization(manifest, {0}, &init).ok());
}
TEST(IsmInputTest, RejectsUnsupportedFallbackSamplingRatesBeforeWritingAnInit) {
  auto manifest = Audio("AAC"); manifest.streams[0].qualities[0].sampling_rate = 12345;
  std::vector<uint8_t> init;
  EXPECT_FALSE(BuildInitialization(manifest, {0}, &init).ok());
}
TEST(IsmInputTest, SupportsH264AliasesAndTheDeclaredNalLengthSize) {
  Manifest manifest = Video();
  auto& stream = manifest.streams[0];
  for (const char* codec : {"X264", "dAvC", "AvC1", "h264"}) {
    for (uint8_t length : {1, 2, 4}) {
      stream.qualities[0].fourcc = codec; stream.qualities[0].nal_length_size = length;
      std::vector<uint8_t> init; ASSERT_TRUE(BuildInitialization(manifest, {0}, &init).ok());
      media::mp4::Movie movie; ASSERT_TRUE(ReadInitialization(init, &movie));
      const auto& entry = movie.tracks[0].media.information.sample_table.description.video_entries[0];
      ASSERT_GT(entry.codec_configuration.data.size(), 4u);
      EXPECT_EQ(length - 1, entry.codec_configuration.data[4] & 3);
    }
  }
}
TEST(IsmInputTest, MapsLanguageTagsToTheMp4LanguageCode) {
  for (const auto& pair : std::vector<std::pair<std::string, std::string>>{
      {"es-419", "spa"}, {"EN-US", "eng"}, {"pt-BR", "por"}, {"deu", "deu"}}) {
    auto manifest = Audio("AACL"); manifest.streams[0].language = pair.first;
    std::vector<uint8_t> init; ASSERT_TRUE(BuildInitialization(manifest, {0}, &init).ok());
    media::mp4::Movie movie; ASSERT_TRUE(ReadInitialization(init, &movie));
    EXPECT_EQ(pair.second, movie.tracks[0].media.header.language.code);
  }
}
TEST(IsmInputTest, PreservesAnExplicitZeroClockWithReorderedVideoSamples) {
  std::vector<uint8_t> init;
  ASSERT_TRUE(BuildInitialization(Video(), {0}, &init).ok());
  media::mp4::Movie movie; ASSERT_TRUE(ReadInitialization(init, &movie));
  ASSERT_EQ(1u, movie.tracks[0].edit.list.edits.size());
  EXPECT_EQ(0, movie.tracks[0].edit.list.edits[0].media_time);
  media::mp4::MovieFragment fragment; fragment.tracks.resize(1);
  auto& traf = fragment.tracks[0]; traf.header.track_id = 1;
  traf.header.sample_description_index = 1; traf.runs.resize(1);
  auto& run = traf.runs[0]; run.sample_count = 2;
  run.sample_durations = {3600,3600}; run.sample_sizes = {1,1};
  run.sample_composition_time_offsets = {10800,0};
  media::mp4::TrackRunIterator iterator(&movie);
  ASSERT_TRUE(iterator.Init(fragment));
  EXPECT_EQ(0, iterator.dts()); EXPECT_EQ(10800, iterator.cts());
  EXPECT_EQ(3600, iterator.duration());
  iterator.AdvanceSample(); ASSERT_TRUE(iterator.IsSampleValid());
  EXPECT_EQ(3600, iterator.dts()); EXPECT_EQ(3600, iterator.cts());
  // Inputs with no edit retain the existing flag-controlled legacy heuristic.
  movie.tracks[0].edit.list.edits.clear();
  media::mp4::TrackRunIterator legacy(&movie); ASSERT_TRUE(legacy.Init(fragment));
  EXPECT_EQ(-10800, legacy.dts()); EXPECT_EQ(0, legacy.cts());
}
}  // namespace ism
}  // namespace shaka
