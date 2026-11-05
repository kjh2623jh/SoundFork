#include "capture-backend/sample_format.hpp"

#include <array>
#include <bit>
#include <iostream>
#include <stdexcept>

using namespace soundfork::capture;
namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template <class T> void put(std::byte* destination, T value) { std::memcpy(destination, &value, sizeof(value)); }
void test_decoders() {
  // Deliberately unaligned payload and independent planes.
  std::array<std::byte, 33> bytes{};
  auto* payload = bytes.data() + 1;
  std::array<std::int32_t, 8> integers{};
  std::array<float, 8> floats{};
  for (auto format : {SampleFormat::s16le, SampleFormat::s24le, SampleFormat::s32le}) {
    const auto stride = sample_bytes(format);
    for (std::size_t i = 0; i < 8; ++i) {
      if (format == SampleFormat::s16le) put(payload + i * stride, static_cast<std::int16_t>(i == 0 ? -32768 : i == 1 ? 32767 : i));
      if (format == SampleFormat::s24le) {
        const auto value = i == 0 ? 0x800000U : i == 1 ? 0x7fffffU : static_cast<unsigned>(i);
        for (unsigned b = 0; b < 3; ++b) payload[i * stride + b] = static_cast<std::byte>((value >> (8 * b)) & 255);
      }
      if (format == SampleFormat::s32le) put(payload + i * stride, i == 0 ? INT32_MIN : i == 1 ? INT32_MAX : static_cast<std::int32_t>(i));
    }
    require(decode_planar(format, 4, payload, 8 * stride, integers.data(), integers.size()), "integer decode failed");
    require(integers[0] == INT32_MIN, "negative minimum/sign extension");
    const auto maximum = format == SampleFormat::s16le ? 2147418112 : format == SampleFormat::s24le ? 2147483392 : INT32_MAX;
    require(integers[2] == maximum, "positive maximum");
    require(integers[1] == (format == SampleFormat::s16le ? 262144 : format == SampleFormat::s24le ? 1024 : 4), "channel isolation");
    require(decode_planar(format, 4, payload, 8 * stride, floats.data(), floats.size()), "integer to float failed");
    require(floats[0] == -1.0F, "integer normalization");
    require(!decode_planar(format, 4, payload, 8 * stride - 1, integers.data(), integers.size()), "short payload accepted");
  }
  const std::array<float, 8> values{-1.0F, 1.0F, -0.5F / 2147483648.0F, 0.5F / 2147483648.0F,
      2.0F, -2.0F, (std::numeric_limits<float>::quiet_NaN)(), (std::numeric_limits<float>::infinity)()};
  std::memcpy(payload, values.data(), sizeof(values));
  require(decode_planar(SampleFormat::f32le, 4, payload, 32, integers.data(), 8), "float to int failed");
  require(integers == std::array<std::int32_t, 8>{INT32_MIN, INT32_MAX, INT32_MAX, INT32_MIN, -1, 0, 1, 0}, "float saturation/rounding/nonfinite");
  require(decode_planar(SampleFormat::f32le, 4, payload, 32, floats.data(), 8), "float passthrough failed");
  require(floats[1] == 2.0F && floats[3] == -2.0F && floats[5] == 0 && floats[7] == 0, "float preservation/nonfinite");
  require(!decode_planar(SampleFormat::invalid, 4, payload, 32, floats.data(), 8), "invalid enum accepted");
  require(!decode_planar(SampleFormat::s32le, UINT32_MAX, payload, 32, integers.data(), 8), "oversized frames accepted");
  require(!decode_planar(SampleFormat::s32le, 4, payload, 32, integers.data(), 7), "short output accepted");
  require(decode_int32(reinterpret_cast<const std::byte*>(values.data()) + 6 * 4, SampleFormat::f32le) == 0, "NaN not silent");
}
void write_word(std::byte* out, std::uint64_t word, unsigned bytes, bool msb) {
  for(unsigned b=0;b<bytes;++b) out[b]=static_cast<std::byte>((word >> ((msb ? bytes-1-b : b)*8)) & 255);
}
void test_extended() {
  struct IntegerCase { SampleFormat format; unsigned bytes,bits; bool msb; };
  const IntegerCase cases[]{
    {SampleFormat::s16be,2,16,true},{SampleFormat::s24be,3,24,true},{SampleFormat::s32be,4,32,true},
    {SampleFormat::s32le16,4,16,false},{SampleFormat::s32le18,4,18,false},
    {SampleFormat::s32le20,4,20,false},{SampleFormat::s32le24,4,24,false},
    {SampleFormat::s32be16,4,16,true},{SampleFormat::s32be18,4,18,true},
    {SampleFormat::s32be20,4,20,true},{SampleFormat::s32be24,4,24,true}};
  std::array<std::byte,65> storage{}; auto* payload=storage.data()+1;
  std::array<std::int32_t,8> ints{}; std::array<float,8> floats{};
  for(const auto& c: cases) {
    require(sample_bytes(c.format)==c.bytes,"container width");
    const std::int64_t minimum=-(1LL<<(c.bits-1)), maximum=(1LL<<(c.bits-1))-1;
    const std::int64_t values[]{minimum,maximum,-1,1,0,minimum,maximum,1};
    for(unsigned i=0;i<8;++i) {
      auto word=static_cast<std::uint64_t>(values[i]);
      // Invalid upper padding is deliberately ignored; only the declared valid bits count.
      if(c.bytes==4 && c.bits<32) word=(word & ((1ULL<<c.bits)-1)) | (0xa5a5a5a5ULL & ~((1ULL<<c.bits)-1));
      write_word(payload+i*c.bytes,word,c.bytes,c.msb);
    }
    require(decode_planar(c.format,4,payload,8*c.bytes,ints.data(),8),"extended integer decode");
    require(decode_planar(c.format,4,payload,8*c.bytes,floats.data(),8),"extended integer float decode");
    for(unsigned frame=0;frame<4;++frame) for(unsigned channel=0;channel<2;++channel) {
      const auto expected=values[channel*4+frame]*(1LL<<(32-c.bits));
      require(ints[frame*2+channel]==expected,"extended sign/scale/channel order");
    }
    require(floats[0]==-1.0F,"extended normalization");
    require(!decode_planar(c.format,4,payload,8*c.bytes-1,ints.data(),8),"extended short payload");
  }
  for(auto format:{SampleFormat::f64le,SampleFormat::f32be,SampleFormat::f64be}) {
    const bool wide=format!=SampleFormat::f32be, msb=format!=SampleFormat::f64le;
    const unsigned bytes=wide?8U:4U;
    const double values[]{-1,1,-0.5/2147483648.0,0.5/2147483648.0,2,-2,
      (std::numeric_limits<double>::quiet_NaN)(),(std::numeric_limits<double>::infinity)()};
    for(unsigned i=0;i<8;++i) write_word(payload+i*bytes,
        wide?std::bit_cast<std::uint64_t>(values[i]):std::bit_cast<std::uint32_t>(static_cast<float>(values[i])),bytes,msb);
    require(decode_planar(format,4,payload,8*bytes,ints.data(),8),"extended float integer decode");
    require(ints==std::array<std::int32_t,8>{INT32_MIN,INT32_MAX,INT32_MAX,INT32_MIN,-1,0,1,0},"extended float saturation/rounding/NaN");
    require(decode_planar(format,4,payload,8*bytes,floats.data(),8),"extended float decode");
    require(floats[1]==2 && floats[3]==-2 && floats[5]==0 && floats[7]==0,"float preservation/nonfinite");
    require(!decode_planar(format,4,payload,8*bytes-1,floats.data(),8),"short float64 payload");
    if(wide) {
      write_word(payload,std::bit_cast<std::uint64_t>((std::numeric_limits<double>::max)()),8,msb);
      require(decode_planar(format,4,payload,64,floats.data(),8) && std::isfinite(floats[0]) &&
          floats[0]==(std::numeric_limits<float>::max)(),"Float64 finite overflow handling");
    }
  }
  require(sample_bytes(static_cast<SampleFormat>(19))==0 && sample_bytes(static_cast<SampleFormat>(UINT32_MAX))==0,
      "unknown wire format accepted");
}
}
int main() {
  try { test_decoders(); test_extended(); std::cout << "Sample format boundaries, unaligned planar decoding and saturation passed\n"; return 0; }
  catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
