// Use of this source code is governed by the BSD-style license in LICENSE.
#include "packager/ism/ism_input.h"
#include <algorithm>
#include <cctype>
#include <memory>
#include <set>
#include "packager/file/file.h"
#include "packager/file/file_closer.h"
#include "packager/file/file_util.h"
#include "packager/media/base/buffer_writer.h"
#include "packager/media/base/bit_writer.h"
#include "packager/media/base/language_utils.h"
#include "packager/media/codecs/h264_byte_to_unit_stream_converter.h"
#include "packager/media/codecs/h265_byte_to_unit_stream_converter.h"
#include "packager/media/formats/mp4/box_definitions.h"
#include "packager/status_macros.h"

namespace shaka {
namespace ism {
namespace {
using namespace media;
using namespace media::mp4;
Status Invalid(const std::string& message) {
  return Status(error::PARSER_FAILURE, "ISM: " + message);
}
const Quality& Best(const Stream& stream) {
  return *std::max_element(stream.qualities.begin(), stream.qualities.end(),
      [](const Quality& a, const Quality& b) { return a.bitrate < b.bitrate; });
}
bool Earlier(uint64_t a, uint32_t scale_a, uint64_t b, uint32_t scale_b) {
  if (a / scale_a != b / scale_b) return a / scale_a < b / scale_b;
  // Each remainder is at most UINT32_MAX - 1, so the cross products fit.
  return (a % scale_a) * scale_b < (b % scale_b) * scale_a;
}
void Protect(FourCC format, const std::vector<uint8_t>& kid,
             ProtectionSchemeInfo* sinf) {
  sinf->format.format = format;
  sinf->type.type = FOURCC_cenc;
  sinf->type.version = 0x00010000;
  sinf->info.track_encryption.default_is_protected = 1;
  sinf->info.track_encryption.default_per_sample_iv_size = 8;
  sinf->info.track_encryption.default_kid = kid;
}
Status Configure(const Stream& stream, const Quality& q,
                 const std::vector<uint8_t>& kid, Track* track) {
  const bool encrypted = !kid.empty();
  std::string codec = q.fourcc;
  std::transform(codec.begin(), codec.end(), codec.begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  auto& description = track->media.information.sample_table.description;
  if (stream.type == "video") {
    if (!q.width || !q.height || q.width > UINT16_MAX || q.height > UINT16_MAX)
      return Invalid("invalid video dimensions");
    track->header.width = q.width << 16;
    track->header.height = q.height << 16;
    track->media.handler.handler_type = FOURCC_vide;
    description.type = kVideo;
    description.video_entries.resize(1);
    auto& entry = description.video_entries[0];
    entry.width = q.width;
    entry.height = q.height;
    entry.pixel_aspect.h_spacing = entry.pixel_aspect.v_spacing = 1;
    FourCC format;
    std::unique_ptr<H26xByteToUnitStreamConverter> converter;
    if (codec == "h264" || codec == "avc1" || codec == "x264" || codec == "davc") {
      format = FOURCC_avc1;
      converter.reset(new H264ByteToUnitStreamConverter(
          H26xStreamFormat::kNalUnitStreamWithoutParameterSetNalus));
    } else if (codec == "hvc1" || codec == "hev1" ||
               codec == "dvhe" || codec == "dvh1") {
      format = codec == "dvhe" ? FOURCC_dvhe :
               codec == "dvh1" ? FOURCC_dvh1 :
               codec == "hev1" ? FOURCC_hev1 : FOURCC_hvc1;
      converter.reset(new H265ByteToUnitStreamConverter(
          format == FOURCC_hev1 || format == FOURCC_dvhe
              ? H26xStreamFormat::kNalUnitStreamWithParameterSetNalus
              : H26xStreamFormat::kNalUnitStreamWithoutParameterSetNalus));
    } else {
      return Invalid("unsupported video FourCC " + q.fourcc);
    }
    std::vector<uint8_t> ignored;
    if (q.codec_private_data.empty() ||
        !converter->ConvertByteStreamToNalUnitStream(q.codec_private_data.data(),
            q.codec_private_data.size(), &ignored) ||
        !converter->GetDecoderConfigurationRecord(&entry.codec_configuration.data))
      return Invalid("invalid video CodecPrivateData");
    if (q.nal_length_size != 1 && q.nal_length_size != 2 && q.nal_length_size != 4)
      return Invalid("invalid NALUnitLengthField");
    // The parameter sets are Annex B, but media samples use the declared
    // length prefix. Changing the record must not rewrite those samples.
    const size_t length_at = format == FOURCC_avc1 ? 4 : 21;
    auto& config = entry.codec_configuration.data;
    if (config.size() <= length_at) return Invalid("short video configuration");
    config[length_at] = (config[length_at] & 0xfc) | (q.nal_length_size - 1);
    entry.format = encrypted ? FOURCC_encv : format;
    if (encrypted) Protect(format, kid, &entry.sinf);
  } else if (stream.type == "audio") {
    if (!q.sampling_rate || !q.channels || q.sampling_rate > 65535)
      return Invalid("invalid audio configuration");
    track->media.handler.handler_type = FOURCC_soun;
    description.type = kAudio;
    description.audio_entries.resize(1);
    auto& entry = description.audio_entries[0];
    entry.channelcount = q.channels;
    entry.samplesize = q.bits_per_sample;
    entry.samplerate = q.sampling_rate;
    FourCC format;
    if (codec == "aacl" || codec == "aach" || codec == "aac" || codec == "aacp") {
      std::vector<uint8_t> asc = q.codec_private_data;
      if (asc.empty()) {
        // A bare AAC-LC declaration is sufficient to build an ASC. HE-AAC
        // needs its signaled extension; do not guess SBR/PS from channel count.
        if ((codec != "aac" && codec != "aacl") ||
            (q.channels > 6 && q.channels != 8))
          return Invalid("missing AAC configuration");
        const uint32_t rates[] = {96000,88200,64000,48000,44100,32000,24000,
                                  22050,16000,12000,11025,8000,7350};
        const auto rate = std::find(rates, rates + 13, q.sampling_rate);
        if (rate == rates + 13)
          return Invalid("AAC-LC fallback needs a standard indexed sampling rate");
        BitWriter bits(&asc);
        bits.WriteBits(2, 5);  // AAC-LC.
        bits.WriteBits(rate - rates, 4);
        bits.WriteBits(q.channels == 8 ? 7 : q.channels, 4);
        bits.WriteBits(0, 3);  // GASpecificConfig.
        bits.Flush();
      }
      format = FOURCC_mp4a;
      auto* config = entry.esds.es_descriptor.mutable_decoder_config_descriptor();
      config->set_object_type(ObjectType::kISO_14496_3);
      config->set_max_bitrate(q.bitrate);
      config->set_avg_bitrate(q.bitrate);
      config->mutable_decoder_specific_info_descriptor()->set_data(asc);
    } else if (codec == "ec-3") {
      format = FOURCC_ec_3;
      const auto& data = q.codec_private_data;
      const uint8_t guid[] = {0xaf,0x87,0xfb,0xa7,0x02,0x2d,0xfb,0x42,
                             0xa4,0xd4,0x05,0xcd,0x93,0x84,0x3b,0xdd};
      if (data.size() < 27 || !std::equal(guid, guid + 16, data.begin() + 6))
        return Invalid("unsupported E-AC-3 CodecPrivateData envelope");
      entry.dec3.data.assign(data.begin() + 22, data.end());
    } else {
      return Invalid("unsupported audio FourCC " + q.fourcc);
    }
    entry.format = encrypted ? FOURCC_enca : format;
    if (encrypted) Protect(format, kid, &entry.sinf);
  } else {
    return Invalid("only video and audio ISM streams are supported");
  }
  return Status::OK;
}
Status ReadBounded(const std::string& path, size_t maximum, std::vector<uint8_t>* bytes) {
  std::unique_ptr<File, FileCloser> file(File::Open(path.c_str(), "r"));
  if (!file) return Status(error::FILE_FAILURE, "ISM: cannot open " + path);
  bytes->clear();
  uint8_t buffer[65536];
  for (;;) {
    const int64_t size = file->Read(buffer, sizeof(buffer));
    if (size < 0) return Status(error::FILE_FAILURE, "ISM: cannot read " + path);
    if (!size) return Status::OK;
    if (static_cast<uint64_t>(size) > maximum - bytes->size())
      return Invalid("input exceeds the size limit: " + path);
    bytes->insert(bytes->end(), buffer, buffer + size);
  }
}
}  // namespace

Status BuildInitialization(const Manifest& manifest,
                           const std::vector<size_t>& selected,
                           std::vector<uint8_t>* bytes) {
  if (!bytes || selected.empty() || !manifest.timescale)
    return Invalid("no selected stream");
  Movie movie;
  movie.header.timescale = manifest.timescale;
  movie.header.next_track_id = static_cast<uint32_t>(manifest.streams.size() + 1);
  for (size_t index : selected) {
    if (index >= manifest.streams.size()) return Invalid("stream index out of range");
    const auto& stream = manifest.streams[index];
    if (stream.qualities.empty()) return Invalid("stream has no quality");
    Track track;
    track.header.track_id = static_cast<uint32_t>(index + 1);
    track.media.header.timescale = stream.timescale;
    // This importer has an explicit clock from the manifest/tfdt. Signal an
    // identity edit so the MP4 reader does not guess a missing edit from the
    // first B-frame's composition offset and shift this track alone.
    EditListEntry clock;
    clock.media_rate_integer = 1;
    track.edit.list.edits.push_back(clock);
    if (!stream.language.empty()) {
      auto language = stream.language.substr(0, stream.language.find('-'));
      std::transform(language.begin(), language.end(), language.begin(),
          [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      language = LanguageToISO_639_2(language);
      track.media.header.language.code = language;
    }
    RETURN_IF_ERROR(Configure(stream, Best(stream), manifest.key_id, &track));
    movie.tracks.push_back(std::move(track));
    TrackExtends extends;
    extends.track_id = static_cast<uint32_t>(index + 1);
    extends.default_sample_description_index = 1;
    movie.extends.tracks.push_back(extends);
  }
  FileType ftyp;
  ftyp.major_brand = static_cast<FourCC>(0x69736d6c);  // isml
  ftyp.compatible_brands = {FOURCC_iso8, FOURCC_isom};
  BufferWriter writer;
  ftyp.Write(&writer);
  movie.Write(&writer);
  writer.SwapBuffer(bytes);
  return Status::OK;
}

Status PrepareInput(const std::string& path,
                    const std::vector<std::string>& selectors,
                    const std::function<bool()>& cancelled,
                    const std::string& temp_dir,
                    PreparedInput* input) {
  if (!input) return Invalid("missing input result");
  *input = PreparedInput();
  std::vector<uint8_t> data;
  RETURN_IF_ERROR(ReadBounded(path,64 * 1024 * 1024,&data));
  Manifest manifest;
  RETURN_IF_ERROR(ParseManifest(std::string(data.begin(),data.end()),&manifest));
  std::set<size_t> selected;
  for (const auto& selector : selectors) {
    size_t index = manifest.streams.size();
    if (selector == "video" || selector == "audio") {
      for (size_t n = 0; n < manifest.streams.size(); ++n) {
        if (manifest.streams[n].type == selector) { index = n; break; }
      }
    } else {
      uint64_t number = 0;
      if (selector.empty()) return Invalid("empty stream selector");
      for (char c : selector) {
        if (c < '0' || c > '9' || number > manifest.streams.size())
          return Invalid("unsupported stream selector " + selector);
        number = number * 10 + c - '0';
      }
      if (number < manifest.streams.size()) index = static_cast<size_t>(number);
    }
    if (index >= manifest.streams.size()) return Invalid("selected stream unavailable");
    selected.insert(index);
  }
  if (selected.empty()) return Invalid("select video, audio or a manifest stream index");
  input->source_stream_indexes.assign(selected.begin(),selected.end());
  RETURN_IF_ERROR(BuildInitialization(manifest,input->source_stream_indexes,&data));
  std::string temp;
  if (!TempFilePath(temp_dir,&temp)) return Status(error::FILE_FAILURE,"ISM: no temporary file");
  struct Cleanup { std::string path; ~Cleanup() { if (!path.empty()) File::Delete(path.c_str()); } } cleanup{temp};
  std::unique_ptr<File,FileCloser> file(File::Open(temp.c_str(),"w"));
  if (!file) return Status(error::FILE_FAILURE,"ISM: cannot write temporary input");
  if (file->Write(data.data(),data.size()) != static_cast<int64_t>(data.size()))
    return Status(error::FILE_FAILURE,"ISM: cannot write initialization");
  std::vector<size_t> next(manifest.streams.size(),0);
  for (;;) {
    size_t index = manifest.streams.size();
    for (size_t candidate : selected) {
      const auto& stream = manifest.streams[candidate];
      if (next[candidate] == stream.chunks.size()) continue;
      if (index == manifest.streams.size() || Earlier(
          stream.chunks[next[candidate]].start, stream.timescale,
          manifest.streams[index].chunks[next[index]].start,
          manifest.streams[index].timescale)) index = candidate;
    }
    if (index == manifest.streams.size()) break;
    if (cancelled && cancelled()) return Status(error::CANCELLED,"ISM preparation cancelled");
    const auto& stream = manifest.streams[index];
    const auto& chunk = stream.chunks[next[index]++];
    std::string fragment_path;
    RETURN_IF_ERROR(ResolveFragment(path,stream,Best(stream),chunk,&fragment_path));
    RETURN_IF_ERROR(ReadBounded(fragment_path,512 * 1024 * 1024,&data));
    std::vector<uint8_t> normalized;
    const auto status = RewriteFragment(static_cast<uint32_t>(index + 1),chunk.start,data,&normalized);
    if (!status.ok()) return Status(status.error_code(),
        "ISM StreamIndex " + std::to_string(index) + " chunk t=" +
        std::to_string(chunk.start) + ": " + status.error_message());
    if (file->Write(normalized.data(),normalized.size()) != static_cast<int64_t>(normalized.size()))
      return Status(error::FILE_FAILURE,"ISM: cannot write fragment");
  }
  if (!file.release()->Close()) return Status(error::FILE_FAILURE,"ISM: cannot close input");
  input->path = temp;
  cleanup.path.clear();
  return Status::OK;
}
}  // namespace ism
}  // namespace shaka
