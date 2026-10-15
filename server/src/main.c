#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <time.h>
#include <string.h>
#include <pthread.h>

#include <errno.h>
#include <signal.h>

#include "common_types.h"
#include "conn_types.h"
#include "control.h"
#include "logger.h"
#include "config.h"
#include "db.h"

#define TAG "main: "

typedef struct {
  pthread_t tid;
  conn_t conn;
  bool used;
} slot_t;

void* handle_conn(void*);
static void on_signal(int sig);
static void install_signal(void);

static volatile sig_atomic_t g_stop = 0;
static slot_t slots[MAX_CONN];


int main(void) {
  if (log_open() == -1) return 1;
  install_signal();
  
  int fd, rc = 0, opt = 1;
  struct sockaddr_in sock = {
    .sin_family = AF_INET,
    .sin_port = htons(PORT),
    .sin_addr.s_addr = INADDR_ANY
  };

  DEBUG(TAG "Creating socket ...");
  if ((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
    ERROR(TAG "Socket creation failed: %d", fd);
    return 1;
  }

  DEBUG(TAG "Setting SO_REUSEADDR sock opt ... ");
  if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
    WARNING(TAG "SO_REUSEADDR failed");

  DEBUG(TAG "Binding socket ... ");
  if ((rc = bind(fd, (struct sockaddr*) &sock, sizeof(sock))) < 0) {
    ERROR(TAG "Socket bind failed: %d", rc);
    goto done;
  }
  
  DEBUG(TAG "Listening for %d connections ... ", MAX_CONN);
  if ((rc = listen(fd, MAX_CONN)) < 0) {
    ERROR(TAG "Listen failed: %d", rc);
    goto done;
  }

  db_open();
  INFO(TAG "server running on 0.0.0.0:%d ...", PORT);
  while (!g_stop) {
    struct sockaddr_in picoaddr;
    socklen_t addrlen = sizeof(picoaddr);

    INFO(TAG "Awaiting connection ... ");
    int picofd = accept(fd, (struct sockaddr*) &picoaddr, &addrlen);
    if (picofd < 0) {
      if (picofd != EINTR) ERROR_ERRNO(TAG "Accept failed");
      continue;
    }
    DEBUG(TAG "New connection established");

    int idx = -1;
    for (int i=0; i<MAX_CONN; i++) {
      if (slots[i].used && pthread_tryjoin_np(slots[i].tid, NULL) == 0)
        slots[i].used = false;
      if(!slots[i].used && idx < 0)
        idx = i;
    }

    if (idx < 0) {
      WARNING(TAG "out of workers");
      close(picofd);
      continue;
    }

    conn_t *c = &slots[idx].conn;
    c->fd = picofd;
    c->addr = picoaddr;
    c->uid[0] = '\0';
    atomic_store(&c->ismanaged, false);
    atomic_store(&c->alive, true);
   
    DEBUG(TAG "Creating handler thread ... ");
    if (pthread_create(&slots[idx].tid, NULL, handle_conn, c) != 0) {
      ERROR(TAG "Failed to create worker thread");
      close(picofd);
    } else {
      INFO(TAG "New worker running id: %d", idx);
      slots[idx].used = true;
    }
  }

done:
  WARNING(TAG "Closing server socket ...");
  close(fd);

  INFO(TAG "Waiting for pico thread to exit ...");
  for (int i=0; i<MAX_CONN; i++) {
    if (!slots[i].used) continue;
    atomic_store(&slots[i].conn.alive, false);
    shutdown(slots[i].conn.fd, SHUT_RDWR);
    pthread_join(slots[i].tid, NULL);
    slots[i].used = false;
  }
  db_close();
  log_close();
  return rc;
}

void* handle_conn(void* conn) {
  conn_t* picoconn = (conn_t*) conn;
  char ip[INET_ADDRSTRLEN] = {0};
  int picofd = picoconn->fd;
  struct sockaddr_in picoaddr = picoconn->addr;

  inet_ntop(AF_INET, &picoaddr.sin_addr, ip, INET_ADDRSTRLEN);
  uint16_t port = ntohs(picoaddr.sin_port);
  INFO(TAG "Connection from %s:%d", ip, port);

  pico_dat_t picodata;
  pthread_t control_handle;
  bool have_ctrl = false;
  uint invalid_packet_count = 0;
  pico_command_t ping = { .type = CMD_PING, .sleep_n_sec = 10 };
  while (atomic_load(&picoconn->alive)) {
    int n = control_pico_read_data(picofd, &picodata);
    if (n > 1) {
      if (picoconn->uid[0] == '\0')
        memcpy(&picoconn->uid, &picodata.uid, MAX_UID_LEN);

      INFO(TAG "received [%d] from client %s:%d (%s)", n, ip, port, picoconn->uid);
      if (!have_ctrl && picoconn->uid[0] != '\0' && !atomic_load(&picoconn->ismanaged)) {
        atomic_store(&picoconn->ismanaged, true);
        if (pthread_create(&control_handle, NULL, control_pico_thread, picoconn) != 0) {
          atomic_store(&picoconn->ismanaged, false);
          ERROR_ERRNO(TAG "failed to create pico_control for client %s:%d (%s)", ip, port, picoconn->uid);
          picoconn->uid[0] = '\0';
          have_ctrl = false;
        } else have_ctrl = true;
      }
      control_pico_log_dat(picodata);
      db_log(&picodata);
      control_pico_send_cmd(picofd, &ping);
      invalid_packet_count = 0;
    } else {
      if (n == SCANNER_PACKET) {
        if (invalid_packet_count >= INVALID_PACKET_TOLERANCE) {
          ERROR(TAG "definetely a scanner; IP: %s", ip);
          break;
        } else {
          WARNING(TAG "potential scanner; got invalid packet from %s:%d (%s)", ip, port, picoconn->uid);
          invalid_packet_count++;
        }
      } else {
        ERROR(TAG "failed to read from client %s:%d (%s)", ip, port, picoconn->uid);
        break;
      }
    }
  }

  INFO(TAG "Cleanup on clent %s:%d", ip, port);
  atomic_store(&picoconn->alive, false);
  if (have_ctrl && atomic_load(&picoconn->ismanaged)) {
    INFO(TAG "waiting for control thread to exit ... ");
    pthread_join(control_handle, NULL);
  }
  close(picofd);
  return NULL;
}

static void on_signal(int sig) {
  WARNING(TAG "signal received, shutting down ... ");
  g_stop = sig;
}

static void install_signal(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = on_signal;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_RESETHAND;

  const int sigs[] = { SIGINT, SIGTERM, SIGHUP, SIGQUIT };
  for (int i=0; i<sizeof(sigs) / sizeof(sigs[0]); i++)
    if (sigaction(sigs[i], &sa, NULL) < 0)
      ERROR_ERRNO(TAG "sigaction: failed");

  signal(SIGPIPE, SIG_IGN); // send() to close socket
}

