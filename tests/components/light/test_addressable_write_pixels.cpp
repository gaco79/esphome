#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "esphome/components/light/addressable_light.h"
#include "esphome/components/light/light_state.h"

namespace esphome::light::testing {

namespace {

std::array<uint16_t, 256> build_gamma_table(double gamma) {
  std::array<uint16_t, 256> table{};
  for (int i = 1; i < 256; i++) {
    double raw = std::round(std::pow(i / 255.0, gamma) * 65535.0);
    table[i] = static_cast<uint16_t>(std::max(1.0, std::min(65535.0, raw)));
  }
  return table;
}

// In-memory strip with a packed buffer. `stride` may exceed the channel count (like spi_led_strip,
// which has a brightness byte per LED), and `expose_layout` selects write_pixels()'s fast path.
class TestStrip : public AddressableLight {
 public:
  TestStrip(int32_t num_leds, uint8_t stride, ChannelColors colors, bool expose_layout)
      : num_leds_(num_leds),
        stride_(stride),
        colors_(colors),
        expose_layout_(expose_layout),
        buf_(num_leds * stride, 0xEE),
        effect_data_(num_leds, 0x5A) {}

  void setup() override {}
  void write_state(LightState * /*state*/) override {}
  int32_t size() const override { return this->num_leds_; }
  void clear_effect_data() override { std::fill(this->effect_data_.begin(), this->effect_data_.end(), 0); }
  LightTraits get_traits() override { return {}; }

  void set_local_brightness(uint8_t brightness) { this->correction_.set_local_brightness(brightness); }
  void set_gamma(const uint16_t *table) { this->correction_.set_gamma_table(table); }
  const std::vector<uint8_t> &buffer() const { return this->buf_; }
  const std::vector<uint8_t> &effect_data() const { return this->effect_data_; }

 protected:
  bool get_pixel_buffer_layout(PixelBufferLayout &layout) const override {
    if (!this->expose_layout_)
      return false;
    layout = {const_cast<uint8_t *>(this->buf_.data()), this->stride_, this->colors_};
    return true;
  }
  ESPColorView get_view_internal(int32_t index) const override {
    uint8_t *led = const_cast<uint8_t *>(this->buf_.data()) + index * this->stride_;
    return {led + this->colors_.r,
            led + this->colors_.g,
            led + this->colors_.b,
            this->colors_.has_white() ? led + this->colors_.w : nullptr,
            const_cast<uint8_t *>(this->effect_data_.data()) + index,
            &this->correction_};
  }

  int32_t num_leds_;
  uint8_t stride_;
  ChannelColors colors_;
  bool expose_layout_;
  std::vector<uint8_t> buf_;
  std::vector<uint8_t> effect_data_;
};

Color test_color(int32_t i) {
  return Color(static_cast<uint8_t>(i * 7), static_cast<uint8_t>(i * 13 + 5), static_cast<uint8_t>(255 - i * 3),
               static_cast<uint8_t>(i * 29));
}

struct Layout {
  uint8_t stride;
  ChannelColors colors;
};

const Layout LAYOUTS[] = {
    {3, {1, 0, 2, ChannelColors::NO_WHITE}},  // GRB
    {4, {1, 0, 2, 3}},                        // GRBW
    {4, {3, 2, 1, ChannelColors::NO_WHITE}},  // spi_led_strip style: unused byte first
};

}  // namespace

// write_pixels() must give exactly the bytes that per-LED operator[]...set() gives, on both its
// fast path and its fallback, for every layout, with gamma on and off and non-trivial brightness.
TEST(AddressableWritePixels, MatchesPerLedSet) {
  constexpr int32_t num_leds = 300;
  const auto gamma = build_gamma_table(2.8);
  for (const Layout &layout : LAYOUTS) {
    for (bool with_gamma : {false, true}) {
      for (bool expose_layout : {false, true}) {
        TestStrip expected(num_leds, layout.stride, layout.colors, false);
        TestStrip actual(num_leds, layout.stride, layout.colors, expose_layout);
        for (TestStrip *strip : {&expected, &actual}) {
          strip->set_correction(0.9f, 0.6f, 0.3f, 0.75f);
          strip->set_local_brightness(173);
          strip->set_gamma(with_gamma ? gamma.data() : nullptr);
        }
        for (int32_t i = 0; i < num_leds; i++)
          expected[i].set(test_color(i));
        actual.write_pixels(0, num_leds, test_color);

        EXPECT_EQ(actual.buffer(), expected.buffer())
            << "stride=" << int(layout.stride) << " gamma=" << with_gamma << " layout=" << expose_layout;
        EXPECT_EQ(actual.effect_data(), expected.effect_data());
      }
    }
  }
}

// LEDs outside [0, size()) are skipped, and color_at() is indexed from `start` either way.
TEST(AddressableWritePixels, ClipsToStrip) {
  for (bool expose_layout : {false, true}) {
    TestStrip strip(10, 3, {0, 1, 2, ChannelColors::NO_WHITE}, expose_layout);
    std::vector<int32_t> seen;
    auto color_at = [&seen](int32_t i) {
      seen.push_back(i);
      return Color(static_cast<uint8_t>(100 + i), 0, 0);
    };

    strip.write_pixels(-2, 5, color_at);  // LEDs 0..2 from i = 2..4
    EXPECT_EQ(seen, (std::vector<int32_t>{2, 3, 4}));
    EXPECT_EQ(strip[0].get_red_raw(), 102);
    EXPECT_EQ(strip[2].get_red_raw(), 104);
    EXPECT_EQ(strip[3].get_red_raw(), 0xEE);

    seen.clear();
    strip.write_pixels(8, 5, color_at);  // LEDs 8..9 from i = 0..1
    EXPECT_EQ(seen, (std::vector<int32_t>{0, 1}));
    EXPECT_EQ(strip[7].get_red_raw(), 0xEE);
    EXPECT_EQ(strip[9].get_red_raw(), 101);

    seen.clear();
    strip.write_pixels(10, 5, color_at);
    strip.write_pixels(3, 0, color_at);
    strip.write_pixels(3, -4, color_at);
    strip.write_pixels(-20, 5, color_at);
    EXPECT_TRUE(seen.empty());
  }
}

// fill_pixels() matches per-LED set() on both paths, and clips like write_pixels().
TEST(AddressableWritePixels, FillMatchesPerLedSet) {
  constexpr int32_t num_leds = 40;
  const auto gamma = build_gamma_table(2.8);
  const Color color(200, 37, 1, 128);
  for (const Layout &layout : LAYOUTS) {
    for (bool with_gamma : {false, true}) {
      for (bool expose_layout : {false, true}) {
        TestStrip expected(num_leds, layout.stride, layout.colors, false);
        TestStrip actual(num_leds, layout.stride, layout.colors, expose_layout);
        for (TestStrip *strip : {&expected, &actual}) {
          strip->set_correction(0.9f, 0.6f, 0.3f, 0.75f);
          strip->set_local_brightness(173);
          strip->set_gamma(with_gamma ? gamma.data() : nullptr);
        }
        for (int32_t i = 0; i < 7; i++)
          expected[i].set(color);
        for (int32_t i = num_leds - 2; i < num_leds; i++)
          expected[i].set(color);
        actual.fill_pixels(-3, 10, color);           // LEDs 0..6
        actual.fill_pixels(num_leds - 2, 5, color);  // LEDs 38..39
        actual.fill_pixels(num_leds, 5, color);
        actual.fill_pixels(10, 0, color);
        actual.fill_pixels(10, -5, color);
        actual.fill_pixels(-20, 5, color);

        EXPECT_EQ(actual.buffer(), expected.buffer())
            << "stride=" << int(layout.stride) << " gamma=" << with_gamma << " layout=" << expose_layout;
        EXPECT_EQ(actual.effect_data(), expected.effect_data());
      }
    }
  }
}

// range().set() / all() = color go through fill_pixels().
TEST(AddressableWritePixels, RangeSetMatchesPerLedSet) {
  const auto gamma = build_gamma_table(2.2);
  TestStrip expected(20, 4, {1, 0, 2, 3}, false);
  TestStrip actual(20, 4, {1, 0, 2, 3}, true);
  const Color color(200, 100, 50, 25);
  for (TestStrip *strip : {&expected, &actual}) {
    strip->set_correction(0.8f, 1.0f, 0.5f, 1.0f);
    strip->set_gamma(gamma.data());
  }
  for (int32_t i = 5; i < 15; i++)
    expected[i].set(color);
  actual.range(5, 15) = color;
  EXPECT_EQ(actual.buffer(), expected.buffer());
}

// Snapshot folds the brightness factors together; it must match color_correct_*() for every input.
TEST(ESPColorCorrectionSnapshot, MatchesColorCorrect) {
  const auto gamma = build_gamma_table(2.8);
  for (const uint16_t *table : {static_cast<const uint16_t *>(nullptr), gamma.data()}) {
    for (uint8_t local : {0, 1, 128, 254, 255}) {
      ESPColorCorrection correction;
      correction.set_gamma_table(table);
      correction.set_max_brightness(Color(255, 200, 1, 0));
      correction.set_local_brightness(local);
      const ESPColorCorrection::Snapshot snapshot = correction.snapshot();
      for (int v = 0; v < 256; v++) {
        const uint8_t value = v;
        ASSERT_EQ(snapshot.red(value), correction.color_correct_red(value)) << v;
        ASSERT_EQ(snapshot.green(value), correction.color_correct_green(value)) << v;
        ASSERT_EQ(snapshot.blue(value), correction.color_correct_blue(value)) << v;
        ASSERT_EQ(snapshot.white(value), correction.color_correct_white(value)) << v;
      }
    }
  }
}

// The division-free rounding must equal the (t + 128) / 257 it replaced for every table value.
TEST(GammaTableCorrect, MatchesDivisionForEveryTableValue) {
  uint16_t table[256] = {};
  for (uint32_t t = 0; t <= 0xFFFF; t++) {
    table[0] = static_cast<uint16_t>(t);
    uint8_t expected = static_cast<uint8_t>((t + 128) / 257);
    if (expected == 0 && t != 0)
      expected = 1;
    ASSERT_EQ(gamma_table_correct(table, 0), expected) << t;
  }
}

}  // namespace esphome::light::testing
