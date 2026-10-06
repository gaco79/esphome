#include <benchmark/benchmark.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <memory>

#include <lwip/igmp.h>

#include "esphome/components/e131/e131.h"
#include "esphome/components/e131/e131_addressable_light_effect.h"
#include "esphome/components/light/addressable_light.h"
#include "esphome/components/light/light_state.h"

namespace esphome::benchmarks {

static constexpr uint16_t E131_PORT = 5568;
static constexpr int RGB_LIGHTS_PER_UNIVERSE = 170;

// In-memory addressable light: stores RGBW bytes, no hardware.
class BenchAddressableLight : public light::AddressableLight {
 public:
  explicit BenchAddressableLight(int32_t num_leds)
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
  uint8_t raw(int32_t index, int channel) const { return this->buf_[index * 4 + channel]; }

 protected:
  light::ESPColorView get_view_internal(int32_t index) const override {
    uint8_t *p = this->buf_.get() + index * 4;
    return {p, p + 1, p + 2, p + 3, this->effect_data_.get() + index, &this->correction_};
  }

  int32_t num_leds_;
  std::unique_ptr<uint8_t[]> buf_;
  std::unique_ptr<uint8_t[]> effect_data_;
};

// Writes an E1.31 data packet (ANSI E1.31-2018) for `universe` carrying `dmx_bytes`
// slots after the start code. Returns the packet length.
static size_t build_e131_packet(uint8_t *buf, uint16_t universe, uint16_t dmx_bytes, uint8_t seed) {
  static const uint8_t ACN_ID[12] = {0x41, 0x53, 0x43, 0x2d, 0x45, 0x31, 0x2e, 0x31, 0x37, 0x00, 0x00, 0x00};
  const size_t len = 126 + dmx_bytes;
  memset(buf, 0, len);
  buf[1] = 0x10;                // preamble size
  memcpy(buf + 4, ACN_ID, 12);  // ACN packet identifier
  buf[21] = 0x04;               // root vector: VECTOR_ROOT_E131_DATA
  buf[43] = 0x02;               // frame vector: VECTOR_E131_DATA_PACKET
  buf[113] = universe >> 8;
  buf[114] = universe & 0xff;
  buf[117] = 0x02;  // DMP vector: VECTOR_DMP_SET_PROPERTY
  buf[118] = 0xa1;  // address & data type
  buf[122] = 0x01;  // address increment
  const uint16_t count = dmx_bytes + 1;
  buf[123] = count >> 8;
  buf[124] = count & 0xff;
  buf[125] = 0x00;  // DMX start code
  for (uint16_t i = 0; i < dmx_bytes; i++)
    buf[126 + i] = static_cast<uint8_t>(seed + i);
  return len;
}

// A light with one E1.31 effect attached to an E131Component bound on the real port,
// plus a UDP sender on loopback.
class E131Rig {
 public:
  E131Rig(int universes, e131::E131ListenMethod method)
      : output_(universes * RGB_LIGHTS_PER_UNIVERSE), state_(&output_), effect_("e131") {
    lwip_stub::reset();
    this->output_.setup_state(&this->state_);
    this->e131_.set_method(method);
    this->e131_.setup();
    this->effect_.set_first_universe(1);
    this->effect_.set_channels(e131::E131_RGB);
    this->effect_.set_e131(&this->e131_);
    this->effect_.init_internal(&this->state_);

    this->tx_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    memset(&this->dest_, 0, sizeof(this->dest_));
    this->dest_.sin_family = AF_INET;
    this->dest_.sin_port = htons(E131_PORT);
    this->dest_.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  }
  ~E131Rig() { ::close(this->tx_); }

  void start_effect() { this->effect_.start_internal(); }
  void stop_effect() { this->effect_.stop(); }
  void send(const uint8_t *buf, size_t len) {
    ::sendto(this->tx_, buf, len, 0, reinterpret_cast<const sockaddr *>(&this->dest_), sizeof(this->dest_));
  }
  e131::E131Component &e131() { return this->e131_; }
  BenchAddressableLight &output() { return this->output_; }

 protected:
  BenchAddressableLight output_;
  light::LightState state_;
  e131::E131Component e131_;
  e131::E131AddressableLightEffect effect_;
  int tx_{-1};
  sockaddr_in dest_{};
};

// --- Full receive path: N universes of RGB data arrive, loop() drains and applies them ---
// Includes the loopback sendto() of each datagram, which is identical before and after
// any change to the component, so differences come from recv + parse + apply.

static void E131_Loop_RGBFrame(benchmark::State &state) {
  const int universes = static_cast<int>(state.range(0));
  E131Rig rig(universes, e131::E131_UNICAST);
  rig.start_effect();

  std::vector<std::vector<uint8_t>> packets(universes, std::vector<uint8_t>(638));
  std::vector<size_t> lengths(universes);
  for (int u = 0; u < universes; u++)
    lengths[u] = build_e131_packet(packets[u].data(), u + 1, RGB_LIGHTS_PER_UNIVERSE * 3, static_cast<uint8_t>(u));

  // Sanity check: one frame must land on the strip.
  for (int u = 0; u < universes; u++)
    rig.send(packets[u].data(), lengths[u]);
  rig.e131().loop();
  const int last = universes * RGB_LIGHTS_PER_UNIVERSE - 1;
  const uint8_t expected = static_cast<uint8_t>((universes - 1) + (RGB_LIGHTS_PER_UNIVERSE - 1) * 3);
  if (rig.output().raw(last, 0) != expected) {
    state.SkipWithError("E1.31 frame was not applied to the light");
    return;
  }

  for (auto _ : state) {
    for (int u = 0; u < universes; u++)
      rig.send(packets[u].data(), lengths[u]);
    rig.e131().loop();
  }
  state.SetItemsProcessed(state.iterations() * universes);
  state.counters["leds"] = universes * RGB_LIGHTS_PER_UNIVERSE;
}
BENCHMARK(E131_Loop_RGBFrame)->Arg(1)->Arg(4);

// --- loop() with nothing queued: the cost paid on every main-loop pass while idle ---

static void E131_Loop_Idle(benchmark::State &state) {
  E131Rig rig(1, e131::E131_UNICAST);
  for (auto _ : state) {
    rig.e131().loop();
  }
}
BENCHMARK(E131_Loop_Idle);

// --- Starting and stopping an effect spanning N universes (multicast) ---
// Reports IGMP join calls per start and lwIP group references left behind afterwards.

static void E131_EffectStartStop(benchmark::State &state) {
  const int universes = static_cast<int>(state.range(0));
  E131Rig rig(universes, e131::E131_MULTICAST);
  int64_t cycles = 0;

  for (auto _ : state) {
    rig.start_effect();
    rig.stop_effect();
    cycles++;
  }

  int leaked = 0;
  for (const auto &entry : lwip_stub::igmp_groups)
    leaked += entry.second;
  state.counters["igmp_joins_per_start"] = static_cast<double>(lwip_stub::igmp_join_calls) / cycles;
  state.counters["leaked_group_refs_per_cycle"] = static_cast<double>(leaked) / cycles;
}
BENCHMARK(E131_EffectStartStop)->Arg(1)->Arg(4)->Arg(8);

}  // namespace esphome::benchmarks
