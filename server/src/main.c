#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <time.h>
#include <string.h>
#include <pthread.h>

#include <errno.h>
#include <signal.h>

#include "common_types.h"
#include "logger.h"
#include "config.h"
#include "db.h"

#define TAG "main: "

typedef struct {
  int fd;
  bool alive;
  bool ismanaged;
  char uid[MAX_UID_LEN];
  struct sockaddr_in addr;
} conn_t;

typedef struct {
  size_t size;
  size_t max;
  conn_t* conns;
} conn_arr_t;


void* handle_conn(void*);
void* pico_control(void*);
int read_pico_data(int picofd, pico_dat_t* data);
int send_pico_cmd(int picofd, pico_command_t* cmd);
static void log_pico_data(pico_dat_t picodata);
static void on_signal(int sig);
static void install_signal(void);

static volatile sig_atomic_t g_stop = 0;


int main(void) {
  if (log_open() == -1) return 1;
  install_signal();
  int fd, opt = 1;
  struct sockaddr_in sock = {
    .sin_family = AF_INET,
    .sin_port = htons(PORT),
    .sin_addr.s_addr = INADDR_ANY
  };

  int picofd;
  struct sockaddr_in picoaddr;
  socklen_t addrlen = sizeof(picoaddr);
  pthread_t picothread;
  conn_t conn;

  DEBUG(TAG "Creating socket ...");
  if ((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
    ERROR(TAG "Socket creation failed");
    return 1;
  }

  DEBUG(TAG "Setting SO_REUSEADDR sock opt ... ");
  if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
    WARNING(TAG "SO_REUSEADDR failed");
  }

  DEBUG(TAG "Binding socket ... ");
  if (bind(fd, (struct sockaddr*) &sock, sizeof(sock)) < 0) {
    ERROR(TAG "Socket bind failed");
    goto cleanup_error;
  }
  
  DEBUG(TAG "Listening for %d connections ... ", MAX_CONN);
  if (listen(fd, MAX_CONN) < 0) {
    ERROR(TAG "Listen failed");
    goto cleanup_error;
  }

  db_open();
  INFO(TAG "server running on 0.0.0.0:%d ...", PORT);
  while (!g_stop) {
    INFO(TAG "Awaiting connection ... ");
    if ((picofd = accept(fd, (struct sockaddr*) &picoaddr, &addrlen)) < 0) {
      ERROR(TAG "Accept failed");
      continue;
    }
    DEBUG(TAG "New connection established");
    conn.fd = picofd;
    conn.addr = picoaddr;
    conn.uid[0] = '\0';
    conn.ismanaged = false;
    conn.alive = true;
   
    DEBUG(TAG "Calling handler thread ... ");
    if (pthread_create(&picothread, NULL, handle_conn, &conn) != 0) {
      ERROR(TAG "Failed to create worker thread");
      close(picofd);
    }
  }

clean_up:
  WARNING(TAG "Socket clean up ...");
  db_close();
  log_close();
  close(fd);
  return 0;

cleanup_error:
  WARNING(TAG "Cleanup after error ... ");
  db_close();
  log_close();
  close(fd);
  return 1;
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
  while (picoconn->alive) {
    int n = read_pico_data(picofd, &picodata);
    if (n == 1) {
      if (picoconn->uid[0] == '\0') memcpy(&picoconn->uid, &picodata.uid, MAX_UID_LEN);
      INFO(TAG "received [%d] from client %s:%d (%s)", n, ip, port, picoconn->uid);
      if (!picoconn->ismanaged && picoconn->uid[0] != '\0') {
        if (pthread_create(&control_handle, NULL, pico_control, picoconn) != 0) {
          ERROR_ERRNO(TAG "failed to create pico_control for client %s:%d (%s)", ip, port, picoconn->uid);
          picoconn->uid[0] = '\0';
        } else picoconn->ismanaged = true;
      }
      log_pico_data(picodata);
      db_log(&picodata);
    } else {
      ERROR(TAG "failed to read from client %s:%d (%s)", ip, port, picoconn->uid);
      picoconn->alive = false;
      break;
    }

    pico_command_t cmd = {
      .magic = PACKET_MAGIC,
      .type = CMD_PING,
      .sleep_n_sec = 10
    };
    send_pico_cmd(picofd, &cmd);
  }

  INFO(TAG "Cleanup on clent %s:%d", ip, port);
  close(picofd);
  return NULL;
}

void* pico_control(void* conn) {
  conn_t* picoconn = (conn_t*) conn;
  char ip[INET_ADDRSTRLEN] = {0};
  struct sockaddr_in picoaddr = picoconn->addr;
  inet_ntop(AF_INET, &picoaddr.sin_addr, ip, INET_ADDRSTRLEN);
  uint16_t port = ntohs(picoaddr.sin_port);

  INFO("starting control on client %s:%d (%s)", ip, port, picoconn->uid);
  char path[sizeof(UNIX_SOCK_PATH) + MAX_UID_LEN + 3];
  snprintf(path, sizeof(path), "%s.%s", UNIX_SOCK_PATH, picoconn->uid);
  INFO("unix sock path '%s' client %s:%d (%s)", path, ip, port, picoconn->uid);

  WARNING("stopping control on client %s:%d (%s)", ip, port, picoconn->uid);
  picoconn->ismanaged = true;
  return NULL;
}

int read_pico_data(int picofd, pico_dat_t* data) {
  int n = sizeof(pico_dat_t);
  uint8_t *p = (uint8_t*) data;
  
  struct timeval tv = { .tv_sec = 10 };
  setsockopt(picofd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  
  while (n) {
    int r = recv(picofd, p, n, 0);
    if (r == 0) return 0;
    if (r < 0) {
      ERROR(TAG "recv failed: %d", r);
      return -1;
    }
    p += r;
    n -= r;
  }
  return 1;
}

int send_pico_cmd(int picofd, pico_command_t* cmd) {
  int n;
  if ((n = send(picofd, cmd, sizeof(pico_command_t), 0)) <= 0)
    ERROR(TAG "send_pico_cmd failed: %d", n);
  return n;
}

static void log_pico_data(pico_dat_t picodata) {
  DEBUG(TAG "\n==== pico data ====\n"
       "  uptime: %d\n"
       "  heap_free: %d\n"
       "  heap_min_free: %d\n"
       "  uid: %s\n"
       "  battery_mv: %d\n"
       "  dist: {%f, %f, %f}\n"
       "===================\n",
       picodata.uptime, picodata.heap_free, picodata.heap_min_free, picodata.uid, picodata.battery_mv, picodata.dist[0], picodata.dist[1], picodata.dist[2]);
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

