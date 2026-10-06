#pragma once

// Host-build stub of lwIP's ip4_addr.h: the host platform has no lwIP.

#include <arpa/inet.h>
#include <netinet/in.h>

using ip4_addr_t = struct in_addr;

#define IP4_ADDR(ipaddr, a, b, c, d) \
  ((ipaddr)->s_addr = htonl(((uint32_t) ((a) &0xff) << 24) | ((uint32_t) ((b) &0xff) << 16) | \
                            ((uint32_t) ((c) &0xff) << 8) | (uint32_t) ((d) &0xff)))
