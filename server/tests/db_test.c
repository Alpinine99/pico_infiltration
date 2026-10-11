#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "common_types.h"
#include "config.h"
#include "db.h"


void print_pico_data(pico_dat_t d);

int main(void) {
  if (log_open() == -1) return 1;
  db_open();
  for (int i=0; i<10; i++) {
    pico_dat_t data = {
      .uptime = i * 30,
      .heap_free = 324 * i,
      .heap_min_free = 234 * i,
      .battery_mv = 4256,
      .dist = { 45.f, 90.f, 120.4f }
    };
    sprintf(data.uid, "south coridor");
    printf("\n --- writing --- \n");
    print_pico_data(data);
    db_log(&data);
    sleep(2);
  }
  db_close();
  log_close();
  return 0;
}

void print_pico_data(pico_dat_t d) {
  printf(" uptime: %d\n heap_free: %d\n heap_min_free: %d\n uid: %s\n battery_mv: %d\n dist: {%.2f, %.2f, %.2f}\n", d.uptime, d.heap_free, d.heap_min_free, d.uid, d.battery_mv, d.dist[0], d.dist[1], d.dist[2]);
}

