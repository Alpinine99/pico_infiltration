#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <pico/stdlib.h>
#include <pico/time.h>
#include <hardware/gpio.h>


#if NLOGGING == 1
#define DEBUG_printf(...)
#else
#define DEBUG_printf(fmt, ...) printf("DEBUG: " fmt "\n", ##__VA_ARGS__)
#endif

#ifdef __cplusplus
extern "C" {
#endif

  typedef struct {
    uint trig;
    uint echo;
  } hc_pin_t;

  typedef struct {
    uint      n_sensors;
    hc_pin_t  *pins;
    uint      wait_ms;
  } hc_pVparams_t;

  typedef struct {
    uint32_t  echo_us;
    float     dist_cm;
  } hc_reading_t;

  void hc_sonar_task(void* args);
  void hc_sonar_kill_task(void);
  bool hc_sonar_is_running(void);
  uint hc_sonar_get_values(hc_reading_t* values, uint max_n, uint timeout);

#ifdef __cplusplus
}
#endif

