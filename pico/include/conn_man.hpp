#pragma once

#include <lwip/sockets.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <string.h>
#include "common_types.h"

class Conn_man {
private:
  int fd;
  struct sockaddr_in addr;
  char ip[INET_ADDRSTRLEN] = {0};
  uint16_t port;
  struct timeval tv = { .tv_sec = 15 };
  bool is_connected = false;
  int n_misses = 0, n_missed_max = 15;

public:
  Conn_man(const char *ip, uint16_t port);
  bool s_connect(void);
  bool s_send(pico_dat_t* data);
  bool s_is_connected(void);
  int s_read(pico_command_t* cmd);
  void s_close(void);
  ~Conn_man(void);
};

