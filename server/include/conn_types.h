#pragma once 

#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include "config.h"


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

