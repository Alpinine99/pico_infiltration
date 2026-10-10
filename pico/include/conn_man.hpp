#pragma once

#include <lwip/sockets.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <string.h>
#include "common_types.h"

#if NLOGGING == 1
#define ERROR(...)
#else
#define ERROR(fmt, ...) printf("ERROR: " fmt "\n", ##__VA_ARGS__); \
  perror("Error")
#endif


class Conn_man {
private:
  int fd;
  struct sockaddr_in addr;
  char ip[INET_ADDRSTRLEN] = {0};
  uint16_t port;
  struct timeval tv = { .tv_sec = 15 };
  bool is_connected = false;

public:
  Conn_man(const char *ip, uint16_t port);
  bool s_connect();
  bool s_send(pico_dat_t* data);
  bool s_is_connected();
  int s_read(pico_command_t* cmd);
  void s_close();
  ~Conn_man();
};

