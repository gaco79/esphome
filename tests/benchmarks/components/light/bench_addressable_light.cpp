#include <benchmark/benchmark.h>

#include <cstring>
#include <memory>

#include "esphome/components/light/addressable_light.h"
#include "esphome/components/light/light_state.h"

extern const esphome::light::GammaTable bench_gamma_2_8;

namespace esphome::benchmarks {

// One frame = 4 E1.31 universes of 170 RGB pixels.
static constexpr int32_t kNumLeds = 680;
// Frames written per benchmark iteration, to amortize CodSpeed instrumentation overhead.
static constexpr int kFrames = 20;

// In-memory addressable light laid out like esp32_rmt_led_strip: a packed byte buffer
// in wire order (GRB / GRBW) with no white channel for RGB strips.
class BenchStripLight : public light::AddressableLight {
 public:
  BenchStripLight(int32_t num_leds, bool rgbw)
      : num_leds_(num_leds),
        bpl_(rgbw ? 4 : 3),
        buf_(new uint8_t[num_leds * (rgbw ? 4 : 3)]()),
        effect_data_(new uint8_t[num_leds]()) {}

  void setup() override {}
  void write_state(light::LightState * /*state*/) override {}
  int32_t size() const override { return this->num_leds_; }
  void clear_effect_data() override { memset(this->effect_data_.get(), 0, this->num_leds_); }
  light::LightTraits get_traits() override {
    light::LightTraits traits;
    traits.set_supported_color_modes({this->bpl_ == 4 ? light::ColorMode::RGB_WHITE : light::ColorMode::RGB});
    return traits;
  }
  const uint8_t *buffer() const { return this->buf_.get(); }

  void set_local_brightness(uint8_t brightness) { this->correction_.set_local_brightness(brightness); }
  // FNV-1a over the output buffer, so different write paths can be checked for identical results.
  uint32_t checksum() const {
    uint32_t h = 2166136261u;
    for (int32_t i = 0; i < this->num_leds_ * this->bpl_; i++)
      h = (h ^ this->buf_[i]) * 16777619u;
    return h;
  }

  // When false, write_pixels() takes its fallback path through get_view_internal().
  bool expose_layout{true};

 protected:
  bool get_pixel_buffer_layout(light::PixelBufferLayout &layout) const override {
    if (!this->expose_layout)
      return false;
    layout = {this->buf_.get(),
              static_cast<uint8_t>(this->bpl_),
              {1, 0, 2, this->bpl_ == 4 ? uint8_t(3) : light::ChannelColors::NO_WHITE}};
    return true;
  }
  light::ESPColorView get_view_internal(int32_t index) const override {
    uint8_t *led = this->buf_.get() + index * this->bpl_;
    return {led + 1,           led + 0, led + 2, this->bpl_ == 4 ? led + 3 : nullptr, this->effect_data_.get() + index,
            &this->correction_};
  }

  int32_t num_leds_;
  int32_t bpl_;
  std::unique_ptr<uint8_t[]> buf_;
  std::unique_ptr<uint8_t[]> effect_data_;
};

struct StripRig {
  StripRig(bool gamma, bool rgbw) : light(kNumLeds, rgbw), state(&light) {
    if (gamma)
      state.set_gamma_table(&bench_gamma_2_8);
    light.setup_state(&state);
    // Non-identity brightness so the scaling maths is exercised.
    light.set_correction(0.9f, 0.8f, 0.7f, 0.6f);
    light.set_local_brightness(200);
    for (int i = 0; i < kNumLeds * 3; i++)
      input[i] = static_cast<uint8_t>(i * 7);
  }
  BenchStripLight light;
  light::LightState state;
  uint8_t input[kNumLeds * 3];
};

// Args: {gamma table set (0/1), RGBW strip (0/1)}
#define STRIP_ARGS ->Args({0, 0})->Args({1, 0})->Args({1, 1})

// --- Per-LED writes through operator[]: (*it)[i].set(Color(...)) ---
static void AddressableLight_Write_Indexed(benchmark::State &state) {
  StripRig rig(state.range(0), state.range(1));
  light::AddressableLight *it = &rig.light;
  for (auto _ : state) {
    for (int f = 0; f < kFrames; f++) {
      const uint8_t *d = rig.input;
      for (int32_t i = 0; i < kNumLeds; i++, d += 3)
        (*it)[i].set(Color(d[0], d[1], d[2], (d[0] + d[1] + d[2]) / 3));
    }
    benchmark::DoNotOptimize(rig.light.buffer());
  }
  state.counters["crc"] = rig.light.checksum();
  state.SetItemsProcessed(state.iterations() * kFrames * kNumLeds);
}
BENCHMARK(AddressableLight_Write_Indexed) STRIP_ARGS;

// --- Same, but with set_rgb() (skips the white channel entirely) ---
static void AddressableLight_Write_IndexedRGB(benchmark::State &state) {
  StripRig rig(state.range(0), state.range(1));
  light::AddressableLight *it = &rig.light;
  for (auto _ : state) {
    for (int f = 0; f < kFrames; f++) {
      const uint8_t *d = rig.input;
      for (int32_t i = 0; i < kNumLeds; i++, d += 3)
        (*it)[i].set_rgb(d[0], d[1], d[2]);
    }
    benchmark::DoNotOptimize(rig.light.buffer());
  }
  state.counters["crc"] = rig.light.checksum();
  state.SetItemsProcessed(state.iterations() * kFrames * kNumLeds);
}
BENCHMARK(AddressableLight_Write_IndexedRGB) STRIP_ARGS;

// --- Range iterator: for (auto view : *it) ---
static void AddressableLight_Write_Iterator(benchmark::State &state) {
  StripRig rig(state.range(0), state.range(1));
  light::AddressableLight *it = &rig.light;
  for (auto _ : state) {
    for (int f = 0; f < kFrames; f++) {
      const uint8_t *d = rig.input;
      for (auto view : *it) {
        view.set(Color(d[0], d[1], d[2], (d[0] + d[1] + d[2]) / 3));
        d += 3;
      }
    }
    benchmark::DoNotOptimize(rig.light.buffer());
  }
  state.counters["crc"] = rig.light.checksum();
  state.SetItemsProcessed(state.iterations() * kFrames * kNumLeds);
}
BENCHMARK(AddressableLight_Write_Iterator) STRIP_ARGS;

// --- Uniform fill: it.all() = color (update_state(), effects); goes through fill_pixels() ---
static void AddressableLight_Write_All(benchmark::State &state) {
  StripRig rig(state.range(0), state.range(1));
  light::AddressableLight *it = &rig.light;
  for (auto _ : state) {
    for (int f = 0; f < kFrames; f++)
      it->all() = Color(f, 2 * f, 3 * f, 0);
    benchmark::DoNotOptimize(rig.light.buffer());
  }
  state.counters["crc"] = rig.light.checksum();
  state.SetItemsProcessed(state.iterations() * kFrames * kNumLeds);
}
BENCHMARK(AddressableLight_Write_All) STRIP_ARGS;

// --- Bulk write: write_pixels(), as e131/wled/adalight use it ---
// Third arg: the output describes its buffer with get_pixel_buffer_layout() (1) or not (0).
static void AddressableLight_Write_Bulk(benchmark::State &state) {
  StripRig rig(state.range(0), state.range(1));
  rig.light.expose_layout = state.range(2);
  light::AddressableLight *it = &rig.light;
  for (auto _ : state) {
    for (int f = 0; f < kFrames; f++) {
      const uint8_t *in = rig.input;
      it->write_pixels(0, kNumLeds, [in](int32_t i) {
        const uint8_t *d = in + 3 * i;
        return Color(d[0], d[1], d[2], (d[0] + d[1] + d[2]) / 3);
      });
    }
    benchmark::DoNotOptimize(rig.light.buffer());
  }
  state.counters["crc"] = rig.light.checksum();
  state.SetItemsProcessed(state.iterations() * kFrames * kNumLeds);
}
BENCHMARK(AddressableLight_Write_Bulk)
    ->Args({0, 0, 0})
    ->Args({1, 0, 0})
    ->Args({1, 1, 0})
    ->Args({0, 0, 1})
    ->Args({1, 0, 1})
    ->Args({1, 1, 1});

}  // namespace esphome::benchmarks
