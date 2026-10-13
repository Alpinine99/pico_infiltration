#pragma once

#include "common_types.h"
#include "conn_types.h"
#include "logger.h"
#include "config.h"

void* control_pico_thread(void* conn);
int control_pico_send_cmd(int picofd, pico_command_t* cmd);
int control_pico_read_data(int picofd, pico_dat_t* data);
void control_pico_log_dat(pico_dat_t picodata);

