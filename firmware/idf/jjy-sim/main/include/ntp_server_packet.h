/*
 * JJY-SIM - NTP Packet Handling (API)
 *
 * Defines the clock snapshot and NTP reply generation interface.
 *
 * Copyright (c) 2026 Shachi-lab
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/time.h>

#define NTP_PACKET_SIZE 48
typedef struct {
  struct timeval reference;
  uint32_t source_ipv4; // Network byte order.
  uint32_t dispersion; // Unsigned 16.16 seconds.
  bool synchronized;
} ntp_server_clock_t;

bool ntp_server_make_reply(const uint8_t *request, size_t length,
                           const ntp_server_clock_t *clock,
                           const struct timeval *received,
                           const struct timeval *transmitted,
                           uint8_t reply[NTP_PACKET_SIZE]);
