// Use of this source code is governed by the BSD-style license in LICENSE.
#include "packager/media/formats/mp2t/bbts_decryptor.h"
#include "packager/media/formats/mp2t/ts_section_pes.h"
#include "packager/media/formats/mp2t/es_parser.h"
#include "packager/media/base/key_source.h"
#include "packager/media/base/media_sample.h"
#include "packager/media/codecs/dovi_decoder_configuration_record.h"
#include "packager/media/formats/mp2t/mp2t_media_parser.h"
#include "packager/media/test/test_data_util.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <string>
#include <tuple>
namespace shaka { namespace media { namespace mp2t { namespace {
class TestKeys : public KeySource {
 public:
 bool available = true; size_t key_size = 16; size_t calls = 0;
 Status FetchKeys(EmeInitDataType,const std::vector<uint8_t>&) override {return Status::OK;}
 Status GetKey(const std::string&,EncryptionKey*) override {return Status(error::INVALID_ARGUMENT,"label");}
 Status GetKey(const std::vector<uint8_t>& id, EncryptionKey* out) override {
  ++calls; if(!available || id!=std::vector<uint8_t>(16,0))return Status(error::INVALID_ARGUMENT,"KID");
  out->key.resize(key_size);for(size_t i=0;i<key_size;++i)out->key[i]=static_cast<uint8_t>(i);return Status::OK;
 }
 Status GetCryptoPeriodKey(uint32_t,uint32_t,const std::string&,EncryptionKey*) override {return Status(error::UNIMPLEMENTED,"period");}
};
std::vector<uint8_t> Sdt(const std::string& name) {
 std::vector<uint8_t> bytes={0x42,0xb0,0,0,1,0xc1,0,0,0,1,0xff,0,1,0,0xf0,0,0x48,0,1,0};
 bytes.push_back(static_cast<uint8_t>(name.size()));bytes.insert(bytes.end(),name.begin(),name.end());
 bytes[17]=static_cast<uint8_t>(bytes.size()-18);bytes[15]=static_cast<uint8_t>(bytes.size()-16);
 bytes.insert(bytes.end(),4,0);const size_t length=bytes.size()-3;bytes[1]|=static_cast<uint8_t>(length>>8);bytes[2]=static_cast<uint8_t>(length);return bytes;
}
const char kMetadata[]="mdcm|s1:9:10|a0|v00112233445566778899aabbccddeeff|e1|f000000|";
const uint8_t kCipher[] = {
    0x00, 0x00, 0x00, 0x01, 0x02, 0x01, 0x7c, 0x7f, 0x13, 0xe1, 0xeb, 0xe3,
    0x42, 0xf5, 0x2a, 0x73, 0xfc, 0xbb, 0x9e, 0xd1, 0xf4, 0x63, 0x10, 0x00,
    0x30, 0x20, 0x50, 0x40, 0x70, 0x60, 0x90, 0x80, 0xb0, 0xa0, 0x1c, 0x1d,
    0x1e, 0x1d, 0x00, 0x00, 0x03, 0x01, 0x10, 0x60, 0x70, 0x40, 0x50, 0xa0,
    0xb0, 0x80, 0x90, 0x2c, 0x2d, 0x2e, 0x2c, 0x30, 0x20, 0x10, 0x00, 0x70,
    0x60, 0x50, 0x40, 0xb0, 0xa0, 0x90, 0x80, 0x3c, 0x3d, 0x3e, 0x3b, 0x40,
    0x50, 0x60, 0x70, 0x00, 0x10, 0x20, 0x30, 0xc0, 0xd0, 0xe0, 0xf0, 0x4c,
    0x4d, 0x4e, 0x4a, 0x50, 0x40, 0x70, 0x60, 0x10, 0x00, 0x30, 0x20, 0xd0,
    0xc0, 0xf0, 0xe0, 0x5c, 0x5d, 0x5e, 0x59, 0x60, 0x70, 0x40, 0x50, 0x20,
    0x30, 0x00, 0x10, 0xe0, 0xf0, 0xc0, 0xd0, 0x6c, 0x6d, 0x6e, 0x68, 0x70,
    0x60, 0x50, 0x40, 0x30, 0x20, 0x10, 0x00, 0xf0, 0xe0, 0xd0, 0xc0, 0x7c,
    0x7d, 0x7e, 0x77, 0x80, 0x90, 0xa0, 0xb0, 0xc0, 0xd0, 0xe0, 0xf0, 0x00,
    0x10, 0x20, 0x30, 0x8c, 0x8d, 0x8e, 0x86, 0x90, 0x80, 0xb0, 0xa0, 0xd0,
    0xc0, 0xf0, 0xe0, 0x10, 0x00, 0x30, 0x20, 0x9c, 0x9d, 0x9e, 0x95, 0x06,
    0x01, 0x1f, 0xc6, 0xa1, 0x17, 0xf4, 0x60, 0xfd, 0x2c, 0x9c, 0xa3, 0x7d,
    0xc6, 0xcd, 0xff, 0x58, 0x5a, 0xc5, 0x24, 0x73, 0xaa, 0xbb,
};
const uint8_t kSeiCipher[] = {
    0x00, 0x00, 0x00, 0x01, 0x4e, 0x01, 0x78, 0x7d, 0x10, 0xe0, 0xec, 0x66,
    0x9a, 0x5f, 0xbe, 0xef,
};
std::vector<uint8_t> Expected() {
 std::vector<uint8_t> expected={0,0,0,1,2,1};for(size_t i=0;i<181;++i)expected.push_back(static_cast<uint8_t>(i));
 expected[6+32]=0;expected[6+33]=17;expected[6+34]=35;expected.push_back(0xaa);expected.push_back(0xbb);return expected;
}
TEST(BbtsDecryptorTest, SparseCounterAndCipherEscapingGoldenVector) {
 TestKeys keys;BbtsDecryptor decryptor(&keys);auto sdt=Sdt(kMetadata);ASSERT_TRUE(decryptor.ParseSdt(sdt.data(),sdt.size()));
 EXPECT_TRUE(decryptor.active());EXPECT_EQ(1u,keys.calls);EXPECT_EQ(0,decryptor.iv()[15]);
 std::vector<uint8_t> clear;ASSERT_TRUE(decryptor.DecryptEs(kCipher,sizeof(kCipher),2,decryptor.iv(),&clear));
 EXPECT_EQ(Expected(),clear);EXPECT_LT(clear.size(),sizeof(kCipher));
}
TEST(BbtsDecryptorTest, AcceptsReorderedMdcmFields) {
  TestKeys keys;
  BbtsDecryptor decryptor(&keys);
  const auto sdt = Sdt(
      "a0|mdcm|s1:9:10|e1|v00112233445566778899aabbccddeeff|f000000|gv|");
  ASSERT_TRUE(decryptor.ParseSdt(sdt.data(), sdt.size()));
  std::vector<uint8_t> clear;
  ASSERT_TRUE(decryptor.DecryptEs(kCipher, sizeof(kCipher), 2,
                                decryptor.iv(), &clear));
  EXPECT_EQ(Expected(), clear);
  EXPECT_EQ(1u, keys.calls);
}
TEST(BbtsDecryptorTest, RemovesOnlySeiTransportTrailer) {
 TestKeys keys;BbtsDecryptor decryptor(&keys);auto sdt=Sdt(kMetadata);ASSERT_TRUE(decryptor.ParseSdt(sdt.data(),sdt.size()));
 std::vector<uint8_t> clear;ASSERT_TRUE(decryptor.DecryptEs(kSeiCipher,sizeof(kSeiCipher),2,decryptor.iv(),&clear));
 EXPECT_EQ((std::vector<uint8_t>{0,0,0,1,0x4e,1,4,3,1,2,3,0x80}),clear);
}
TEST(BbtsDecryptorTest, IgnoresOrdinaryServiceWithoutKey) {
 BbtsDecryptor decryptor(nullptr);auto sdt=Sdt("Ordinary TV");EXPECT_TRUE(decryptor.ParseSdt(sdt.data(),sdt.size()));EXPECT_FALSE(decryptor.active());
 sdt[0]=0x4a;EXPECT_TRUE(decryptor.ParseSdt(sdt.data(),sdt.size()));
}
TEST(BbtsDecryptorTest, RequiresExplicitZeroKidKey) {
 auto sdt=Sdt(kMetadata);BbtsDecryptor none(nullptr);EXPECT_FALSE(none.ParseSdt(sdt.data(),sdt.size()));
 TestKeys keys;keys.available=false;BbtsDecryptor missing(&keys);EXPECT_FALSE(missing.ParseSdt(sdt.data(),sdt.size()));
 keys.available=true;keys.key_size=15;BbtsDecryptor short_key(&keys);EXPECT_FALSE(short_key.ParseSdt(sdt.data(),sdt.size()));
}
TEST(BbtsDecryptorTest, RejectsTruncatedSignalingAndInvalidIv) {
 auto sdt=Sdt(kMetadata);TestKeys keys;
 for(size_t n=0;n<sdt.size();++n){BbtsDecryptor d(&keys);EXPECT_FALSE(d.ParseSdt(sdt.data(),n))<<n;}
 std::string invalid=kMetadata;invalid[invalid.find("|v")+2]='z';sdt=Sdt(invalid);BbtsDecryptor d(&keys);EXPECT_FALSE(d.ParseSdt(sdt.data(),sdt.size()));
}
TEST(BbtsDecryptorTest, RejectsUnsupportedEncryptionParameters) {
 TestKeys keys;std::string invalid=kMetadata;invalid.replace(invalid.find("a0"),2,"a1");auto sdt=Sdt(invalid);BbtsDecryptor d(&keys);EXPECT_FALSE(d.ParseSdt(sdt.data(),sdt.size()));
}
TEST(BbtsDecryptorTest, RejectsInvalidElementaryFraming) {
 TestKeys keys;BbtsDecryptor d(&keys);auto sdt=Sdt(kMetadata);ASSERT_TRUE(d.ParseSdt(sdt.data(),sdt.size()));std::vector<uint8_t> clear;
 EXPECT_FALSE(d.DecryptEs(nullptr,0,2,d.iv(),&clear));EXPECT_FALSE(d.DecryptEs(kCipher,sizeof(kCipher),0,d.iv(),&clear));
 const uint8_t bad[]={1,2,3,4};EXPECT_FALSE(d.DecryptEs(bad,sizeof(bad),2,d.iv(),&clear));
}
class CaptureEs : public EsParser {
 public:
 explicit CaptureEs(std::vector<uint8_t>* output):EsParser(256),output_(output){}
 bool Parse(const uint8_t* data,int size,int64_t pts,int64_t)override{EXPECT_EQ(0,pts);output_->assign(data,data+size);return true;}
 bool Flush()override{return true;}void Reset()override{}
 private:std::vector<uint8_t>* output_;
};
TEST(BbtsDecryptorTest, PesKeepsIvSnapshotAcrossSignalingChange) {
 TestKeys keys;BbtsDecryptor d(&keys);auto sdt=Sdt(kMetadata);ASSERT_TRUE(d.ParseSdt(sdt.data(),sdt.size()));
 std::vector<uint8_t> clear;TsSectionPes parser(std::unique_ptr<EsParser>(new CaptureEs(&clear)),&d,2);
 std::vector<uint8_t> pes={0,0,1,0xe0,0,0,0x80,0x80,5,0x21,0,1,0,1};pes.insert(pes.end(),kCipher,kCipher+sizeof(kCipher));
 const size_t split=pes.size()/2;ASSERT_TRUE(parser.Parse(true,pes.data(),static_cast<int>(split)));
 std::string changed=kMetadata;changed[changed.find("|v")+2]='f';sdt=Sdt(changed);ASSERT_TRUE(d.ParseSdt(sdt.data(),sdt.size()));
 ASSERT_TRUE(parser.Parse(false,pes.data()+split,static_cast<int>(pes.size()-split)));ASSERT_TRUE(parser.Flush());EXPECT_EQ(Expected(),clear);EXPECT_EQ(1u,keys.calls);
}

TEST(BbtsDecryptorTest, PreservesCompleteRpuAndRemovesVerifiedTrailer) {
 const uint8_t encrypted[] = {
    0x00, 0x00, 0x00, 0x01, 0x7c, 0x01, 0x65, 0x76, 0x18, 0xe2, 0xaf, 0x87,
    0xf2, 0xa2, 0x4c, 0x5a, 0xf6, 0xb0, 0x12, 0x2e, 0x8a, 0xba, 0x40, 0x01,
    0x23, 0xef, 0x40, 0xd5, 0x79, 0x8b, 0x88, 0x66, 0x57, 0xbb, 0x00, 0x03,
    0x00, 0x1e, 0x98, 0x88, 0x83, 0x63, 0x70, 0x30, 0x2c, 0xe9, 0x8a, 0xe9,
    0x87, 0x7a, 0xf3, 0xc1, 0x50, 0x32, 0x12, 0x58, 0x43, 0x01, 0x45, 0x27,
    0x14, 0xde, 0x5c, 0xb9, 0x9c, 0x99, 0x1b, 0x6f, 0xbe, 0xe6, 0xdc, 0x5b,
    0xf6, 0x0c, 0xba, 0xfa, 0xdc, 0x5f, 0x2b, 0x52, 0x26, 0xe2, 0x51, 0x74,
    0x56, 0x98, 0x60, 0x5b, 0x8d, 0xb2, 0x65, 0xb8, 0x38, 0xd4, 0xa2, 0xa6,
    0x30, 0x6e, 0x55, 0xa9, 0x49, 0x52, 0x18, 0x5e, 0xc5, 0x13, 0x48, 0xa8,
    0xeb, 0x3f, 0x8e, 0xbb, 0xe6, 0x5f, 0x67, 0x73, 0x7c, 0xea, 0x50, 0x8c,
    0xa8, 0xfd, 0x83, 0x6c, 0x84, 0xd7, 0x69, 0x96, 0x62, 0xbb, 0x00, 0xc7,
    0xc1, 0xac, 0x48, 0x11, 0x1d, 0x25, 0x85, 0x45, 0xae, 0x77, 0x88, 0xdb,
    0x50, 0x2e, 0xc0, 0x00, 0x00, 0x0a, 0x00, 0x13, 0x22, 0x33, 0x47, 0x55,
    0x64, 0x77, 0x88, 0x9a, 0xaa, 0xab, 0xae, 0x7f, 0xa8, 0xf5, 0x0e, 0x5f,
    0x15, 0xb5, 0xab, 0xcd, 0xfa, 0x38, 0xfd, 0x7a, 0x9e, 0xd8, 0x7f, 0x14,
    0x9c, 0x90, 0x00, 0x11, 0x21, 0x33, 0x44, 0x56, 0x66, 0x77, 0x8b, 0x99,
    0xb3, 0xb3, 0x1f, 0x73, 0x80, 0x58, 0x20, 0x21, 0x2a, 0x35, 0x29, 0x6f,
    0x4b, 0x77, 0x48, 0xb1, 0x8b, 0xc4, 0xa8, 0x0a, 0x7e, 0xea, 0xf6, 0x91,
    0x63, 0x33, 0x45, 0xd5, 0x30, 0x30, 0x8a, 0x36, 0x5a, 0x45, 0x4f, 0xaa,
    0xff, 0x0c, 0x00, 0x12, 0x22, 0x83, 0x5a, 0x57, 0xf9, 0x8e, 0x74, 0x06,
    0x5b, 0x45, 0x04, 0x00, 0x08, 0x07, 0x8a, 0x14, 0x84, 0x13, 0x50, 0x55,
    0x66, 0x74, 0x88, 0x99, 0xf2, 0xb9, 0xc0, 0x48, 0x30, 0x6d, 0xb7, 0x92,
    0xc2, 0x92, 0x13, 0x65, 0x3c, 0xcb, 0x74, 0xea, 0x8d, 0xcf, 0x85, 0x77,
    0x3c, 0xbe, 0xef,
 };
 TestKeys keys;BbtsDecryptor d(&keys);auto sdt=Sdt(kMetadata);ASSERT_TRUE(d.ParseSdt(sdt.data(),sdt.size()));
 std::vector<uint8_t> clear;ASSERT_TRUE(d.DecryptEs(encrypted,sizeof(encrypted),2,d.iv(),&clear));
 ASSERT_EQ(275u,clear.size());EXPECT_TRUE(DOVIDecoderConfigurationRecord::IsProfile5Rpu(clear.data()+4,clear.size()-4));
 auto corrupt=std::vector<uint8_t>(encrypted,encrypted+sizeof(encrypted));corrupt[30]^=1;
 ASSERT_TRUE(d.DecryptEs(corrupt.data(),corrupt.size(),2,d.iv(),&clear));
 EXPECT_GT(clear.size(),275u);EXPECT_FALSE(DOVIDecoderConfigurationRecord::IsProfile5Rpu(clear.data()+4,clear.size()-4));
}
std::vector<uint8_t> SignalingPacket() {
 auto sdt=Sdt(kMetadata);const uint8_t crc[]={0x4d,0x23,0x50,0x97};std::copy(crc,crc+4,sdt.end()-4);
 std::vector<uint8_t> packet(188,0xff);packet[0]=0x47;packet[1]=0x40;packet[2]=17;packet[3]=0x10;packet[4]=0;
 std::copy(sdt.begin(),sdt.end(),packet.begin()+5);return packet;
}
void InitParser(Mp2tMediaParser* parser,KeySource* keys) {
 parser->Init(MediaParser::InitCB([](const std::vector<std::shared_ptr<StreamInfo>>&){ }),
  MediaParser::NewMediaSampleCB([](uint32_t,std::shared_ptr<MediaSample>)->bool{return true;}),
  MediaParser::NewTextSampleCB([](uint32_t,std::shared_ptr<TextSample>)->bool{return true;}),keys);
}
TEST(BbtsDecryptorTest, RejectsIncompleteTransportPacketAtEof) {
 TestKeys keys;Mp2tMediaParser parser;InitParser(&parser,&keys);auto packet=SignalingPacket();
 ASSERT_TRUE(parser.Parse(packet.data(),static_cast<int>(packet.size())));EXPECT_EQ(1u,keys.calls);
 ASSERT_TRUE(parser.Parse(packet.data(),100));EXPECT_FALSE(parser.Flush());
}
TEST(BbtsDecryptorTest, RejectsSignalingWithoutMediaAtEof) {
 TestKeys keys;Mp2tMediaParser parser;InitParser(&parser,&keys);auto packet=SignalingPacket();
 ASSERT_TRUE(parser.Parse(packet.data(),static_cast<int>(packet.size())));EXPECT_FALSE(parser.Flush());
}
TEST(BbtsDecryptorTest, LimitsIncompletePesBuffer) {
 TestKeys keys;BbtsDecryptor d(&keys);auto sdt=Sdt(kMetadata);ASSERT_TRUE(d.ParseSdt(sdt.data(),sdt.size()));
 std::vector<uint8_t> clear;TsSectionPes parser(std::unique_ptr<EsParser>(new CaptureEs(&clear)),&d,2);
 std::vector<uint8_t> oversized(16*1024*1024+1,0);oversized[2]=1;oversized[3]=0xe0;
 EXPECT_FALSE(parser.Parse(true,oversized.data(),static_cast<int>(oversized.size())));
}

TEST(BbtsDecryptorTest, LateSignalingPreservesClearTransportClock) {
  const auto data = ReadTestDataFile("bear-640x360-hevc.ts");
  ASSERT_FALSE(data.empty());
  const auto signal = SignalingPacket();
  using Timing = std::tuple<uint32_t, int64_t, int64_t, int64_t>;
  for (size_t chunk : {size_t{512}, size_t{65536}, data.size() + signal.size()}) {
    std::vector<Timing> expected;
    std::vector<Timing> actual;
    for (bool late_signal : {false, true}) {
      TestKeys keys;
      Mp2tMediaParser parser;
      auto* timings = late_signal ? &actual : &expected;
      parser.Init(
          MediaParser::InitCB([](const std::vector<std::shared_ptr<StreamInfo>>&) {}),
          MediaParser::NewMediaSampleCB(
              [timings](uint32_t pid, std::shared_ptr<MediaSample> sample) {
                timings->emplace_back(pid, sample->dts(), sample->pts(),
                                      sample->duration());
                return true;
              }),
          MediaParser::NewTextSampleCB(
              [](uint32_t, std::shared_ptr<TextSample>) { return false; }),
          &keys);
      auto input = data;
      if (late_signal)
        input.insert(input.end(), signal.begin(), signal.end());
      for (size_t offset = 0; offset < input.size(); offset += chunk) {
        ASSERT_TRUE(parser.Parse(input.data() + offset,
            static_cast<int>(std::min(chunk, input.size() - offset))));
      }
      ASSERT_TRUE(parser.Flush());
      EXPECT_EQ(late_signal ? 1u : 0u, keys.calls);
    }
    ASSERT_FALSE(expected.empty());
    EXPECT_EQ(expected, actual) << "append size=" << chunk;
  }
}
} } } }
