#include <gtest/gtest.h>

#include "esphome/core/defines.h"

#ifdef USE_HOST

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <memory>
#include <vector>

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

// E1.31 data packet for `universe` with `dmx_bytes` slots of `value`; `count_override`
// replaces the property value count to build malformed packets.
static std::vector<uint8_t> e131_packet(uint16_t universe, uint16_t dmx_bytes, uint8_t value, int count_override = -1) {
  static const uint8_t ACN_ID[12] = {0x41, 0x53, 0x43, 0x2d, 0x45, 0x31, 0x2e, 0x31, 0x37, 0x00, 0x00, 0x00};
  std::vector<uint8_t> buf(126 + dmx_bytes, 0);
  buf[1] = 0x10;
  memcpy(&buf[4], ACN_ID, sizeof(ACN_ID));
  buf[21] = 0x04;  // VECTOR_ROOT_E131_DATA
  buf[43] = 0x02;  // VECTOR_E131_DATA_PACKET
  buf[113] = universe >> 8;
  buf[114] = universe & 0xff;
  buf[117] = 0x02;  // VECTOR_DMP_SET_PROPERTY
  buf[118] = 0xa1;
  buf[122] = 0x01;
  const uint16_t count = count_override >= 0 ? count_override : dmx_bytes + 1;
  buf[123] = count >> 8;
  buf[124] = count & 0xff;
  memset(&buf[126], value, dmx_bytes);
  return buf;
}

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

// --- Loop only runs while an effect is active ---

TEST_F(E131Test, LoopDisabledWhileNoEffectActive) {
  E131Component e131;
  e131.setup();
  EXPECT_TRUE(e131.is_idle());

  Strip a(&e131, 1, 1);
  Strip b(&e131, 2, 1);
  a.start();
  EXPECT_TRUE(e131.is_in_loop_state());
  b.start();
  a.stop();
  EXPECT_TRUE(e131.is_in_loop_state());  // b still active
  b.stop();
  EXPECT_TRUE(e131.is_idle());
}

// --- Receive path ---

class E131ReceiveTest : public E131Test {
 protected:
  void SetUp() override {
    E131Test::SetUp();
    this->e131_.set_method(E131_UNICAST);
    this->e131_.setup();
    this->tx_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    this->dest_.sin_family = AF_INET;
    this->dest_.sin_port = htons(5568);
    this->dest_.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  }
  void TearDown() override { ::close(this->tx_); }
  void send(const std::vector<uint8_t> &packet) {
    ::sendto(this->tx_, packet.data(), packet.size(), 0, reinterpret_cast<const sockaddr *>(&this->dest_),
             sizeof(this->dest_));
  }

  E131Component e131_;
  int tx_{-1};
  sockaddr_in dest_{};
};

TEST_F(E131ReceiveTest, AppliesAllUniversesOfAFrame) {
  Strip strip(&this->e131_, 1, 2);
  strip.start();

  this->send(e131_packet(1, RGB_LIGHTS_PER_UNIVERSE * 3, 0x11));
  this->send(e131_packet(2, RGB_LIGHTS_PER_UNIVERSE * 3, 0x22));
  this->e131_.loop();

  EXPECT_EQ(strip.output.red(0), 0x11);
  EXPECT_EQ(strip.output.red(RGB_LIGHTS_PER_UNIVERSE - 1), 0x11);
  EXPECT_EQ(strip.output.red(RGB_LIGHTS_PER_UNIVERSE), 0x22);
  EXPECT_EQ(strip.output.red(2 * RGB_LIGHTS_PER_UNIVERSE - 1), 0x22);
}

TEST_F(E131ReceiveTest, RejectsCountBeyondReceivedData) {
  Strip strip(&this->e131_, 1, 1);
  strip.start();

  // Claims 513 values but carries only 30
  this->send(e131_packet(1, 30, 0x33, E131_MAX_PROPERTY_VALUES_COUNT));
  this->e131_.loop();
  EXPECT_EQ(strip.output.red(0), 0);
}

TEST_F(E131ReceiveTest, OversizedDatagramIsTruncatedSafely) {
  Strip strip(&this->e131_, 1, 1);
  strip.start();

  // A valid full universe followed by trailing bytes beyond the largest E1.31 packet
  auto packet = e131_packet(1, 512, 0x44);
  packet.resize(1200, 0xEE);
  this->send(packet);
  this->e131_.loop();
  EXPECT_EQ(strip.output.red(0), 0x44);
  EXPECT_EQ(strip.output.red(RGB_LIGHTS_PER_UNIVERSE - 1), 0x44);
}

}  // namespace esphome::e131::testing

#endif  // USE_HOST
