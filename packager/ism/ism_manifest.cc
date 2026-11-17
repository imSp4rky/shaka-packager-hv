// Use of this source code is governed by the BSD-style license in LICENSE.
#include "packager/ism/ism_manifest.h"
#include <algorithm>
#include <cctype>
#include <limits>
#include <memory>
#include <mutex>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/uri.h>

namespace shaka {
namespace ism {
namespace {
Status Invalid(const std::string& message) {
  return Status(error::PARSER_FAILURE, "ISM: " + message);
}
std::string Lower(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return text;
}
bool Named(xmlNode* node, const char* name) {
  return node->type == XML_ELEMENT_NODE && xmlStrEqual(node->name, BAD_CAST name);
}
std::string Attr(xmlNode* node, const char* name) {
  xmlChar* value = xmlGetProp(node, BAD_CAST name);
  if (!value) return {};
  std::string result(reinterpret_cast<const char*>(value));
  xmlFree(value);
  return result;
}
bool Number(const std::string& text, uint64_t* value) {
  if (text.empty()) return false;
  *value = 0;
  for (char c : text) {
    if (c < '0' || c > '9' ||
        *value > (std::numeric_limits<uint64_t>::max() - (c - '0')) / 10)
      return false;
    *value = *value * 10 + c - '0';
  }
  return true;
}
bool U32(xmlNode* node, const char* name, uint32_t* value, bool required) {
  const auto text = Attr(node, name);
  if (text.empty()) return !required;
  uint64_t number;
  if (!Number(text, &number) || number > UINT32_MAX) return false;
  *value = static_cast<uint32_t>(number);
  return true;
}
bool Hex(const std::string& text, std::vector<uint8_t>* data) {
  if (text.size() % 2) return false;
  data->clear();
  int high = -1;
  for (char c : text) {
    int n = c >= '0' && c <= '9' ? c - '0' :
            c >= 'a' && c <= 'f' ? c - 'a' + 10 :
            c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
    if (n < 0) return false;
    if (high < 0) high = n;
    else { data->push_back(static_cast<uint8_t>(high * 16 + n)); high = -1; }
  }
  return true;
}
bool Base64(const std::string& text, std::vector<uint8_t>* data) {
  const std::string alphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  uint32_t buffer = 0;
  unsigned bits = 0;
  unsigned symbols = 0, padding = 0;
  data->clear();
  for (char c : text) {
    if (c == ' ' || c == '\r' || c == '\n' || c == '\t') continue;
    if (c == '=') { if (++padding > 2) return false; continue; }
    const auto index = alphabet.find(c);
    if (padding || index == std::string::npos) return false;
    ++symbols;
    buffer = (buffer << 6) | static_cast<uint32_t>(index);
    bits += 6;
    if (bits >= 8) { bits -= 8; data->push_back(buffer >> bits); }
  }
  return (symbols + padding) % 4 == 0 &&
      padding == (symbols % 4 == 2 ? 2u : symbols % 4 == 3 ? 1u : 0u) &&
      bits < 6 && (bits == 0 || (buffer & ((1u << bits) - 1)) == 0);
}
bool KidNode(xmlNode* node, std::vector<uint8_t>* key_id) {
  for (; node; node = node->next) {
    if (Named(node, "ALGID")) {
      xmlChar* text = xmlNodeGetContent(node);
      const bool ctr = text && xmlStrEqual(text, BAD_CAST "AESCTR");
      xmlFree(text);
      if (!ctr) return false;
    }
    if (Named(node, "KID")) {
      const auto algorithm = Attr(node, "ALGID");
      if (!algorithm.empty() && algorithm != "AESCTR") return false;
      auto value = Attr(node, "VALUE");
      if (value.empty()) {
        xmlChar* text = xmlNodeGetContent(node);
        if (text) { value = reinterpret_cast<const char*>(text); xmlFree(text); }
      }
      std::vector<uint8_t> id;
      if (!Base64(value, &id) || id.size() != 16) return false;
      std::reverse(id.begin(), id.begin() + 4);
      std::reverse(id.begin() + 4, id.begin() + 6);
      std::reverse(id.begin() + 6, id.begin() + 8);
      if (!key_id->empty() && *key_id != id) return false;
      *key_id = std::move(id);
    }
    if (!KidNode(node->children, key_id)) return false;
  }
  return true;
}
bool Protection(xmlNode* node, std::vector<uint8_t>* key_id) {
  std::string system = Attr(node, "SystemID");
  if (system.size() > 2 && system.front() == '{' && system.back() == '}')
    system = system.substr(1, system.size() - 2);
  std::transform(system.begin(), system.end(), system.begin(),
      [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  if (system != "9A04F079-9840-4286-AB92-E65BE0885F95") return false;
  xmlChar* text = xmlNodeGetContent(node);
  if (!text) return false;
  std::vector<uint8_t> pro;
  const bool decoded = Base64(reinterpret_cast<const char*>(text), &pro);
  xmlFree(text);
  if (!decoded || pro.size() < 6) return false;
  const auto le16 = [&pro](size_t p) { return pro[p] | (pro[p + 1] << 8); };
  const uint32_t length = pro[0] | (uint32_t(pro[1]) << 8) |
                          (uint32_t(pro[2]) << 16) | (uint32_t(pro[3]) << 24);
  if (length != pro.size()) return false;
  size_t pos = 6;
  const unsigned records = le16(4);
  for (unsigned i = 0; i < records; ++i) {
    if (pos + 4 > pro.size()) return false;
    const unsigned type = le16(pos), size = le16(pos + 2);
    pos += 4;
    if (size > pro.size() - pos) return false;
    if (type == 1) {
      std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)> doc(
          xmlReadMemory(reinterpret_cast<const char*>(pro.data() + pos), size,
                        "playready.xml", "UTF-16LE", XML_PARSE_NONET), xmlFreeDoc);
      auto* root = doc ? xmlDocGetRootElement(doc.get()) : nullptr;
      if (!root || !Named(root, "WRMHEADER") || doc->intSubset || doc->extSubset ||
          !KidNode(root, key_id)) return false;
    }
    pos += size;
  }
  return pos == pro.size() && key_id->size() == 16;
}
void Replace(const std::string& token, const std::string& value, std::string* text) {
  size_t pos = 0;
  while ((pos = text->find(token, pos)) != std::string::npos) {
    text->replace(pos, token.size(), value);
    pos += value.size();
  }
}
// MS-SSTR 2.2.2.6: r is a one-based count, not DASH's additional repeats.
// Sample durations still come from the fragment's trun/tfhd, never from an
// estimate of the manifest's frame rate.
Status Timeline(xmlNode* stream_node, const Manifest& manifest, Stream* stream) {
  const size_t kMaxChunks = 1000000;
  for (auto* node = stream_node->children; node; node = node->next) {
    if (!Named(node, "c")) continue;
    uint64_t start = 0, duration = 0, count = 1;
    const auto time = Attr(node, "t"), length = Attr(node, "d");
    if (time.empty()) {
      if (!stream->chunks.empty()) {
        const auto& previous = stream->chunks.back();
        start = previous.start + previous.duration;
      }
    } else if (!Number(time, &start)) {
      return Invalid("invalid chunk start");
    }
    if (!Attr(node, "r").empty() && !Number(Attr(node, "r"), &count))
      return Invalid("invalid chunk repeat count");
    if (!count || count > kMaxChunks || stream->chunks.size() > kMaxChunks - count)
      return Invalid("unbounded chunk timeline");
    if (!length.empty()) {
      if (!Number(length, &duration)) return Invalid("invalid chunk duration");
    } else {
      if (time.empty() || count != 1)
        return Invalid("cannot infer an omitted or repeated chunk duration");
      if (!stream->chunks.empty()) {
        const auto& previous = stream->chunks.back();
        if (start <= previous.start) return Invalid("non-increasing chunk timeline");
        duration = start - previous.start;
      } else {
        auto* following = node->next;
        while (following && !Named(following, "c")) following = following->next;
        if (following) {
          uint64_t next;
          if (!Number(Attr(following, "t"), &next) || next <= start)
            return Invalid("cannot infer the first chunk duration");
          duration = next - start;
        } else {
          // Convert a single chunk's presentation duration exactly, without
          // overflowing duration * timescale or rounding a timestamp.
          const uint64_t whole = manifest.duration / manifest.timescale;
          const uint64_t fraction = manifest.duration % manifest.timescale;
          const uint64_t scaled_fraction = fraction * stream->timescale;
          if (scaled_fraction % manifest.timescale ||
              whole > (UINT64_MAX - scaled_fraction / manifest.timescale) /
                          stream->timescale)
            return Invalid("single chunk duration cannot be represented exactly");
          duration = whole * stream->timescale + scaled_fraction / manifest.timescale;
        }
      }
    }
    if (!duration || count > (UINT64_MAX - start) / duration ||
        (!stream->chunks.empty() && start <= stream->chunks.back().start))
      return Invalid("invalid or overflowing chunk timeline");
    for (uint64_t n = 0; n < count; ++n) {
      stream->chunks.push_back(Chunk(start, duration));
      start += duration;
    }
  }
  return Status::OK;
}
}  // namespace

bool IsManifestPath(const std::string& path) {
  std::string name = path.substr(0, path.find_first_of("?#"));
  std::transform(name.begin(), name.end(), name.begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return (name.size() >= 4 && name.compare(name.size() - 4, 4, ".ism") == 0) ||
         (name.size() >= 5 && name.compare(name.size() - 5, 5, ".ismc") == 0) ||
         (name.size() >= 9 && name.compare(name.size() - 9, 9, "/manifest") == 0);
}

Status ParseManifest(const std::string& xml, Manifest* manifest) {
  if (!manifest || xml.empty() || xml.size() > 64 * 1024 * 1024)
    return Invalid("empty or oversized manifest");
  static std::once_flag initialized;
  std::call_once(initialized, xmlInitParser);
  *manifest = Manifest();
  std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)> doc(
      xmlReadMemory(xml.data(), static_cast<int>(xml.size()), "manifest.ism",
                    nullptr, XML_PARSE_NONET), xmlFreeDoc);
  if (!doc || doc->intSubset || doc->extSubset)
    return Invalid("invalid XML or unsupported DTD");
  auto* root = xmlDocGetRootElement(doc.get());
  if (!root || !Named(root, "SmoothStreamingMedia"))
    return Invalid("expected a SmoothStreamingMedia client manifest");
  const auto live = Lower(Attr(root, "IsLive"));
  if (live == "true" || live == "1")
    return Invalid("a finite VOD manifest is required");
  if (!live.empty() && live != "false" && live != "0")
    return Invalid("invalid IsLive value");
  if (!U32(root, "TimeScale", &manifest->timescale, false) ||
      manifest->timescale == 0 ||
      !Number(Attr(root, "Duration"), &manifest->duration))
    return Invalid("invalid duration or timescale");
  for (auto* child = root->children; child; child = child->next) {
    if (Named(child, "Protection")) {
      bool has_header = false;
      for (auto* p = child->children; p; p = p->next) {
        if (Named(p, "ProtectionHeader")) {
          has_header = true;
          if (!Protection(p, &manifest->key_id))
            return Invalid("unsupported protection, algorithm or multiple KIDs");
        }
      }
      if (!has_header) return Invalid("empty protection header");
    }
    if (!Named(child, "StreamIndex")) continue;
    Stream stream;
    stream.type = Lower(Attr(child, "Type"));
    stream.name = Attr(child, "Name");
    stream.language = Attr(child, "Language");
    stream.url = Attr(child, "Url");
    stream.timescale = manifest->timescale;
    if (!U32(child, "TimeScale", &stream.timescale, false) || !stream.timescale)
      return Invalid("invalid stream timescale");
    // Sparse text/marker streams need not carry audio/video QualityLevels.
    // Retain their manifest indexes without requiring an unselected codec.
    if (stream.type == "text") {
      manifest->streams.push_back(std::move(stream));
      continue;
    }
    if (stream.type != "video" && stream.type != "audio")
      return Invalid("invalid StreamIndex type");
    for (auto* entry = child->children; entry; entry = entry->next) {
      if (Named(entry, "QualityLevel")) {
        Quality quality;
        uint32_t channels = 0, bits = 16, nal_length_size = 4;
        if (!U32(child, "MaxWidth", &quality.width, false) ||
            !U32(child, "MaxHeight", &quality.height, false))
          return Invalid("invalid inherited video dimensions");
        if (!U32(entry, "Bitrate", &quality.bitrate, true) ||
            !U32(entry, "MaxWidth", &quality.width, false) ||
            !U32(entry, "MaxHeight", &quality.height, false) ||
            !U32(entry, "SamplingRate", &quality.sampling_rate, false) ||
            !U32(entry, "Channels", &channels, false) || channels > UINT16_MAX ||
            !U32(entry, "BitsPerSample", &bits, false) || bits > UINT16_MAX ||
            !U32(entry, "NALUnitLengthField", &nal_length_size, false) ||
            (nal_length_size != 1 && nal_length_size != 2 && nal_length_size != 4) ||
            !Hex(Attr(entry, "CodecPrivateData"), &quality.codec_private_data))
          return Invalid("invalid QualityLevel");
        quality.fourcc = Attr(entry, "FourCC");
        quality.channels = channels;
        quality.bits_per_sample = bits;
        quality.nal_length_size = static_cast<uint8_t>(nal_length_size);
        stream.qualities.push_back(std::move(quality));
      }
    }
    const auto timeline_status = Timeline(child, *manifest, &stream);
    if (!timeline_status.ok()) return timeline_status;
    if (stream.qualities.empty() || stream.chunks.empty() || stream.url.empty())
      return Invalid("stream has no quality, chunks or URL");
    manifest->streams.push_back(std::move(stream));
  }
  return manifest->streams.empty() ? Invalid("no streams") : Status::OK;
}

Status ResolveFragment(const std::string& manifest_path, const Stream& stream,
                       const Quality& quality, const Chunk& chunk,
                       std::string* location) {
  std::string path = stream.url;
  Replace("{bitrate}", std::to_string(quality.bitrate), &path);
  Replace("{Bitrate}", std::to_string(quality.bitrate), &path);
  Replace("{start time}", std::to_string(chunk.start), &path);
  Replace("{start_time}", std::to_string(chunk.start), &path);
  if (!location || path.empty() || path.find("\\") != std::string::npos ||
      path.find('{') != std::string::npos || path.find('}') != std::string::npos)
    return Invalid("invalid or unresolved fragment URL");
  const auto name = manifest_path.substr(0, manifest_path.find_first_of("?#"));
  const auto lower = Lower(name);
  if (lower.compare(0, 7, "http://") == 0 || lower.compare(0, 8, "https://") == 0) {
    std::unique_ptr<xmlChar, decltype(xmlFree)> resolved(
        xmlBuildURI(BAD_CAST path.c_str(), BAD_CAST name.c_str()), xmlFree);
    if (!resolved) return Invalid("cannot resolve fragment URL");
    std::unique_ptr<xmlURI, decltype(&xmlFreeURI)> uri(
        xmlParseURI(reinterpret_cast<const char*>(resolved.get())), xmlFreeURI);
    if (!uri || !uri->scheme || !uri->server || !*uri->server || uri->user ||
        (Lower(uri->scheme) != "http" && Lower(uri->scheme) != "https"))
      return Invalid("only HTTP(S) media URLs are supported");
    *location = reinterpret_cast<const char*>(resolved.get());
    location->erase(location->find('#') == std::string::npos ? location->size() : location->find('#'));
    return Status::OK;
  }
  if (path[0] == '/' || path.find_first_of(":?#") != std::string::npos)
    return Invalid("local fragments must use relative file paths");
  std::vector<std::string> segments;
  for (size_t p = 0; p < path.size();) {
    const size_t slash = path.find('/', p);
    const auto segment = path.substr(p, slash == std::string::npos ? slash : slash - p);
    if (segment == "..") {
      if (segments.empty()) return Invalid("fragment escapes the manifest directory");
      segments.pop_back();
    } else if (!segment.empty() && segment != ".") {
      segments.push_back(segment);
    }
    if (slash == std::string::npos) break;
    p = slash + 1;
  }
  if (segments.empty()) return Invalid("empty fragment path");
  path.clear();
  for (const auto& segment : segments) {
    if (!path.empty()) path += '/';
    path += segment;
  }
  const auto slash = name.find_last_of("/\\");
  *location = (slash == std::string::npos ? "" : name.substr(0, slash + 1)) + path;
  return Status::OK;
}
}  // namespace ism
}  // namespace shaka
