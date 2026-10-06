#pragma once

#include <algorithm>

#include "channel_colors.h"
#include "esp_color_correction.h"
#include "esp_color_view.h"
#include "esp_range_view.h"
#include "esphome/core/color.h"
#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "light_output.h"
#include "light_state.h"
#include "light_transformer.h"

#ifdef USE_POWER_SUPPLY
#include "esphome/components/power_supply/power_supply.h"
#endif

namespace esphome::light {

/// Convert the color information from a `LightColorValues` object to a `Color` object (does not apply brightness).
Color color_from_light_color_values(LightColorValues val);

/// Use a custom state class for addressable lights, to allow type system to discriminate between addressable and
/// non-addressable lights.
class AddressableLightState final : public LightState {
  using LightState::LightState;
};

/// Where an output keeps its LEDs' bytes, see AddressableLight::get_pixel_buffer_layout().
struct PixelBufferLayout {
  /// First byte of LED 0.
  uint8_t *data;
  /// Bytes from the start of one LED to the start of the next.
  uint8_t stride;
  /// Offset of each colour within an LED's bytes.
  ChannelColors colors;
};

class AddressableLight : public LightOutput, public Component {
 public:
  /// Set `count` LEDs starting at `start`, with the same result as `(*this)[start + i].set(color_at(i))`
  /// for each i in [0, count). `color_at(i)` returns the uncorrected colour; its white value is ignored by
  /// strips without a white channel. LEDs outside [0, size()) are skipped.
  ///
  /// Use this to write runs of LEDs, for example from a network frame. It avoids the per-LED virtual
  /// calls of operator[], and when the output describes its buffer with get_pixel_buffer_layout() it
  /// writes the corrected bytes in a single tight loop.
  template<typename F> void write_pixels(int32_t start, int32_t count, F &&color_at) {
    const int32_t first = start < 0 ? -start : 0;
    const int32_t last = std::min(count, this->size() - start);
    if (first >= last)
      return;

    PixelBufferLayout layout;
    if (!this->get_pixel_buffer_layout(layout)) {
      for (int32_t i = first; i < last; i++) {
        const Color color = color_at(i);
        this->get_view_internal(start + i).set_rgbw(color.r, color.g, color.b, color.w);
      }
      return;
    }

    const ESPColorCorrection::Snapshot correction = this->correction_.snapshot();
    const ChannelColors colors = layout.colors;
    const uint8_t stride = layout.stride;
    uint8_t *led = layout.data + (start + first) * stride;
    // Separate loops so a strip without white never computes the white value.
    if (colors.has_white()) {
      for (int32_t i = first; i < last; i++, led += stride) {
        const Color color = color_at(i);
        led[colors.r] = correction.red(color.r);
        led[colors.g] = correction.green(color.g);
        led[colors.b] = correction.blue(color.b);
        led[colors.w] = correction.white(color.w);
      }
    } else {
      for (int32_t i = first; i < last; i++, led += stride) {
        const Color color = color_at(i);
        led[colors.r] = correction.red(color.r);
        led[colors.g] = correction.green(color.g);
        led[colors.b] = correction.blue(color.b);
      }
    }
  }

  /// Set `count` LEDs starting at `start` to `color`, with the same result as write_pixels() with a
  /// constant colour, but correcting the colour only once. LEDs outside [0, size()) are skipped.
  void fill_pixels(int32_t start, int32_t count, const Color &color);

  virtual int32_t size() const = 0;
  ESPColorView operator[](int32_t index) const { return this->get_view_internal(interpret_index(index, this->size())); }
  ESPColorView get(int32_t index) { return this->get_view_internal(interpret_index(index, this->size())); }
  virtual void clear_effect_data() = 0;
  ESPRangeView range(int32_t from, int32_t to) {
    from = interpret_index(from, this->size());
    to = interpret_index(to, this->size());
    return ESPRangeView(this, from, to);
  }
  ESPRangeView all() { return ESPRangeView(this, 0, this->size()); }
  ESPRangeIterator begin() { return this->all().begin(); }
  ESPRangeIterator end() { return this->all().end(); }
  void shift_left(int32_t amnt) {
    if (amnt < 0) {
      this->shift_right(-amnt);
      return;
    }
    if (amnt > this->size())
      amnt = this->size();
    this->range(0, -amnt) = this->range(amnt, this->size());
  }
  void shift_right(int32_t amnt) {
    if (amnt < 0) {
      this->shift_left(-amnt);
      return;
    }
    if (amnt > this->size())
      amnt = this->size();
    this->range(amnt, this->size()) = this->range(0, -amnt);
  }
  // Indicates whether an effect that directly updates the output buffer is active to prevent overwriting
  bool is_effect_active() const { return this->effect_active_; }
  void set_effect_active(bool effect_active) { this->effect_active_ = effect_active; }
  std::unique_ptr<LightTransformer> create_default_transition() override;
  void set_correction(float red, float green, float blue, float white = 1.0f) {
    this->correction_.set_max_brightness(
        Color(to_uint8_scale(red), to_uint8_scale(green), to_uint8_scale(blue), to_uint8_scale(white)));
  }
  void setup_state(LightState *state) override {
#ifdef USE_LIGHT_GAMMA_LUT
    this->correction_.set_gamma_table(state->get_gamma_table());
#endif
    this->state_parent_ = state;
  }
  void update_state(LightState *state) override;
  void schedule_show() { this->state_parent_->schedule_write_(); }

#ifdef USE_POWER_SUPPLY
  void set_power_supply(power_supply::PowerSupply *power_supply) { this->power_.set_parent(power_supply); }
#endif

  void call_setup() override;

 protected:
  friend class AddressableLightTransformer;

  void mark_shown_() {
#ifdef USE_POWER_SUPPLY
    for (const auto &c : *this) {
      if (c.get_red_raw() > 0 || c.get_green_raw() > 0 || c.get_blue_raw() > 0 || c.get_white_raw() > 0) {
        this->power_.request();
        return;
      }
    }
    this->power_.unrequest();
#endif
  }
  virtual ESPColorView get_view_internal(int32_t index) const = 0;
  /// Outputs that keep their LEDs in one packed byte buffer can describe it here, so write_pixels()
  /// and fill_pixels() can write it directly instead of going through get_view_internal() for every
  /// LED. Returning a layout promises that LED i is at `data + i * stride`, using this light's own
  /// correction_; a subclass that remaps indices or corrections in get_view_internal() must return
  /// false. Return false (the default) when there is no such buffer, or it isn't allocated yet.
  virtual bool get_pixel_buffer_layout(PixelBufferLayout & /*layout*/) const { return false; }

  ESPColorCorrection correction_{};
  LightState *state_parent_{nullptr};
#ifdef USE_POWER_SUPPLY
  power_supply::PowerSupplyRequester power_;
#endif
  bool effect_active_{false};
};

class AddressableLightTransformer : public LightTransformer {
 public:
  AddressableLightTransformer(AddressableLight &light) : light_(light) {}

  void start() override;
  optional<LightColorValues> apply() override;

 protected:
  AddressableLight &light_;
  float last_transition_progress_{0.0f};
  Color target_color_{};
  Color uniform_start_color_{};
  bool uniform_start_scanned_{false};
  bool uniform_start_is_uniform_{false};
};

}  // namespace esphome::light
