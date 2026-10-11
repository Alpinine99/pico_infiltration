#pragma once

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sqlite3.h>
#include "common_types.h"      // pico_dat_t

int db_open(void); 
int db_log(const pico_dat_t *p); 
void db_close(void);

