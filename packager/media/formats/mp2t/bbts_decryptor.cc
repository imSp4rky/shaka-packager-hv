// Use of this source code is governed by the BSD-style license in LICENSE.

#include "packager/media/formats/mp2t/bbts_decryptor.h"

#include <algorithm>
#include <string>

#include "packager/base/logging.h"
#include "packager/media/base/key_source.h"
#include "packager/media/codecs/dovi_decoder_configuration_record.h"
#include "packager/media/codecs/nalu_reader.h"

namespace shaka {
namespace media {
namespace mp2t {
namespace {

int HexDigit(uint8_t value) {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'a' && value <= 'f') return value - 'a' + 10;
  if (value >= 'A' && value <= 'F') return value - 'A' + 10;
  return -1;
}

// Return an SEI's verified end in its original EBSP byte layout. A BBTS
// metadata trailer is removed only after all declared payloads and stop bits.
size_t SeiEnd(const std::vector<uint8_t>& nalu, size_t header) {
  std::vector<uint8_t> rbsp;
  std::vector<size_t> original_ends;
  unsigned zeros = 0;
  for (size_t i = header; i < nalu.size(); ++i) {
    if (zeros >= 2 && nalu[i] == 3 && i + 1 < nalu.size() && nalu[i + 1] <= 3) {
      zeros = 0;
      continue;
    }
    rbsp.push_back(nalu[i]);
    original_ends.push_back(i + 1);
    zeros = nalu[i] == 0 ? zeros + 1 : 0;
  }
  size_t pos = 0;
  while (pos < rbsp.size()) {
    if (rbsp[pos] == 0x80 && rbsp.size() - pos == 5 &&
        nalu.size() - original_ends[pos] == 4) {
      return original_ends[pos];
    }
    // Extended payload_type and payload_size, with strict size bounds.
    while (pos < rbsp.size() && rbsp[pos] == 255) ++pos;
    if (pos == rbsp.size()) return nalu.size();
    ++pos;
    size_t payload_size = 0;
    uint8_t part;
    do {
      if (pos == rbsp.size()) return nalu.size();
      part = rbsp[pos++];
      payload_size += part;
      if (payload_size > rbsp.size()) return nalu.size();
    } while (part == 255);
    if (payload_size > rbsp.size() - pos) return nalu.size();
    pos += payload_size;
  }
  return nalu.size();
}

}  // namespace

BbtsDecryptor::BbtsDecryptor(KeySource* key_source) : key_source_(key_source) {}

bool BbtsDecryptor::Configure(const uint8_t* service, size_t size) {
  // MDCM fields are not ordered: newer producers can start with "a0|"
  // and put the encryption mode before the IV. Match whole fields only.
  const std::string text =
      "|" + std::string(reinterpret_cast<const char*>(service), size) + "|";
  if (text.find("|mdcm|") == std::string::npos) return true;
  if (text.find("|s1:9:10|") == std::string::npos ||
      text.find("|a0|") == std::string::npos ||
      text.find("|e1|") == std::string::npos) {
    LOG(ERROR) << "Unsupported BBTS/MDCM encryption parameters.";
    return false;
  }
  const size_t start = text.find("|v");
  if (start == std::string::npos || start + 35 > text.size() ||
      text[start + 34] != '|') {
    LOG(ERROR) << "Missing or truncated BBTS video IV.";
    return false;
  }
  Iv next_iv{};
  for (size_t i = 0; i < 16; ++i) {
    const int high = HexDigit(text[start + 2 + i * 2]);
    const int low = HexDigit(text[start + 3 + i * 2]);
    if (high < 0 || low < 0) return false;
    next_iv[i] = static_cast<uint8_t>((high << 4) | low);
  }
  std::fill(next_iv.begin() + 12, next_iv.end(), 0);
  if (!has_key_) {
    EncryptionKey key;
    if (!key_source_ ||
        !key_source_->GetKey(std::vector<uint8_t>(16, 0), &key).ok() ||
        key.key.size() != 16 ||
        AES_set_encrypt_key(key.key.data(), 128, &aes_key_) != 0) {
      LOG(ERROR) << "BBTS needs a 128-bit raw decryption key with an all-zero "
                    "key ID (00000000000000000000000000000000).";
      return false;
    }
    has_key_ = true;
  }
  iv_ = next_iv;
  active_ = true;
  return true;
}

bool BbtsDecryptor::ParseSdt(const uint8_t* section, size_t size) {
  if (!section || size < 3) return false;
  if (section[0] != 0x42 && section[0] != 0x46) return true;
  if (size < 15) return false;
  const size_t end = 3 + (((section[1] & 15) << 8) | section[2]);
  if (end < 15 || end > size) return false;
  size_t service = 11;
  while (service < end - 4) {
    if (end - 4 - service < 5) return false;
    const size_t length = ((section[service + 3] & 15) << 8) | section[service + 4];
    size_t pos = service + 5;
    if (length > end - 4 - pos) return false;
    const size_t descriptor_end = pos + length;
    while (pos < descriptor_end) {
      if (descriptor_end - pos < 2) return false;
      const uint8_t tag = section[pos++];
      const size_t descriptor_size = section[pos++];
      if (descriptor_size > descriptor_end - pos) return false;
      if (tag == 0x48) {
        if (descriptor_size < 3) return false;
        const size_t name_position = 2 + section[pos + 1];
        if (name_position >= descriptor_size) return false;
        const size_t name_size = section[pos + name_position];
        if (name_size > descriptor_size - name_position - 1) return false;
        if (!Configure(section + pos + name_position + 1, name_size)) return false;
      }
      pos += descriptor_size;
    }
    service = descriptor_end;
  }
  return true;
}

bool BbtsDecryptor::DecryptNalu(const uint8_t* data, size_t size, size_t header,
                               const Iv& iv, std::vector<uint8_t>* clear) const {
  if (size <= header + 2) {
    clear->assign(data, data + size);
    return true;
  }
  clear->assign(data, data + header);
  // Encryption's EBSP escaping is outside the cipher; the recovered payload
  // already has its original escaping. Do not pad it with ciphertext bytes.
  unsigned zeros = 0;
  for (size_t i = header; i < size - 2; ++i) {
    if (zeros >= 2 && data[i] == 3 && i + 1 < size - 2 && data[i + 1] <= 3) {
      zeros = 0;
      continue;
    }
    clear->push_back(data[i]);
    zeros = data[i] == 0 ? zeros + 1 : 0;
  }
  Iv counter = iv;
  const size_t body_end = clear->size();
  for (size_t offset = header, block = 0; offset < body_end; offset += 16, ++block) {
    for (size_t i = 16; i > 0; --i) {
      if (++counter[i - 1] != 0) break;
    }
    Iv mask = counter;
    const size_t length = std::min(size_t{16}, body_end - offset);
    if (block % 10 == 0 || body_end - offset <= 16)
      AES_encrypt(counter.data(), mask.data(), &aes_key_);
    for (size_t i = 0; i < length; ++i) (*clear)[offset + i] ^= mask[i];
  }
  clear->insert(clear->end(), data + size - 2, data + size);
  const uint8_t type = header == 2 ? (data[0] >> 1) & 63 : data[0] & 31;
  if (header == 2 && type == 62 && clear->size() > 8 &&
      !DOVIDecoderConfigurationRecord::IsProfile5Rpu(clear->data(), clear->size()) &&
      DOVIDecoderConfigurationRecord::IsProfile5Rpu(clear->data(), clear->size() - 4)) {
    // Verified complete RPU + CRC + stop bit, followed by a BBTS trailer.
    clear->resize(clear->size() - 4);
  } else if ((header == 2 && (type == 39 || type == 40)) ||
             (header == 1 && type == 6)) {
    clear->resize(SeiEnd(*clear, header));
  }
  return true;
}

bool BbtsDecryptor::DecryptEs(const uint8_t* data, size_t size, size_t header,
                             const Iv& iv, std::vector<uint8_t>* clear) const {
  if (!active_ || !data || !clear || (header != 1 && header != 2)) return false;
  clear->clear();
  uint64_t start;
  uint8_t prefix;
  if (!NaluReader::FindStartCode(data, size, &start, &prefix)) return false;
  clear->insert(clear->end(), data, data + start);
  while (start < size) {
    const size_t payload = static_cast<size_t>(start) + prefix;
    uint64_t next = 0;
    uint8_t next_prefix = 0;
    const bool has_next = NaluReader::FindStartCode(
        data + payload, size - payload, &next, &next_prefix);
    const size_t end = has_next ? payload + static_cast<size_t>(next) : size;
    if (end < payload + header) return false;
    std::vector<uint8_t> nalu;
    if (!DecryptNalu(data + payload, end - payload, header, iv, &nalu)) return false;
    clear->insert(clear->end(), data + static_cast<size_t>(start), data + payload);
    clear->insert(clear->end(), nalu.begin(), nalu.end());
    start = end;
    prefix = next_prefix;
  }
  return true;
}

}  // namespace mp2t
}  // namespace media
}  // namespace shaka
