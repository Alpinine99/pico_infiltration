#include "HCSR04.h"

#include <string.h>
#include <FreeRTOS.h>
#include <task.h>

#include "logger.h"

#define HC_SENSOR_GAP_MS 30
#define HC_MAX_ECHO_TIME_US (int64_t) ((HC_MAX_DIST_CM / 0.0334f) * 2)
#define HC_ECHO_MAX_WAIT 100 * 1000

#define TAG "[HCSR04] "

static volatile bool stop_task = false;
static volatile bool task_running = false;

static struct {
  uint n;
  hc_reading_t r[HC_MAX_SENSORS];
} snap;

void hc_sonar_init(hc_pin_t pin) {
  INFO(TAG "sonar init on trig: %d, echo: %d", pin.trig, pin.echo);

  gpio_init(pin.trig);
  gpio_set_dir(pin.trig, GPIO_OUT);
  gpio_put(pin.trig, 0);

  gpio_init(pin.echo);
  gpio_set_dir(pin.echo, GPIO_IN);
}

static hc_reading_t hc_sonar_read(hc_pin_t pin) {
  const hc_reading_t invalid = { .echo_us = 0, .dist_cm = -1.f };

  gpio_put(pin.trig, 1);
  busy_wait_us_32(10);
  gpio_put(pin.trig, 0);

  absolute_time_t t0 = get_absolute_time();
  while (!gpio_get(pin.echo)) {
    if (absolute_time_diff_us(t0, get_absolute_time()) > HC_ECHO_MAX_WAIT)
      return invalid;
  }

  absolute_time_t start = get_absolute_time();
  while (gpio_get(pin.echo)) {
    if (absolute_time_diff_us(start, get_absolute_time()) > HC_MAX_ECHO_TIME_US)
      return invalid;
  }
  int64_t us = absolute_time_diff_us(start, get_absolute_time());
  return (hc_reading_t) { .echo_us = (uint32_t) us, .dist_cm = ((0.0334f * us) / 2.f) };
}

void hc_sonar_task(void* args) {
  hc_pVparams_t* params = (hc_pVparams_t*) args;
  
  uint n = params->n_sensors;
  if (n > HC_MAX_SENSORS) {
    WARNING(TAG "n_sensors %u > %d, clamping", n, HC_MAX_SENSORS);
    n = HC_MAX_SENSORS;
  }
  
  task_running = true;
  // init sensors
  for (int i=0; i<n; i++)
    hc_sonar_init(params->pins[i]);

  INFO(TAG "Stating sonar task ... ");
  while (!stop_task) {
    hc_reading_t sweep[HC_MAX_SENSORS];
    for (int i=0; i<n; i++) {
      sweep[i] = hc_sonar_read(params->pins[i]);
      vTaskDelay(pdMS_TO_TICKS(HC_SENSOR_GAP_MS));
    }

    taskENTER_CRITICAL();
    memcpy(snap.r, sweep, n * sizeof(sweep[0]));
    snap.n = n;
    taskEXIT_CRITICAL();

    vTaskDelay(pdMS_TO_TICKS(params->wait_ms));
  }
  taskENTER_CRITICAL();
  snap.n = 0;
  taskEXIT_CRITICAL();
  stop_task = false;
  task_running = false;
  WARNING(TAG "sonar task exited");
  vTaskDelete(NULL);
}

uint hc_sonar_get_values(hc_reading_t* out, uint max_n, uint timeout) {
  const TickType_t start = xTaskGetTickCount();
  const TickType_t limit = pdMS_TO_TICKS(timeout);

  for (;;) {
    uint n;
    taskENTER_CRITICAL();
    n = snap.n < max_n ? snap.n : max_n;
    if (n)
      memcpy(out, snap.r, n * sizeof(snap.r[0]));
    taskEXIT_CRITICAL();

    if (n) return n;
    if ((TickType_t) (xTaskGetTickCount() - start) >= limit) return 0;
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void hc_sonar_kill_task() {
  if (!task_running) return;
  INFO(TAG "Stopping sonar task ... ");
  stop_task = true;
  while (task_running) vTaskDelay(pdMS_TO_TICKS(15));
}
  
bool hc_sonar_is_running(void) {
  return task_running;
}

