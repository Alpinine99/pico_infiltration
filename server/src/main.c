#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <time.h>
#include <string.h>
#include "common_types.h"


#ifndef NDEBUG
#define DEBUG(fmt, ...) printf("DEBUG: \033[34;1m" fmt "\033[0m\n" __VA_OPT__(,) __VA_ARGS__)
#else
#define DEBUG(...)
#endif

#define ERROR(fmt, ...) \
  fprintf(stderr, "ERROR: \033[31;1m" fmt "\033[0m\n" __VA_OPT__(,) __VA_ARGS__); \
  perror("main")

#define INFO(fmt, ...) printf("INFO: \033[1m" fmt "\033[0m\n" __VA_OPT__(,) __VA_ARGS__)
#define WARNING(fmt, ...) fprintf(stderr, "WARNING: \033[33;1m" fmt "\033[0m\n" __VA_OPT__(,) __VA_ARGS__)

#define PORT 9090
#define MAX_CONN 4
#define SOCK_READ_TIMEOUT 1000 * 10

typedef struct {
  int fd;
  struct sockaddr_in addr;
} conn_t;


void* handle_conn(void*);
int read_pico_data(int picofd, pico_dat_t* data);
int send_pico_cmd(int picofd, pico_command_t* cmd);
static void log_pico_data(pico_dat_t picodata);

int main(void) {
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

  DEBUG("Creating socket ...");
  if ((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
    ERROR("Socket creation failed");
    return 1;
  }

  DEBUG("Setting SO_REUSEADDR sock opt ... ");
  if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
    WARNING("SO_REUSEADDR failed");
  }

  DEBUG("Binding socket ... ");
  if (bind(fd, (struct sockaddr*) &sock, sizeof(sock)) < 0) {
    ERROR("Socket bind failed");
    goto cleanup_error;
  }
  
  DEBUG("Listening for %d connections ... ", MAX_CONN);
  if (listen(fd, MAX_CONN) < 0) {
    ERROR("Listen failed");
    goto cleanup_error;
  }

  DEBUG("Waiting for connections ... ");
  INFO("server running on 0.0.0.0:%d ...", PORT);
  while (true) {
    INFO("Awaiting connection ... ");
    if ((picofd = accept(fd, (struct sockaddr*) &picoaddr, &addrlen)) < 0) {
      ERROR("Accept failed");
      continue;
    }
    DEBUG("New connection established");
    conn.fd = picofd;
    conn.addr = picoaddr;
   
    DEBUG("Calling handler thread ... ");
    if (pthread_create(&picothread, NULL, handle_conn, &conn) != 0) {
      ERROR("Failed to create worker thread");
      close(picofd);
    }
  }

clean_up:
  DEBUG("Socket clean up ...");
  close(fd);
  return 0;

cleanup_error:
  DEBUG("Cleanu up after error ... ");
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
  INFO("Connection from %s:%d", ip, port);

  pico_dat_t picodata;
  while (true) {
    int n = read_pico_data(picofd, &picodata);
    if (n == 1) {
      INFO("received [%d] from client %s:%d", n, ip, port);
      log_pico_data(picodata);
    } else {
      ERROR("failed to read from client %s:%d", ip, port);
      goto cleanup_pico;
    }

    // TODO: Log to database
    
    pico_command_t cmd = {
      .magic = PACKET_MAGIC,
      .type = CMD_PING,
      .sleep_n_sec = 10
    };
    send_pico_cmd(picofd, &cmd);
  }

cleanup_pico:
  DEBUG("Cleanup on clent %s:%d", ip, port);
  close(picofd);
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
      ERROR("recv failed: %d", r);
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
    ERROR("send_pico_cmd failed: %d", n);
  return n;
}

static void log_pico_data(pico_dat_t picodata) {
  INFO("\n==== pico data ====\n"
       "  uptime: %d\n"
       "  heap_free: %d\n"
       "  heap_min_free: %d\n"
       "  uid: %s\n"
       "  battery_mv: %d\n"
       "  dist: {%f, %f, %f}\n"
       "===================\n",
       picodata.uptime, picodata.heap_free, picodata.heap_min_free, picodata.uid, picodata.battery_mv, picodata.dist[0], picodata.dist[1], picodata.dist[2]);
}

