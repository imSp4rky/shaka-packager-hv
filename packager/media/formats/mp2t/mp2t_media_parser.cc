// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "packager/media/formats/mp2t/mp2t_media_parser.h"

#include <algorithm>
#include <memory>
#include <limits>

#include "packager/base/bind.h"
#include "packager/media/base/media_sample.h"
#include "packager/media/base/stream_info.h"
#include "packager/media/base/text_sample.h"
#include "packager/media/base/bit_reader.h"
#include "packager/media/base/video_stream_info.h"
#include "packager/media/codecs/dovi_decoder_configuration_record.h"
#include "packager/media/codecs/nalu_reader.h"
#include "packager/media/formats/mp2t/bbts_decryptor.h"
#include "packager/media/formats/mp2t/es_parser.h"
#include "packager/media/formats/mp2t/es_parser_audio.h"
#include "packager/media/formats/mp2t/es_parser_dvb.h"
#include "packager/media/formats/mp2t/es_parser_h264.h"
#include "packager/media/formats/mp2t/es_parser_h265.h"
#include "packager/media/formats/mp2t/mp2t_common.h"
#include "packager/media/formats/mp2t/ts_packet.h"
#include "packager/media/formats/mp2t/ts_section.h"
#include "packager/media/formats/mp2t/ts_section_pat.h"
#include "packager/media/formats/mp2t/ts_section_pes.h"
#include "packager/media/formats/mp2t/ts_section_pmt.h"
#include "packager/media/formats/mp2t/ts_section_psi.h"
#include "packager/media/formats/mp2t/ts_stream_type.h"

namespace shaka {
namespace media {
namespace mp2t {

namespace {
class BbtsSdtSection : public TsSectionPsi {
 public:
  explicit BbtsSdtSection(BbtsDecryptor* bbts)
      : TsSectionPsi(4093), bbts_(bbts) {}
  bool ParsePsiSection(BitReader* reader) override {
    return bbts_->ParseSdt(reader->current_byte_ptr(), reader->bits_available() / 8);
  }
  void ResetPsiSection() override {}
 private:
  BbtsDecryptor* bbts_;
};
}  // namespace

class PidState {
 public:
  enum PidType {
    kPidPat,
    kPidPmt,
    kPidSdt,
    kPidAudioPes,
    kPidVideoPes,
    kPidTextPes,
  };

  PidState(int pid,
           PidType pid_type,
           std::unique_ptr<TsSection> section_parser);

  // Extract the content of the TS packet and parse it.
  // Return true if successful.
  bool PushTsPacket(const TsPacket& ts_packet);

  // Flush the PID state (possibly emitting some pending frames)
  // and reset its state.
  bool Flush();

  // Enable/disable the PID.
  // Disabling a PID will reset its state and ignore any further incoming TS
  // packets.
  void Enable();
  void Disable();
  bool IsEnabled() const;

  PidType pid_type() const { return pid_type_; }

  std::shared_ptr<StreamInfo>& config() { return config_; }
  void set_config(const std::shared_ptr<StreamInfo>& config) {
    config_ = config;
  }

 private:
  friend Mp2tMediaParser;
  void ResetState();

  int pid_;
  PidType pid_type_;
  std::unique_ptr<TsSection> section_parser_;

  std::deque<std::shared_ptr<MediaSample>> media_sample_queue_;
  std::deque<std::shared_ptr<TextSample>> text_sample_queue_;

  bool enable_;
  int continuity_counter_;
  std::shared_ptr<StreamInfo> config_;
};

PidState::PidState(int pid,
                   PidType pid_type,
                   std::unique_ptr<TsSection> section_parser)
    : pid_(pid),
      pid_type_(pid_type),
      section_parser_(std::move(section_parser)),
      enable_(false),
      continuity_counter_(-1) {
  DCHECK(section_parser_);
}

bool PidState::PushTsPacket(const TsPacket& ts_packet) {
  DCHECK_EQ(ts_packet.pid(), pid_);

  // The current PID is not part of the PID filter,
  // just discard the incoming TS packet.
  if (!enable_)
    return true;

  int expected_continuity_counter = (continuity_counter_ + 1) % 16;
  if (continuity_counter_ >= 0 &&
      ts_packet.continuity_counter() != expected_continuity_counter) {
    LOG(ERROR) << "TS discontinuity detected for pid: " << pid_;
    // TODO(tinskip): Handle discontinuity better.
    return false;
  }

  bool status = section_parser_->Parse(
      ts_packet.payload_unit_start_indicator(),
      ts_packet.payload(),
      ts_packet.payload_size());

  // At the minimum, when parsing failed, auto reset the section parser.
  // Components that use the Mp2tMediaParser can take further action if needed.
  if (!status) {
    LOG(ERROR) << "Parsing failed for pid = " << pid_ << ", type=" << pid_type_;
    ResetState();
  }

  return status;
}

bool PidState::Flush() {
  RCHECK(section_parser_->Flush());
  ResetState();
  return true;
}

void PidState::Enable() {
  enable_ = true;
}

void PidState::Disable() {
  if (!enable_)
    return;

  ResetState();
  enable_ = false;
}

bool PidState::IsEnabled() const {
  return enable_;
}

void PidState::ResetState() {
  section_parser_->Reset();
  continuity_counter_ = -1;
}

Mp2tMediaParser::Mp2tMediaParser()
    : sbr_in_mimetype_(false),
      is_initialized_(false) {
}

Mp2tMediaParser::~Mp2tMediaParser() {}

void Mp2tMediaParser::Init(const InitCB& init_cb,
                           const NewMediaSampleCB& new_media_sample_cb,
                           const NewTextSampleCB& new_text_sample_cb,
                           KeySource* decryption_key_source) {
  DCHECK(!is_initialized_);
  DCHECK(init_cb_.is_null());
  DCHECK(!init_cb.is_null());
  DCHECK(!new_media_sample_cb.is_null());
  DCHECK(!new_text_sample_cb.is_null());

  init_cb_ = init_cb;
  new_media_sample_cb_ = new_media_sample_cb;
  new_text_sample_cb_ = new_text_sample_cb;
  bbts_.reset(new BbtsDecryptor(decryption_key_source));
}

bool Mp2tMediaParser::Flush() {
  DVLOG(1) << "Mp2tMediaParser::Flush";
  const uint8_t* pending_ts;
  int pending_size;
  ts_byte_queue_.Peek(&pending_ts, &pending_size);
  if (bbts_->active() && pending_size != 0) {
    LOG(ERROR) << "Incomplete BBTS transport packet at end of input.";
    return false;
  }

  // Flush the buffers and reset the pids.
  for (const auto& pair : pids_) {
    DVLOG(1) << "Flushing PID: " << pair.first;
    PidState* pid_state = pair.second.get();
    RCHECK(pid_state->Flush());
  }
  bool result = EmitRemainingSamples();
  if (bbts_->active() && (!is_initialized_ || !bbts_origin_set_)) {
    LOG(ERROR) << "BBTS input ended before complete media streams were available.";
    result = false;
  }
  pids_.clear();

  // Remove any bytes left in the TS buffer.
  // (i.e. any partial TS packet => less than 188 bytes).
  ts_byte_queue_.Reset();
  return result;
}

bool Mp2tMediaParser::Parse(const uint8_t* buf, int size) {
  DVLOG(2) << "Mp2tMediaParser::Parse size=" << size;

  // Add the data to the parser state.
  ts_byte_queue_.Push(buf, size);

  while (true) {
    const uint8_t* ts_buffer;
    int ts_buffer_size;
    ts_byte_queue_.Peek(&ts_buffer, &ts_buffer_size);
    if (ts_buffer_size < TsPacket::kPacketSize)
      break;

    // Synchronization.
    int skipped_bytes = TsPacket::Sync(ts_buffer, ts_buffer_size);
    if (skipped_bytes > 0) {
      DVLOG(1) << "Packet not aligned on a TS syncword:"
               << " skipped_bytes=" << skipped_bytes;
      ts_byte_queue_.Pop(skipped_bytes);
      continue;
    }

    // Parse the TS header, skipping 1 byte if the header is invalid.
    std::unique_ptr<TsPacket> ts_packet(
        TsPacket::Parse(ts_buffer, ts_buffer_size));
    if (!ts_packet) {
      DVLOG(1) << "Error: invalid TS packet";
      ts_byte_queue_.Pop(1);
      continue;
    }
    DVLOG(LOG_LEVEL_TS)
        << "Processing PID=" << ts_packet->pid()
        << " start_unit=" << ts_packet->payload_unit_start_indicator();

    // Parse the section.
    auto it = pids_.find(ts_packet->pid());
    if (it == pids_.end() && ts_packet->pid() == 17) {
      std::unique_ptr<TsSection> section(new BbtsSdtSection(bbts_.get()));
      std::unique_ptr<PidState> state(new PidState(17, PidState::kPidSdt,
                                                   std::move(section)));
      state->Enable();
      it = pids_.emplace(17, std::move(state)).first;
    }
    if (it == pids_.end() &&
        ts_packet->pid() == TsSection::kPidPat) {
      // Create the PAT state here if needed.
      std::unique_ptr<TsSection> pat_section_parser(new TsSectionPat(
          base::Bind(&Mp2tMediaParser::RegisterPmt, base::Unretained(this))));
      std::unique_ptr<PidState> pat_pid_state(new PidState(
          ts_packet->pid(), PidState::kPidPat, std::move(pat_section_parser)));
      pat_pid_state->Enable();
      it = pids_.emplace(ts_packet->pid(), std::move(pat_pid_state)).first;
    }

    if (it != pids_.end()) {
      // Lock the clock when the first clear media PES starts, even if its
      // samples are still buffered. A later BBTS SDT must not rebase them.
      if (!bbts_origin_set_ && !bbts_->active() &&
          ts_packet->payload_unit_start_indicator() &&
          (it->second->pid_type() == PidState::kPidAudioPes ||
           it->second->pid_type() == PidState::kPidVideoPes)) {
        bbts_origin_ = 0;
        bbts_origin_set_ = true;
      }
      RCHECK(it->second->PushTsPacket(*ts_packet));
    } else {
      DVLOG(LOG_LEVEL_TS) << "Ignoring TS packet for pid: " << ts_packet->pid();
    }

    // Go to the next packet.
    ts_byte_queue_.Pop(TsPacket::kPacketSize);
  }

  // Emit the A/V buffers that kept accumulating during TS parsing.
  return EmitRemainingSamples();
}

void Mp2tMediaParser::RegisterPmt(int program_number, int pmt_pid) {
  DVLOG(1) << "RegisterPmt:"
           << " program_number=" << program_number
           << " pmt_pid=" << pmt_pid;

  // Only one TS program is allowed. Ignore the incoming program map table,
  // if there is already one registered.
  for (const auto& pair : pids_) {
    if (pair.second->pid_type() == PidState::kPidPmt) {
      DVLOG_IF(1, pmt_pid != pair.first) << "More than one program is defined";
      return;
    }
  }

  // Create the PMT state here if needed.
  DVLOG(1) << "Create a new PMT parser";
  std::unique_ptr<TsSection> pmt_section_parser(new TsSectionPmt(base::Bind(
      &Mp2tMediaParser::RegisterPes, base::Unretained(this), pmt_pid)));
  std::unique_ptr<PidState> pmt_pid_state(
      new PidState(pmt_pid, PidState::kPidPmt, std::move(pmt_section_parser)));
  pmt_pid_state->Enable();
  pids_.emplace(pmt_pid, std::move(pmt_pid_state));
}

void Mp2tMediaParser::RegisterPes(int pmt_pid,
                                  int pes_pid,
                                  TsStreamType stream_type,
                                  const uint8_t* descriptor,
                                  size_t descriptor_length) {
  if (pids_.count(pes_pid) != 0)
    return;
  bool dovi_registration = false;
  std::vector<uint8_t> dovi_descriptor;
  for (size_t pos = 0; pos < descriptor_length;) {
    if (descriptor_length - pos < 2 ||
        descriptor[pos + 1] > descriptor_length - pos - 2) {
      parse_error_ = true;
      return;
    }
    const uint8_t tag = descriptor[pos];
    const size_t length = descriptor[pos + 1];
    if (tag == 5 && length == 4 &&
        descriptor[pos + 2] == 'D' && descriptor[pos + 3] == 'O' &&
        descriptor[pos + 4] == 'V' && descriptor[pos + 5] == 'I')
      dovi_registration = true;
    if (tag == 0xb0 && length >= 4 && length <= 24) {
      dovi_descriptor.assign(descriptor + pos + 2, descriptor + pos + 2 + length);
    }
    pos += 2 + length;
  }
  if (stream_type == TsStreamType::kPesPrivateData && dovi_registration)
    stream_type = TsStreamType::kHevc;
  if (stream_type == TsStreamType::kHevc && dovi_registration &&
      !dovi_descriptor.empty())
    dovi_descriptors_[pes_pid] = std::move(dovi_descriptor);
  DVLOG(1) << "RegisterPes:"
           << " pes_pid=" << pes_pid << " stream_type=" << std::hex
           << static_cast<int>(stream_type) << std::dec;

  // Create a stream parser corresponding to the stream type.
  PidState::PidType pid_type = PidState::kPidVideoPes;
  std::unique_ptr<EsParser> es_parser;
  auto on_new_stream = base::Bind(&Mp2tMediaParser::OnNewStreamInfo,
                                  base::Unretained(this), pes_pid);
  auto on_emit_media = base::Bind(&Mp2tMediaParser::OnEmitMediaSample,
                                  base::Unretained(this), pes_pid);
  auto on_emit_text = base::Bind(&Mp2tMediaParser::OnEmitTextSample,
                                 base::Unretained(this), pes_pid);
  switch (stream_type) {
    case TsStreamType::kAvc:
      es_parser.reset(new EsParserH264(pes_pid, on_new_stream, on_emit_media));
      break;
    case TsStreamType::kHevc:
      es_parser.reset(new EsParserH265(pes_pid, on_new_stream, on_emit_media));
      break;
    case TsStreamType::kAdtsAac:
    case TsStreamType::kMpeg1Audio:
    case TsStreamType::kAc3:
      es_parser.reset(
          new EsParserAudio(pes_pid, static_cast<TsStreamType>(stream_type),
                            on_new_stream, on_emit_media, sbr_in_mimetype_));
      pid_type = PidState::kPidAudioPes;
      break;
    case TsStreamType::kDvbSubtitles:
      es_parser.reset(new EsParserDvb(pes_pid, on_new_stream, on_emit_text,
                                      descriptor, descriptor_length));
      pid_type = PidState::kPidTextPes;
      break;
    default: {
      auto type = static_cast<int>(stream_type);
      DCHECK(type <= 0xff);
      LOG_IF(ERROR, !stream_type_logged_once_[type])
          << "Ignore unsupported MPEG2TS stream type 0x" << std::hex << type
          << std::dec;
      stream_type_logged_once_[type] = true;
      return;
    }
  }

  // Create the PES state here.
  DVLOG(1) << "Create a new PES state";
  std::unique_ptr<TsSection> pes_section_parser(
      new TsSectionPes(std::move(es_parser),
                       pid_type == PidState::kPidVideoPes ? bbts_.get() : nullptr,
                       stream_type == TsStreamType::kAvc ? 1 : 2));
  std::unique_ptr<PidState> pes_pid_state(
      new PidState(pes_pid, pid_type, std::move(pes_section_parser)));
  pes_pid_state->Enable();
  pids_.emplace(pes_pid, std::move(pes_pid_state));
}

void Mp2tMediaParser::OnNewStreamInfo(
    uint32_t pes_pid,
    std::shared_ptr<StreamInfo> new_stream_info) {
  DCHECK(!new_stream_info || new_stream_info->track_id() == pes_pid);
  DVLOG(1) << "OnVideoConfigChanged for pid=" << pes_pid
           << ", has_info=" << (new_stream_info ? "true" : "false");

  auto pid_state = pids_.find(pes_pid);
  if (pid_state == pids_.end()) {
    LOG(ERROR) << "PID State for new stream not found (pid = "
               << new_stream_info->track_id() << ").";
    return;
  }

  if (new_stream_info) {
    // Set the stream configuration information for the PID.
    pid_state->second->set_config(new_stream_info);
    if (!is_initialized_ && new_stream_info->codec() == kCodecH265)
      pending_hevc_.insert(pes_pid);
  } else {
    LOG(WARNING) << "Ignoring unsupported stream with pid=" << pes_pid;
    pid_state->second->Disable();
  }

  // Finish initialization if all streams have configs.
  FinishInitializationIfNeeded();
}

bool Mp2tMediaParser::FinishInitializationIfNeeded() {
  // Nothing to be done if already initialized.
  if (is_initialized_)
    return true;
  if (!pending_hevc_.empty())
    return true;

  // Wait for more data to come to finish initialization.
  if (pids_.empty())
    return true;

  std::vector<std::shared_ptr<StreamInfo>> all_stream_info;
  uint32_t num_es(0);
  for (const auto& pair : pids_) {
    if ((pair.second->pid_type() == PidState::kPidAudioPes ||
         pair.second->pid_type() == PidState::kPidVideoPes ||
         pair.second->pid_type() == PidState::kPidTextPes) &&
        pair.second->IsEnabled()) {
      ++num_es;
      if (pair.second->config())
        all_stream_info.push_back(pair.second->config());
    }
  }
  if (num_es && (all_stream_info.size() == num_es)) {
    // All stream configurations have been received. Initialization can
    // be completed.
    init_cb_.Run(all_stream_info);
    DVLOG(1) << "Mpeg2TS stream parser initialization done";
    is_initialized_ = true;
  }
  return true;
}

void Mp2tMediaParser::OnEmitMediaSample(
    uint32_t pes_pid,
    std::shared_ptr<MediaSample> new_sample) {
  DCHECK(new_sample);
  DVLOG(LOG_LEVEL_ES) << "OnEmitMediaSample: "
                      << " pid=" << pes_pid
                      << " size=" << new_sample->data_size()
                      << " dts=" << new_sample->dts()
                      << " pts=" << new_sample->pts();

  // Add the sample to the appropriate PID sample queue.
  auto pid_state = pids_.find(pes_pid);
  if (pid_state == pids_.end()) {
    LOG(ERROR) << "PID State for new sample not found (pid = " << pes_pid
               << ").";
    return;
  }
  if (pending_hevc_.count(pes_pid)) {
    if (!RecoverDolbyVision(pes_pid, *new_sample)) {
      parse_error_ = true;
      return;
    }
    pending_hevc_.erase(pes_pid);
    FinishInitializationIfNeeded();
  }
  pid_state->second->media_sample_queue_.push_back(std::move(new_sample));
}

bool Mp2tMediaParser::RecoverDolbyVision(uint32_t pid, const MediaSample& sample) {
  auto* video = static_cast<VideoStreamInfo*>(pids_.at(pid)->config().get());
  NaluReader reader(Nalu::kH265, video->nalu_length_size(), sample.data(),
                    sample.data_size());
  Nalu nalu;
  NaluReader::Result result;
  bool rpu = false;
  while ((result = reader.Advance(&nalu)) == NaluReader::kOk) {
    if (nalu.type() != 62) continue;
    if (!DOVIDecoderConfigurationRecord::IsProfile5Rpu(
            nalu.data(), nalu.header_size() + nalu.payload_size())) {
      // Ordinary clear TS previously passed these NALs through as HEVC.
      // Do not tighten that path merely because BBTS support is enabled.
      if (!bbts_->active()) return true;
      LOG(ERROR) << "Unsupported or invalid Dolby Vision RPU in MPEG-TS.";
      return false;
    }
    rpu = true;
  }
  RCHECK(result == NaluReader::kEOStream);
  if (!rpu) {
    if (bbts_->active() && dovi_descriptors_.count(pid)) {
      LOG(ERROR) << "MPEG-TS declares Dolby Vision but the first access unit "
                    "has no validated RPU.";
      return false;
    }
    return true;
  }
  const auto& hevc = video->codec_config();
  RCHECK(hevc.size() >= 23 && (hevc[16] & 3) == 1 &&
         (hevc[17] & 7) == 2 && (hevc[18] & 7) == 2);
  uint8_t level;
  RCHECK(DOVIDecoderConfigurationRecord::GetProfile5Level(
      video->width(), video->height(), video->time_scale(), sample.duration(), &level));
  std::vector<uint8_t> dovi(24, 0);
  dovi[0] = 1;
  dovi[2] = 5 << 1;
  dovi[3] = (level << 3) | 5;
  const auto declared = dovi_descriptors_.find(pid);
  if (declared != dovi_descriptors_.end()) {
    RCHECK((declared->second[2] >> 1) == 5 && (declared->second[3] & 7) == 5);
    std::copy(declared->second.begin(), declared->second.end(), dovi.begin());
  }
  DOVIDecoderConfigurationRecord config;
  RCHECK(config.Parse(dovi));
  video->set_codec(kCodecH265DolbyVision);
  const FourCC format =
      video->h26x_stream_format() == H26xStreamFormat::kNalUnitStreamWithParameterSetNalus
          ? FOURCC_dvhe : FOURCC_dvh1;
  video->set_codec_string(config.GetCodecString(format));
  auto extra = video->extra_config();
  const uint8_t box[] = {0, 0, 0, 32, 'd', 'v', 'c', 'C'};
  extra.insert(extra.end(), box, box + sizeof(box));
  extra.insert(extra.end(), dovi.begin(), dovi.end());
  video->set_extra_config(extra);
  return true;
}

void Mp2tMediaParser::OnEmitTextSample(uint32_t pes_pid,
                                       std::shared_ptr<TextSample> new_sample) {
  DCHECK(new_sample);
  DVLOG(LOG_LEVEL_ES) << "OnEmitTextSample: "
                      << " pid=" << pes_pid
                      << " start=" << new_sample->start_time();

  // Add the sample to the appropriate PID sample queue.
  auto pid_state = pids_.find(pes_pid);
  if (pid_state == pids_.end()) {
    LOG(ERROR) << "PID State for new sample not found (pid = "
               << pes_pid << ").";
    return;
  }
  pid_state->second->text_sample_queue_.push_back(std::move(new_sample));
}

bool Mp2tMediaParser::EmitRemainingSamples() {
  DVLOG(LOG_LEVEL_ES) << "Mp2tMediaParser::EmitRemainingBuffers";

  // No buffer should be sent until fully initialized.
  if (parse_error_)
    return false;
  if (!is_initialized_)
    return true;

  if (bbts_->active() && !bbts_origin_set_) {
    int64_t first_dts = std::numeric_limits<int64_t>::max();
    for (const auto& pair : pids_) {
      if ((pair.second->pid_type() == PidState::kPidAudioPes ||
           pair.second->pid_type() == PidState::kPidVideoPes) &&
          pair.second->IsEnabled()) {
        // Wait for each enabled A/V track, retaining its original relative
        // offset. A common origin keeps separately packaged tracks in sync.
        if (pair.second->media_sample_queue_.empty()) return true;
        first_dts = std::min(first_dts, pair.second->media_sample_queue_.front()->dts());
      }
    }
    if (first_dts == std::numeric_limits<int64_t>::max()) return true;
    bbts_origin_ = first_dts;
    bbts_origin_set_ = true;
  }

  // Buffer emission.
  for (const auto& pid_pair : pids_) {
    for (auto sample : pid_pair.second->media_sample_queue_) {
      if (bbts_->active()) {
        sample->set_dts(sample->dts() - bbts_origin_);
        sample->set_pts(sample->pts() - bbts_origin_);
      }
      RCHECK(new_media_sample_cb_.Run(pid_pair.first, sample));
    }
    pid_pair.second->media_sample_queue_.clear();

    for (auto sample : pid_pair.second->text_sample_queue_) {
      RCHECK(new_text_sample_cb_.Run(pid_pair.first, sample));
    }
    pid_pair.second->text_sample_queue_.clear();
  }

  return true;
}

}  // namespace mp2t
}  // namespace media
}  // namespace shaka
