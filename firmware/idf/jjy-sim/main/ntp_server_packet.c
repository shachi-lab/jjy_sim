/*
 * JJY-SIM - NTP Packet Handling
 *
 * Validates NTP requests and constructs server replies.
 * - Encodes timestamps and synchronization information
 * - Keeps packet handling independent of socket/task management
 *
 * Copyright (c) 2026 Shachi-lab
 * SPDX-License-Identifier: MIT
 *
 * Developed by Shachi-lab
 * https://shachi-lab.com
 *   ____  _                _     _     _       _
 *  / ___)| |__   __ _  ___| |__ |_|   | | __ _| |__
 *  \___ \| '_ \ / _` |/ __) '_ \ _  _ | |/ _` | '_ \
 *   ___) | | | | (_| | (__| | | | ||_|| | (_| | |_) |
 *  (____/|_| |_|\__,_|\___)_| |_|_|   |_|\__,_|_.__/
 */
#include <string.h>
#include "ntp_server_packet.h"

static void put_u32(uint8_t *out, uint32_t value)
{
  out[0] = (uint8_t)(value >> 24);
  out[1] = (uint8_t)(value >> 16);
  out[2] = (uint8_t)(value >> 8);
  out[3] = (uint8_t)value;
}

static void put_timestamp(uint8_t *out, const struct timeval *tv)
{
  // Intentional modulo 2^32 also handles the NTP era rollover in 2036.
  put_u32(out, (uint32_t)((uint64_t)tv->tv_sec + 2208988800ULL));
  put_u32(out + 4, (uint32_t)(((uint64_t)tv->tv_usec << 32) / 1000000));
}

bool ntp_server_make_reply(const uint8_t *request, size_t length,
                           const ntp_server_clock_t *clock,
                           const struct timeval *received,
                           const struct timeval *transmitted,
                           uint8_t reply[NTP_PACKET_SIZE])
{
  // Only ordinary, unauthenticated v1-v4 client requests. No control/private modes.
  if (length != NTP_PACKET_SIZE) return false;
  unsigned version = (request[0] >> 3) & 7;
  if ((version < 1 || version > 4) || (request[0] & 7) != 3) return false;
  memset(reply, 0, NTP_PACKET_SIZE);
  reply[0] = (uint8_t)((clock->synchronized ? 0 : 0xc0) | (version << 3) | 4);
  // IDF SNTP does not expose upstream stratum. Use the last usable secondary
  // stratum conservatively, rather than claiming a direct primary reference.
  reply[1] = clock->synchronized ? 15 : 16;
  reply[2] = request[2] < 4 ? 4 : request[2] > 17 ? 17 : request[2];
  reply[3] = (uint8_t)-10; // Advertised clock precision ~1 ms, not measured accuracy.
  put_u32(reply + 4, 6554); // Conservative 100 ms root delay.
  put_u32(reply + 8, clock->dispersion);
  memcpy(reply + 12, &clock->source_ipv4, 4);
  if (clock->synchronized) put_timestamp(reply + 16, &clock->reference);
  memcpy(reply + 24, request + 40, 8); // Echo the client's transmit timestamp exactly.
  put_timestamp(reply + 32, received);
  put_timestamp(reply + 40, transmitted);
  return true;
}
