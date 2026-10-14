#pragma once 

#include <netinet/in.h>
#include <stdatomic.h>
#include <stdint.h>

#include "common_types.h"
#include "config.h"


typedef struct {
  int fd;
  atomic_bool alive;
  atomic_bool ismanaged;
  char uid[MAX_UID_LEN];
  struct sockaddr_in addr;
} conn_t;

typedef struct {
  size_t size;
  size_t max;
  conn_t* conns;
} conn_arr_t;

