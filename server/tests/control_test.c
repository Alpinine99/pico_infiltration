#include <stdio.h>
#include <pthread.h>
#include <stdbool.h>
#include <unistd.h>

#include "conn_types.h"
#include "control.h"


int main(void) {
  pthread_t control_h;
  char uid[] = "south_corridor.dwe4r";
  conn_t conn = { .fd = 1 };
  sprintf(conn.uid, uid, sizeof(conn.uid));

  atomic_store(&conn.ismanaged, true);
  atomic_store(&conn.alive, true);
  if (pthread_create(&control_h, NULL, control_pico_thread, &conn) != 0) {
    fprintf(stderr, "failed to create control thread\n");
    return 1;
  }

  sleep(10);
  atomic_store(&conn.alive, false);
  while (conn.ismanaged) {
    printf("waiting for control thread to exit\n");
    sleep(1);
  }

  return 0;
}

