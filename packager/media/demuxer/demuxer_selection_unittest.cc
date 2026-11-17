// Use of this source code is governed by the BSD-style license in LICENSE.
#include "packager/media/demuxer/demuxer.h"
#include "packager/media/test/test_data_util.h"
#include <gtest/gtest.h>

namespace shaka {
namespace media {
namespace {
class SelectedStream : public MediaHandler {
 public:
  std::vector<StreamType> types;
  size_t samples = 0;
  Status InitializeInternal() override { return Status::OK; }
  Status Process(std::unique_ptr<StreamData> data) override {
    if (data->stream_data_type == StreamDataType::kStreamInfo)
      types.push_back(data->stream_info->stream_type());
    if (data->stream_data_type == StreamDataType::kMediaSample) ++samples;
    return Status::OK;
  }
  Status OnFlushRequest(size_t) override { return Status::OK; }
};
}  // namespace
TEST(DemuxerSelectionTest, VideoSelectorRoutesOnlyOneVideoTrack) {
  Demuxer demuxer(GetTestDataFilePath("bear-640x360.ts").AsUTF8Unsafe());
  auto video = std::make_shared<SelectedStream>();
  ASSERT_TRUE(demuxer.SetHandler("video", video).ok());
  ASSERT_TRUE(demuxer.Initialize().ok());
  ASSERT_TRUE(demuxer.Run().ok());
  ASSERT_EQ(1u, video->types.size());
  EXPECT_EQ(kStreamVideo, video->types[0]);
  EXPECT_GT(video->samples, 0u);
}
TEST(DemuxerSelectionTest, ExplicitVideoAndAudioHaveSeparateOutputs) {
  Demuxer demuxer(GetTestDataFilePath("bear-640x360.ts").AsUTF8Unsafe());
  auto video = std::make_shared<SelectedStream>();
  auto audio = std::make_shared<SelectedStream>();
  ASSERT_TRUE(demuxer.SetHandler("video", video).ok());
  ASSERT_TRUE(demuxer.SetHandler("audio", audio).ok());
  ASSERT_TRUE(demuxer.Initialize().ok());
  ASSERT_TRUE(demuxer.Run().ok());
  ASSERT_EQ(1u, video->types.size());
  ASSERT_EQ(1u, audio->types.size());
  EXPECT_EQ(kStreamVideo, video->types[0]);
  EXPECT_EQ(kStreamAudio, audio->types[0]);
  EXPECT_GT(video->samples, 0u);
  EXPECT_GT(audio->samples, 0u);
}
}  // namespace media
}  // namespace shaka
