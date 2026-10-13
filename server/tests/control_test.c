#include <stdio.h>
#include <pthread.h>
#include <unistd.h>

#include "conn_types.h"
#include "control.h"


int main(void) {
  pthread_t control_h;
  char uid[] = "south_corridor.dwe4r";
  conn_t conn = {
    .fd = 1,
    .alive = true,
    .ismanaged = false,
  };
  sprintf(conn.uid, uid, sizeof(conn.uid));

  if (pthread_create(&control_h, NULL, control_pico_thread, &conn) != 0) {
    fprintf(stderr, "failed to create control thread\n");
    return 1;
  }

  sleep(1);
  conn.alive = false;
  while (conn.ismanaged) {
    printf("waiting for control thread to exit\n");
    sleep(1);
  }

  return 0;
}

