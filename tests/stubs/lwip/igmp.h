#pragma once

// Host-build stub of lwIP's IGMP API (the host platform has no lwIP).
//
// Mirrors lwIP's per-group reference counting: every igmp_joingroup() increments the
// group's use count, and igmp_leavegroup() decrements it, leaving the group only when
// the count reaches zero. Tests and benchmarks inspect esphome::lwip_stub to verify
// that joins and leaves are balanced.

#include <arpa/inet.h>
#include <netinet/in.h>

#include <cstdint>
#include <map>

using err_t = int8_t;
static constexpr err_t ERR_OK = 0;
static constexpr err_t ERR_VAL = -6;

namespace esphome::lwip_stub {

inline const struct in_addr IP4_ADDR_ANY4_VALUE = {INADDR_ANY};

/// Joined groups (network byte order address) -> lwIP use count.
inline std::map<uint32_t, int> igmp_groups;
inline uint32_t igmp_join_calls = 0;
inline uint32_t igmp_leave_calls = 0;

inline void reset() {
  igmp_groups.clear();
  igmp_join_calls = 0;
  igmp_leave_calls = 0;
}

/// lwIP use count for 239.255.<hi>.<lo>, the E1.31 multicast group of a universe.
inline int e131_group_use(uint16_t universe) {
  auto it = igmp_groups.find(htonl(0xEFFF0000u | universe));
  return it == igmp_groups.end() ? 0 : it->second;
}

}  // namespace esphome::lwip_stub

#define IP4_ADDR_ANY4 (&esphome::lwip_stub::IP4_ADDR_ANY4_VALUE)

inline err_t igmp_joingroup(const struct in_addr * /*ifaddr*/, const struct in_addr *groupaddr) {
  esphome::lwip_stub::igmp_join_calls++;
  esphome::lwip_stub::igmp_groups[groupaddr->s_addr]++;
  return ERR_OK;
}

inline err_t igmp_leavegroup(const struct in_addr * /*ifaddr*/, const struct in_addr *groupaddr) {
  esphome::lwip_stub::igmp_leave_calls++;
  auto it = esphome::lwip_stub::igmp_groups.find(groupaddr->s_addr);
  if (it == esphome::lwip_stub::igmp_groups.end())
    return ERR_VAL;
  if (--it->second <= 0)
    esphome::lwip_stub::igmp_groups.erase(it);
  return ERR_OK;
}
