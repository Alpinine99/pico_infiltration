#include <FreeRTOS.h>
#include <task.h>

#include <pico/time.h>
#include <pico/stdlib.h>
#include <pico/unique_id.h>
#include <pico/cyw43_arch.h>

#include <hardware/adc.h>
#include <hardware/gpio.h>
#include <hardware/watchdog.h>
#include <hardware/structs/padsbank0.h>

#include "common_types.h"
#include "conn_man.hpp"
#include "HCSR04.h"
#include "logger.h"

#define TOLERANCE 5
#define N_SENSORS 3
#define UPDATE_TIME_US (uint64_t) UPDATE_TIME_S * 1000 * 1000

#define VSYS_GPIO 29
#define VSYS_ADC_CH 3
#define VSYS_SAMPLES 16

#define TAG "[app] "


static void app(void *args);
static bool connect_wifi(int max_retry);
static void error_indicator(int timeout_s);
static void heart_beat(void* args);
static void worker_updater(void* args);
static void print_pico_cmd(pico_command_t cmd);
static void print_distances(hc_reading_t *hc_values);
static void gen_unique_id(uint8_t* g_uid);
static uint16_t read_vsys_mv(void);

static uint8_t g_uid[MAX_UID_LEN];
static bool exit_task = false;
static TaskHandle_t worker_h = NULL;
static TaskHandle_t heart_beat_h = NULL;
static TaskHandle_t hc_sonar_h = NULL;

int main(void) {
  stdio_init_all();

  xTaskCreate(
    app, "main", 1024 * 10, NULL, 2, NULL
  );

  vTaskStartScheduler();
  while (true);
}

static void app(void *args) {
#if NLOGGING == 0
  // wait for 10 second then only proceed
  for (int i = 0; i < 100 && !stdio_usb_connected(); ++i)
    vTaskDelay(pdMS_TO_TICKS(100));
#endif

  // setup variables
  gen_unique_id(g_uid);
  static const hc_pin_t sonar_pins[N_SENSORS] = { {19, 16}, {20, 17}, {21, 18} };
  static hc_pVparams_t hc_params = {.n_sensors = N_SENSORS, .pins = (hc_pin_t*) sonar_pins, .wait_ms = 250};
  hc_reading_t hc_values[N_SENSORS];
  pico_command_t cmd;
  pico_dat_t pkt;
  uint rate;

  // create a connection object
  Conn_man conn(SERVER_ADDRESS, SERVER_PORT);
  uint error_tolerance = TOLERANCE;

  // wifi init
  INFO(TAG "Initializing wifi module ... ");
  while (cyw43_arch_init()) {
    WARNING(TAG "Failed to init wifi");
    if (error_tolerance <= 0) {
      ERROR(TAG "Maximum error tolerance reached");
      goto reboot;
    }
    error_tolerance--;
    vTaskDelay(pdMS_TO_TICKS(3000));
  }
  error_tolerance = TOLERANCE;
  cyw43_arch_enable_sta_mode();
  INFO(TAG "Wifi station mode enabled");

start_execution:
  INFO(TAG "Execution starting ... ");
  memset(hc_values, 0, sizeof(hc_values));
  error_tolerance = TOLERANCE;
  rate = 15000;

start_heatbeat:
  if (error_tolerance <= 0) {
    ERROR(TAG "Maximum error tolerance reached");
    goto reboot;
  }
  if (heart_beat_h) {
    vTaskDelete(heart_beat_h);
    heart_beat_h = NULL;
  }

  // blink alive state
  INFO(TAG "Starting hearbeat ... ");
  if (xTaskCreate(
    heart_beat, "heart_beat", 256, &rate, tskIDLE_PRIORITY + 1, &heart_beat_h
  ) != pdPASS) {
    WARNING(TAG "failed to start hearbeat");
    error_indicator(3);
    error_tolerance--;
    goto start_heatbeat;
  }
  error_tolerance = TOLERANCE;

connect_2_wifi: 
  if (cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA) == CYW43_LINK_UP) {
    cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
  // connecting to wifi
  INFO(TAG "Connecting to wifi network: %s ... ", WIFI_SSID);
  while (!connect_wifi(3)) {
    WARNING(TAG "Wifi connection failed, sleeping");
    vTaskDelay(pdMS_TO_TICKS(30000));
  }
  INFO(TAG "Connected to network: %s", WIFI_SSID);

connect_2_server:
  INFO(TAG "Connecting to server ... ");
  while (!conn.s_connect()) {
    conn.s_close();
    WARNING(TAG "failed to connect to server");
    error_indicator(3);
    if (cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA) != CYW43_LINK_UP) {
      ERROR(TAG "Wifi connection lost");
      goto connect_2_wifi;
    }
    vTaskDelay(pdMS_TO_TICKS(10000));
  }
  INFO(TAG "Connected to server");

create_sonar_worker:
  if (error_tolerance <= 0) {
    ERROR(TAG "Maximum error tolerance reached");
    goto reboot;
  }
  if (hc_sonar_is_running()) hc_sonar_kill_task();
  hc_sonar_h = NULL;

  INFO(TAG "Starting sonar worker thread ... ");
  if (xTaskCreate(
        hc_sonar_task, "sensors", 1024, &hc_params, tskIDLE_PRIORITY + 1, &hc_sonar_h
    ) != pdPASS) { 
    WARNING(TAG "failed to start sonar worker");
    error_indicator(3);
    error_tolerance--;
    goto create_worker;
  }
  // wait for worker to set run flag to 1
  vTaskDelay(pdMS_TO_TICKS(100));
  error_tolerance = TOLERANCE;

create_worker:
  if (error_tolerance <= 0) {
    ERROR(TAG "Maximum error tolerance reached");
    goto reboot;
  }
  if (worker_h) {
    vTaskDelete(worker_h);
    worker_h = NULL;
  }

  INFO(TAG "Starting updater worker thread ... ");
  if (xTaskCreate(
    worker_updater, "updater", 512, &conn, tskIDLE_PRIORITY + 1, &worker_h
  ) != pdPASS) {
    WARNING(TAG "failed to create worker");
    error_indicator(3);
    error_tolerance--;
    goto create_worker;
  }
  error_tolerance = TOLERANCE;

  INFO(TAG "Starting cmd reader ... ");
  while (
      worker_h && conn.s_is_connected() && hc_sonar_is_running() && 
      cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA) == CYW43_LINK_UP
      ) {
    int n = conn.s_read(&cmd);
    if (n > 0) {
      INFO(TAG "received [%d] from server", n);
      print_pico_cmd(cmd);
      switch (cmd.type) {
        case CMD_REBOOT:
          INFO(TAG "Received reboot intent from server");
          goto reboot;
        case CMD_SLEEP:
          INFO(TAG "Received sleep intent from server");
          goto sleep_for;
        default:
          INFO(TAG "Received a ping intent from server");
      }
    } else {
      WARNING(TAG "failed to recv cmd from server");
    }
  }
  INFO(TAG "cmd reader stopped");

  // if wifi disconnects
  if (cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA) != CYW43_LINK_UP) {
    ERROR(TAG "Wifi connection lost");
    error_indicator(3);
    conn.s_close();
    goto connect_2_wifi;
  }
  
  // if server disconnected
  if (!conn.s_is_connected()) {
    ERROR(TAG "Server connection lost");
    conn.s_close();
    goto connect_2_server;
  }

  // if worker is dead
  if (!worker_h) {
    ERROR(TAG "Worker is dead");
    goto create_worker;
  }

  // if sensors go dead
  if (!hc_sonar_is_running()) {
    ERROR(TAG "sensor task is dead");
    hc_sonar_h = NULL;
    goto create_sonar_worker;
  }

reboot:
  WARNING(TAG "rebooting pico");
  if (conn.s_is_connected()) conn.s_close();
  if (heart_beat_h) vTaskDelete(heart_beat_h);
  if (worker_h) vTaskDelete(worker_h);
  if (hc_sonar_h) vTaskDelete(hc_sonar_h);
  cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
  cyw43_arch_disable_sta_mode();

  error_indicator(5);
  cyw43_arch_deinit();
  watchdog_reboot(0, 0, 100);   // reboot after 100 ms
  vTaskDelete(NULL);

sleep_for:
  WARNING(TAG "Preparing for herbernation ... ");
  INFO(TAG "Waiting for workers to exit ... ");
  exit_task = true;
  hc_sonar_kill_task();
  hc_sonar_h = NULL;
  while (worker_h || heart_beat_h) vTaskDelay(pdMS_TO_TICKS(10));

  if (conn.s_is_connected()) conn.s_close();
  if (cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA) != 0)
    WARNING(TAG "wifi leave failed connection");

  cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);
  vTaskDelay(pdMS_TO_TICKS(3000));
  cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0);

  // start sleep
  INFO(TAG "sleeping for %d s", cmd.sleep_n_sec);
  vTaskDelay(pdMS_TO_TICKS(1000 * cmd.sleep_n_sec));
  exit_task = false;
  goto start_execution;
}

static void heart_beat(void *args) {
  uint rate = *(uint*) args;
  while (!exit_task) {
    for (int i=0; i<4; i++) {
      cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, !cyw43_arch_gpio_get(CYW43_WL_GPIO_LED_PIN));
      vTaskDelay(pdMS_TO_TICKS(100));
    }
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(rate));
  }
  heart_beat_h = NULL;
  vTaskDelete(NULL);
}

static void print_pico_cmd(pico_command_t cmd) {
  INFO(TAG "==== pico command ====");
  INFO(TAG " TYPE: %s", (cmd.type == CMD_REBOOT ? "reboot" : cmd.type == CMD_SLEEP ? "sleep" : "ping"));
  INFO(TAG " SLEEP_N_SEC: %d", cmd.sleep_n_sec);
  INFO(TAG "======================");
}

static void print_distances(hc_reading_t *hc_values) {
    INFO(TAG "====== values ======");
    for (int i=0; i<N_SENSORS; i++)
      INFO(TAG " echo_us: %ld, dist_cm: %3.f", hc_values[i].echo_us, hc_values[i].dist_cm);
    INFO(TAG "====================");
}

static void gen_unique_id(uint8_t* g_uid) {
  // Use board unique id
  char unique_id_buf[5];
  pico_get_unique_board_id_string(unique_id_buf, sizeof(unique_id_buf));
  for(int i=0; i < sizeof(unique_id_buf) - 1; i++)
      unique_id_buf[i] = tolower(unique_id_buf[i]);
  
  // Generate a unique name, e.g. left-coridor-123
  char uid[MAX_UID_LEN];
  snprintf(uid, MAX_UID_LEN, "%s.%s", ROOM_LOCATION, unique_id_buf);
  for (int i=strlen(uid); i<MAX_UID_LEN; i++)
    uid[i] = 0;

  INFO(TAG "Device name %s", uid);
  memcpy(g_uid, uid, MAX_UID_LEN);
}

static void worker_updater(void* args) {
  Conn_man *conn = (Conn_man*) args;
  float d[N_SENSORS] = { 0, 0, 0 };
  float lowest_val[N_SENSORS] = { HC_MAX_DIST_CM, HC_MAX_DIST_CM, HC_MAX_DIST_CM };
  float divisor[N_SENSORS] = {0, 0, 0};
  absolute_time_t last_updated = get_absolute_time(); 
  absolute_time_t t0 = get_absolute_time(); 
  while (!exit_task) {
    hc_reading_t r[N_SENSORS];
    uint n = hc_sonar_get_values(r, 3, 0);
    for (int i=0; i<n; i++) {
      if (r[i].dist_cm > 0) {
        d[i] += r[i].dist_cm;
        divisor[i]++;
      }
    }
    if (!n) WARNING(TAG "Nothing read from hc_sonar");

    // average as per REACTION_TIME_MS
    if (absolute_time_diff_us(t0, get_absolute_time()) < REACTION_TIME_MS) {
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }
    
    for (int i=0; i<N_SENSORS; i++) {
      if (divisor[i] != 0) {
        d[i] /= divisor[i];
        if (d[i] < lowest_val[i])
          lowest_val[i] = d[i];
      }
      d[i] = 0; divisor[i] = 0;
    }

    if (absolute_time_diff_us(last_updated, get_absolute_time()) >= UPDATE_TIME_US) {
      // print_distances(r);
      for (int i=0; i<N_SENSORS; i++) {
        if (lowest_val[i] == HC_MAX_DIST_CM)
          lowest_val[i] = -1.f;
      }

      INFO(TAG "dist_cm: {%.3f, %.3f, %.3f}", lowest_val[0], lowest_val[1], lowest_val[2]);
      pico_dat_t data = {
        .magic  = PACKET_MAGIC,
        .uptime = (uint32_t)(time_us_64() / 1000000ULL),    // wraps in 136 years
        .heap_free = xPortGetFreeHeapSize(),
        .heap_min_free = xPortGetMinimumEverFreeHeapSize(),
        .battery_mv = read_vsys_mv(),
        .dist = { lowest_val[0], lowest_val[1], lowest_val[2] }
      };
      memcpy(&data.uid, g_uid, sizeof(data.uid));
      
      if (!conn->s_send(&data)) {
        ERROR(TAG "update to server failed");
        break;
      }
      INFO(TAG "Update sent to server");
      for (int i=0; i<N_SENSORS; i++)
        lowest_val[i] = HC_MAX_DIST_CM;
      last_updated = get_absolute_time();
    }
    t0 = get_absolute_time();
  }
  WARNING(TAG "Worker exiting ... ");
  worker_h = NULL;
  vTaskDelete(NULL);
}

static uint16_t read_vsys_mv(void) {
  cyw43_thread_enter();
  gpio_function_t func = gpio_get_function(VSYS_GPIO);
  uint32_t  pad = padsbank0_hw->io[VSYS_GPIO];

  adc_init();
  adc_gpio_init(VSYS_GPIO);
  adc_select_input(VSYS_ADC_CH);
  for (int i=0; i<4; ++i) (void) adc_read();
  uint32_t sum = 0;
  for (int i=0; i<VSYS_SAMPLES; ++i) sum += adc_read();

  gpio_set_function(VSYS_GPIO, func);
  padsbank0_hw->io[VSYS_GPIO] = pad;
  cyw43_thread_exit();

  uint32_t raw = sum / VSYS_SAMPLES;
  return (uint16_t) (raw * 3u * 3300u / 4096u);
}

static void error_indicator(int timeout_s) {
  for (int i=0; i<timeout_s; i++) {
    for (int j=0; j<20; j++) {
      cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, !cyw43_arch_gpio_get(CYW43_WL_GPIO_LED_PIN));
      vTaskDelay(pdMS_TO_TICKS(50));
    }
  }
  cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0);
}

bool connect_wifi(int max_retry) {
  int rc;
  for (int i = 0; i < max_retry; ++i) {
    if ((rc = cyw43_arch_wifi_connect_timeout_ms(WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA2_AES_PSK, 30000)) == 0) {
      INFO(TAG "Connected. IP: %s", ip4addr_ntoa(netif_ip4_addr(netif_default)));
      return true;
    }
    WARNING(TAG "Wifi connection failed error=%d, retrying ...", rc);
    error_indicator(3);
  }
  return false;
}

