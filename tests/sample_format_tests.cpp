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
}
int main() {
  try { test_decoders(); std::cout << "Sample format boundaries, unaligned planar decoding and saturation passed\n"; return 0; }
  catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
