#include <FreeRTOS.h>
#include <task.h>

#include <pico/time.h>
#include <pico/stdlib.h>
#include <pico/unique_id.h>
#include <pico/cyw43_arch.h>
#include <hardware/watchdog.h>
#include "common_types.h"
#include "conn_man.hpp"
#include "HCSR04.h"

#if NLOGGING == 1
#define DEBUG_printf(...)
#else
#define DEBUG_printf(fmt, ...) printf("DEBUG: " fmt "\n", ##__VA_ARGS__)
#endif

#define TOLERANCE 10
#define N_SENSORS 3

static void app(void *args);
static bool connect_wifi(int max_retry);
static void error_indicator(int timeout_s);
static void heart_beat(void* args);
static void worker_updater(void* args);
static void print_pico_cmd(pico_command_t cmd);
static void print_distances(hc_reading_t *hc_values);
static void gen_unique_id(uint8_t* g_uid);

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

  gen_unique_id(g_uid);
  while (cyw43_arch_init()) {
    DEBUG_printf("Failed to init wifi");
    vTaskDelay(pdMS_TO_TICKS(3000));
  }
  cyw43_arch_enable_sta_mode();
  DEBUG_printf("Wifi station mode enabled");

start_execution:
  DEBUG_printf("Execution starting ... ");
  Conn_man conn(SERVER_ADDRESS, SERVER_PORT);
  uint error_tolerance = TOLERANCE;
  static const hc_pin_t sonar_pins[N_SENSORS] = { {19, 16}, {20, 17}, {21, 18} };
  static hc_pVparams_t hc_params = { .n_sensors = N_SENSORS, .pins = (hc_pin_t*) sonar_pins, .wait_ms = 250 };
  hc_reading_t hc_values[N_SENSORS];
  
  pico_command_t cmd;
  pico_dat_t pkt;
  uint rate = 15000;

start_heatbeat:
  if (error_tolerance <= 0) {
    DEBUG_printf("Error tolerance reached: %d", error_tolerance);
    goto reboot;
  }
  if (heart_beat_h) {
    vTaskDelete(heart_beat_h);
    heart_beat_h = NULL;
  }

  DEBUG_printf("Starting hearbeat");
  if (xTaskCreate(
    heart_beat, "heart_beat", 256, &rate, tskIDLE_PRIORITY + 1, &heart_beat_h
  ) != pdPASS) {
    DEBUG_printf("failed to start hearbeat");
    error_indicator(5);
    error_tolerance--;
    goto start_heatbeat;
  }
  error_tolerance = TOLERANCE;

connect_2_wifi: 
  if (cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA) == CYW43_LINK_UP) {
    cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
  while (!connect_wifi(30)) {
    DEBUG_printf("Wifi connection failed, sleeping");
    vTaskDelay(pdMS_TO_TICKS(30000));
  }

connect_2_server:
  DEBUG_printf("Connecting to server");
  while (!conn.s_connect()) {
    DEBUG_printf("failed to connect to server");
    error_indicator(5);
    if (cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA) != CYW43_LINK_UP) {
      DEBUG_printf("Wifi connection lost");
      goto connect_2_wifi;
    }
    conn.s_close();
    vTaskDelay(pdMS_TO_TICKS(5000));
  }
  DEBUG_printf("Connected to server");

create_sonar_worker:
  if (error_tolerance <= 0) {
    DEBUG_printf("Error tolerance reached: %d", error_tolerance);
    goto reboot;
  }
  if (hc_sonar_is_running()) {
    hc_sonar_kill_task();
    while (hc_sonar_is_running()) vTaskDelay(pdMS_TO_TICKS(10));
  }
  hc_sonar_h = NULL;

  DEBUG_printf("Starting sonar worker thread");
  if (xTaskCreate(
        hc_sonar_task, "sensors", 1024, &hc_params, tskIDLE_PRIORITY + 1, &hc_sonar_h
    ) != pdPASS) { 
    DEBUG_printf("failed to start sonar worker");
    error_indicator(5);
    error_tolerance--;
    goto create_worker;
  }
  error_tolerance = TOLERANCE;

create_worker:
  if (error_tolerance <= 0) {
    DEBUG_printf("Error tolerance reached: %d", error_tolerance);
    goto reboot;
  }
  if (worker_h) {
    vTaskDelete(worker_h);
    worker_h = NULL;
  }

  DEBUG_printf("Creating worker");
  if (xTaskCreate(
    worker_updater, "updater", 512, &conn, tskIDLE_PRIORITY + 1, &worker_h
  ) != pdPASS) {
    DEBUG_printf("failed to create worker");
    error_indicator(5);
    error_tolerance--;
    goto create_worker;
  }
  error_tolerance = TOLERANCE;

  DEBUG_printf("Starting cmd reader");
  while (
      worker_h && conn.s_is_connected() && hc_sonar_is_running() && 
      cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA) == CYW43_LINK_UP
      ) {
    int n = conn.s_read(&cmd);
    if (n == 1) {
      DEBUG_printf("received [%d] from server", n);
      print_pico_cmd(cmd);
      switch (cmd.type) {
        case CMD_REBOOT:
          DEBUG_printf("Received reboot intent from server");
          goto reboot;
        case CMD_SLEEP:
          DEBUG_printf("Received sleep intent from server");
          goto sleep_for;
        default:
          DEBUG_printf("Received a ping intent from server");
      }
    } else {
      DEBUG_printf("failed to recv cmd from server");
    }
  }

  // if wifi disconnects
  if (cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA) != CYW43_LINK_UP) {
    DEBUG_printf("Wifi connection lost");
    error_indicator(3);
    conn.s_close();
    goto connect_2_wifi;
  }
  
  // if server disconnected
  if (!conn.s_is_connected()) {
    DEBUG_printf("Server connection lost");
    conn.s_close();
    goto connect_2_server;
  }

  // if worker is dead
  if (!worker_h) {
    DEBUG_printf("Worker is dead");
    goto create_worker;
  }

  // if sensors go dead
  if (!hc_sonar_is_running()) {
    DEBUG_printf("sensor task is dead");
    hc_sonar_h = NULL;
    goto create_sonar_worker;
  }

reboot:
  DEBUG_printf("rebooting pico");
  if (conn.s_is_connected()) conn.s_close();
  if (heart_beat_h) vTaskDelete(heart_beat_h);
  if (worker_h) vTaskDelete(worker_h);
  if (hc_sonar_h) vTaskDelete(hc_sonar_h);
  if (cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA) != 0)
    DEBUG_printf("wifi leave failed");

  error_indicator(5);
  cyw43_arch_disable_sta_mode();
  cyw43_arch_deinit();
  watchdog_reboot(0, 0, 100);   // reboot after 100 ms
  vTaskDelete(NULL);

sleep_for:
  exit_task = true;
  hc_sonar_kill_task();
  DEBUG_printf("Waiting for workers to exit");
  while (worker_h || heart_beat_h || hc_sonar_is_running())
    vTaskDelay(pdMS_TO_TICKS(100));
  hc_sonar_h = NULL;

  if (conn.s_is_connected()) conn.s_close();
  if (cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA) != 0)
    DEBUG_printf("wifi leave failed");

  cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);
  vTaskDelay(pdMS_TO_TICKS(2000));
  cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0);

  // start sleep
  DEBUG_printf("sleeping for %d s", cmd.sleep_n_sec);
  vTaskDelay(pdMS_TO_TICKS(1000 * (cmd.sleep_n_sec - 2)));
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
  DEBUG_printf("==== pico command ====");
  DEBUG_printf(" TYPE: %s", (cmd.type == CMD_REBOOT ? "reboot" : cmd.type == CMD_SLEEP ? "sleep" : "ping"));
  DEBUG_printf(" SLEEP_N_SEC: %d", cmd.sleep_n_sec);
  DEBUG_printf("======================");
}

static void print_distances(hc_reading_t *hc_values) {
    DEBUG_printf("====== values ======");
    for (int i=0; i<N_SENSORS; i++)
      DEBUG_printf(" echo_us: %ld, dist_cm: %3.f", hc_values[i].echo_us, hc_values[i].dist_cm);
    DEBUG_printf("====================");
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

  DEBUG_printf("Device name %s", uid);
  memcpy(g_uid, uid, MAX_UID_LEN);
}

static void worker_updater(void* args) {
  Conn_man *conn = (Conn_man*) args;
  DEBUG_printf("Worker started");
  while (!exit_task) {
    hc_reading_t r[N_SENSORS];
    float d[N_SENSORS] = { -1.f, -1.f, -1.f };
    
    uint n = hc_sonar_get_values(r, 3, 0);
    for (int i=0; i<n; i++)
      d[i] = r[i].dist_cm;
    if (!n) DEBUG_printf("Nothing read from hc_sonar");

    print_distances(r);
    pico_dat_t data = {
      .magic  = PACKET_MAGIC,
      .uptime = (uint32_t)(time_us_64() / 1000000ULL),    // wraps in 136 years
      .heap_free = xPortGetFreeHeapSize(),
      .heap_min_free = xPortGetMinimumEverFreeHeapSize(),
      .battery_mv = 3000,
      .dist = { d[0], d[1], d[2] }
    };
    memcpy(&data.uid, g_uid, sizeof(data.uid));
    
    DEBUG_printf("s_send sending update");
    if (!conn->s_send(&data)) {
      DEBUG_printf("s_send failed");
      break;
    }
    
    DEBUG_printf("Update sent to server");
    vTaskDelay(pdMS_TO_TICKS(5000));
  }
  DEBUG_printf("Worker exiting ... ");
  worker_h = NULL;
  vTaskDelete(NULL);
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
  DEBUG_printf("Connecting to wifi network: %s", WIFI_SSID);
  for (int i = 0; i < max_retry; ++i) {
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);
    int rc = cyw43_arch_wifi_connect_timeout_ms(WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA2_AES_PSK, 30000);
    if (rc == 0) {
      DEBUG_printf("Connected. IP: %s", ip4addr_ntoa(netif_ip4_addr(netif_default)));
      return true;
    }
    DEBUG_printf("Wifi connection failed error=%d, retrying ...", rc);
    error_indicator(3);
  }
  return false;
}

