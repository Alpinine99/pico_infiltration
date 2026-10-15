#pragma once

#include <stdint.h>

#define PACKET_MAGIC 0xA55A
#define MAX_UID_LEN 24

typedef struct __attribute__((packed)) {
  uint16_t magic;             // PACKET_MAGIC, detects a desynced stream
  uint32_t uptime;            // seconds (or ms) since boot
  uint32_t heap_free;         // xPortGetFreeHeapSize()
  uint32_t heap_min_free;     // xPortGetMinimumEverFreeHeapSize()
  uint8_t  uid[MAX_UID_LEN];  // raw bytes
  uint16_t battery_mv;        // millivolts
  float    dist[3];
} pico_dat_t;

enum {
  CMD_REBOOT  = 0x00,
  CMD_SLEEP   = 0x01,
  CMD_PING    = 0x02
};

typedef struct __attribute__((packed)) {
  uint16_t magic;
  uint8_t  type;
  uint32_t sleep_n_sec;
} pico_command_t;

_Static_assert(sizeof(pico_dat_t) == 2 + 4 + 4 + 4 + MAX_UID_LEN + 2 + 12, "pico_dat_t layout changed");
_Static_assert(sizeof(pico_command_t) == 2 + 1 + 4, "pico_command_t layout changed");

