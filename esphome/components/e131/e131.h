#pragma once
#include "esphome/core/defines.h"
#ifdef USE_NETWORK
#if defined(USE_SOCKET_IMPL_BSD_SOCKETS) || defined(USE_SOCKET_IMPL_LWIP_SOCKETS)
#include "esphome/components/socket/socket.h"
#elif defined(USE_SOCKET_IMPL_LWIP_TCP)
#include <WiFiUdp.h>
#endif
#include "esphome/core/component.h"

#include <cinttypes>
#include <memory>
#include <vector>

namespace esphome::e131 {

class E131AddressableLightEffect;

enum E131ListenMethod { E131_MULTICAST, E131_UNICAST };

const int E131_MAX_PROPERTY_VALUES_COUNT = 513;
/// Largest valid E1.31 data packet: 125 header bytes + start code + 512 DMX slots.
static constexpr size_t E131_MAX_PACKET_SIZE = 638;

/// DMX property values of a received packet. Points into the receive buffer, valid during loop() only.
struct E131Packet {
  uint16_t count{0};               // number of values, including the DMX start code at values[0]
  const uint8_t *values{nullptr};  // values[0] is the start code
};

struct UniverseConsumer {
  uint16_t universe;
  uint16_t consumers;
};

class E131Component final : public esphome::Component {
 public:
  E131Component();
  ~E131Component();

  void setup() override;
  void loop() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  void add_effect(E131AddressableLightEffect *light_effect);
  void remove_effect(E131AddressableLightEffect *light_effect);

  void set_method(E131ListenMethod listen_method) { this->listen_method_ = listen_method; }

 protected:
  inline ssize_t read_(uint8_t *buf, size_t len) {
#if defined(USE_SOCKET_IMPL_BSD_SOCKETS) || defined(USE_SOCKET_IMPL_LWIP_SOCKETS)
    return this->socket_->read(buf, len);
#elif defined(USE_SOCKET_IMPL_LWIP_TCP)
    if (!this->udp_.parsePacket())
      return -1;
    return this->udp_.read(buf, len);
#else
    return -1;
#endif
  }
  bool packet_(const uint8_t *data, size_t len, int &universe, E131Packet &packet);
  bool process_(int universe, const E131Packet &packet);
  void join_igmp_groups_();
  void igmp_join_(uint16_t universe);
  UniverseConsumer *find_universe_(int universe);
  void join_(int universe);
  void leave_(int universe);

  E131ListenMethod listen_method_{E131_MULTICAST};
  // Set once the socket is bound; IGMP joins requested earlier are made in setup().
  bool multicast_ready_{false};
#if defined(USE_SOCKET_IMPL_BSD_SOCKETS) || defined(USE_SOCKET_IMPL_LWIP_SOCKETS)
  std::unique_ptr<socket::Socket> socket_;
#elif defined(USE_SOCKET_IMPL_LWIP_TCP)
  WiFiUDP udp_;
#endif
  std::vector<E131AddressableLightEffect *> light_effects_;
  std::vector<UniverseConsumer> universe_consumers_;
};

}  // namespace esphome::e131

#endif
