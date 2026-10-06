#include <gtest/gtest.h>

#include "esphome/core/defines.h"

#ifdef USE_HOST

#include <cstring>
#include <memory>

#include <lwip/igmp.h>

#include "esphome/components/e131/e131.h"
#include "esphome/components/e131/e131_addressable_light_effect.h"
#include "esphome/components/light/addressable_light.h"
#include "esphome/components/light/light_state.h"

namespace esphome::e131::testing {

static constexpr int RGB_LIGHTS_PER_UNIVERSE = 170;

// In-memory addressable light: stores RGBW bytes, no hardware.
class TestAddressableLight : public light::AddressableLight {
 public:
  explicit TestAddressableLight(int32_t num_leds)
      : num_leds_(num_leds), buf_(new uint8_t[num_leds * 4]()), effect_data_(new uint8_t[num_leds]()) {}

  void setup() override {}
  void write_state(light::LightState * /*state*/) override {}
  int32_t size() const override { return this->num_leds_; }
  void clear_effect_data() override { memset(this->effect_data_.get(), 0, this->num_leds_); }
  light::LightTraits get_traits() override {
    light::LightTraits traits;
    traits.set_supported_color_modes({light::ColorMode::RGB});
    return traits;
  }
  uint8_t red(int32_t index) const { return this->buf_[index * 4]; }

 protected:
  light::ESPColorView get_view_internal(int32_t index) const override {
    uint8_t *p = this->buf_.get() + index * 4;
    return {p, p + 1, p + 2, p + 3, this->effect_data_.get() + index, &this->correction_};
  }

  int32_t num_leds_;
  std::unique_ptr<uint8_t[]> buf_;
  std::unique_ptr<uint8_t[]> effect_data_;
};

// A light strip driven by one E1.31 effect.
struct Strip {
  Strip(E131Component *e131, int first_universe, int universes)
      : output(universes * RGB_LIGHTS_PER_UNIVERSE), state(&output), effect("e131") {
    this->output.setup_state(&this->state);
    this->effect.set_first_universe(first_universe);
    this->effect.set_channels(E131_RGB);
    this->effect.set_e131(e131);
    this->effect.init_internal(&this->state);
  }
  void start() { this->effect.start_internal(); }
  void stop() { this->effect.stop(); }

  TestAddressableLight output;
  light::LightState state;
  E131AddressableLightEffect effect;
};

class E131Test : public ::testing::Test {
 protected:
  void SetUp() override { lwip_stub::reset(); }
};

// --- IGMP membership ---

TEST_F(E131Test, StartStopJoinsEachUniverseOnceAndLeavesAll) {
  E131Component e131;
  e131.setup();
  Strip strip(&e131, 1, 4);

  for (int cycle = 0; cycle < 3; cycle++) {
    lwip_stub::igmp_join_calls = 0;
    strip.start();
    EXPECT_EQ(lwip_stub::igmp_join_calls, 4u);
    for (uint16_t u = 1; u <= 4; u++)
      EXPECT_EQ(lwip_stub::e131_group_use(u), 1) << "universe " << u;

    strip.stop();
    EXPECT_TRUE(lwip_stub::igmp_groups.empty()) << "cycle " << cycle;
  }
}

TEST_F(E131Test, OverlappingEffectsShareMembership) {
  E131Component e131;
  e131.setup();
  Strip a(&e131, 1, 4);  // universes 1-4
  Strip b(&e131, 3, 2);  // universes 3-4

  a.start();
  b.start();
  for (uint16_t u = 1; u <= 4; u++)
    EXPECT_EQ(lwip_stub::e131_group_use(u), 1) << "universe " << u;

  a.stop();
  EXPECT_EQ(lwip_stub::e131_group_use(1), 0);
  EXPECT_EQ(lwip_stub::e131_group_use(2), 0);
  EXPECT_EQ(lwip_stub::e131_group_use(3), 1);
  EXPECT_EQ(lwip_stub::e131_group_use(4), 1);

  b.stop();
  EXPECT_TRUE(lwip_stub::igmp_groups.empty());
}

TEST_F(E131Test, EffectStartedBeforeSetupIsJoinedOnceBySetup) {
  E131Component e131;
  Strip strip(&e131, 1, 2);

  strip.start();
  EXPECT_EQ(lwip_stub::igmp_join_calls, 0u);  // no socket yet

  e131.setup();
  EXPECT_EQ(lwip_stub::e131_group_use(1), 1);
  EXPECT_EQ(lwip_stub::e131_group_use(2), 1);

  strip.stop();
  EXPECT_TRUE(lwip_stub::igmp_groups.empty());
}

TEST_F(E131Test, UnicastNeverJoins) {
  E131Component e131;
  e131.set_method(E131_UNICAST);
  e131.setup();
  Strip strip(&e131, 1, 2);

  strip.start();
  strip.stop();
  EXPECT_EQ(lwip_stub::igmp_join_calls, 0u);
  EXPECT_EQ(lwip_stub::igmp_leave_calls, 0u);
}

}  // namespace esphome::e131::testing

#endif  // USE_HOST
