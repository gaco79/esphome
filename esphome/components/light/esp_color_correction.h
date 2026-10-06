#pragma once

#include "esphome/core/color.h"
#include "esphome/core/hal.h"

namespace esphome::light {

/// A gamma curve as codegen emits it into PROGMEM: the lookup table and the gamma it was built from
struct GammaTable {
  uint16_t lut[256];
  uint16_t gamma_x100;
};

/// Binary search a monotonically increasing uint16[256] PROGMEM table.
/// Returns the largest index where table[index] <= target.
inline uint8_t gamma_table_reverse_search(const uint16_t *table, uint16_t target) {
  uint8_t lo = 0, hi = 255;
  while (lo < hi) {
    uint8_t mid = (lo + hi + 1) / 2;
    if (progmem_read_uint16(&table[mid]) <= target) {
      lo = mid;
    } else {
      hi = mid - 1;
    }
  }
  return lo;
}

/// Look up `value` in a uint16[256] PROGMEM gamma table and round the result to 8 bits.
/// A non-zero table entry never rounds down to 0. A null table returns `value` unchanged.
inline uint8_t ESPHOME_ALWAYS_INLINE gamma_table_correct(const uint16_t *table, uint8_t value) {
  if (table == nullptr)
    return value;
  uint16_t table_value = progmem_read_uint16(&table[value]);
  // (x - (x >> 8)) >> 8 equals x / 257 for every x up to 65535 + 128, without a division
  // (ESP8266 has no divide instruction and would call __divsi3 for every channel).
  uint32_t x = uint32_t(table_value) + 128;
  uint8_t result = (x - (x >> 8)) >> 8;
  if (result == 0 && table_value != 0)
    return 1;
  return result;
}

class ESPColorCorrection {
 public:
  /// The correction captured in local values, for loops that correct many pixels.
  ///
  /// Gives the same results as color_correct_*(). Each channel's two brightness factors are
  /// multiplied together up front. Holding them by value also stops writes to a uint8_t pixel
  /// buffer, which the compiler must assume can alias any object, from forcing a reload of the
  /// correction for every pixel.
  struct Snapshot {
    inline uint8_t ESPHOME_ALWAYS_INLINE red(uint8_t value) const { return this->apply_(value, this->red_factor); }
    inline uint8_t ESPHOME_ALWAYS_INLINE green(uint8_t value) const { return this->apply_(value, this->green_factor); }
    inline uint8_t ESPHOME_ALWAYS_INLINE blue(uint8_t value) const { return this->apply_(value, this->blue_factor); }
    inline uint8_t ESPHOME_ALWAYS_INLINE white(uint8_t value) const { return this->apply_(value, this->white_factor); }

    const uint16_t *gamma_table;
    uint32_t red_factor;
    uint32_t green_factor;
    uint32_t blue_factor;
    uint32_t white_factor;

   protected:
    inline uint8_t ESPHOME_ALWAYS_INLINE apply_(uint8_t value, uint32_t factor) const {
      // Same as esp_scale8_twice(): (value * (1 + max) * (1 + local)) >> 16
      return gamma_table_correct(this->gamma_table, (uint32_t(value) * factor) >> 16);
    }
  };
  Snapshot snapshot() const {
    const uint32_t local = 1 + uint32_t(this->local_brightness_);
    return {this->gamma_table_, (1 + uint32_t(this->max_brightness_.red)) * local,
            (1 + uint32_t(this->max_brightness_.green)) * local, (1 + uint32_t(this->max_brightness_.blue)) * local,
            (1 + uint32_t(this->max_brightness_.white)) * local};
  }

  void set_max_brightness(const Color &max_brightness) { this->max_brightness_ = max_brightness; }
  void set_local_brightness(uint8_t local_brightness) { this->local_brightness_ = local_brightness; }
  void set_gamma_table(const uint16_t *table) { this->gamma_table_ = table; }
  inline Color color_correct(Color color) const ESPHOME_ALWAYS_INLINE {
    // corrected = (uncorrected * max_brightness * local_brightness) ^ gamma
    return Color(this->color_correct_red(color.red), this->color_correct_green(color.green),
                 this->color_correct_blue(color.blue), this->color_correct_white(color.white));
  }
  inline uint8_t color_correct_red(uint8_t red) const ESPHOME_ALWAYS_INLINE {
    uint8_t res = esp_scale8_twice(red, this->max_brightness_.red, this->local_brightness_);
    return this->gamma_correct_(res);
  }
  inline uint8_t color_correct_green(uint8_t green) const ESPHOME_ALWAYS_INLINE {
    uint8_t res = esp_scale8_twice(green, this->max_brightness_.green, this->local_brightness_);
    return this->gamma_correct_(res);
  }
  inline uint8_t color_correct_blue(uint8_t blue) const ESPHOME_ALWAYS_INLINE {
    uint8_t res = esp_scale8_twice(blue, this->max_brightness_.blue, this->local_brightness_);
    return this->gamma_correct_(res);
  }
  inline uint8_t color_correct_white(uint8_t white) const ESPHOME_ALWAYS_INLINE {
    uint8_t res = esp_scale8_twice(white, this->max_brightness_.white, this->local_brightness_);
    return this->gamma_correct_(res);
  }
  Color color_uncorrect(Color color) const;
  inline uint8_t color_uncorrect_red(uint8_t red) const ESPHOME_ALWAYS_INLINE {
    return this->color_uncorrect_channel_(red, this->max_brightness_.red);
  }
  inline uint8_t color_uncorrect_green(uint8_t green) const ESPHOME_ALWAYS_INLINE {
    return this->color_uncorrect_channel_(green, this->max_brightness_.green);
  }
  inline uint8_t color_uncorrect_blue(uint8_t blue) const ESPHOME_ALWAYS_INLINE {
    return this->color_uncorrect_channel_(blue, this->max_brightness_.blue);
  }
  inline uint8_t color_uncorrect_white(uint8_t white) const ESPHOME_ALWAYS_INLINE {
    return this->color_uncorrect_channel_(white, this->max_brightness_.white);
  }

 protected:
  /// Forward gamma: read uint16 PROGMEM table, convert to uint8
  inline uint8_t gamma_correct_(uint8_t value) const ESPHOME_ALWAYS_INLINE {
    return gamma_table_correct(this->gamma_table_, value);
  }
  /// Reverse gamma: binary search the forward PROGMEM table
  uint8_t gamma_uncorrect_(uint8_t value) const;
  /// Shared body of color_uncorrect_{red,green,blue,white}. Kept out-of-line
  /// to avoid duplicating two 16-bit divides at every call site.
  uint8_t color_uncorrect_channel_(uint8_t value, uint8_t max_brightness) const;

  const uint16_t *gamma_table_{nullptr};
  Color max_brightness_{255, 255, 255, 255};
  uint8_t local_brightness_{255};
};

}  // namespace esphome::light
