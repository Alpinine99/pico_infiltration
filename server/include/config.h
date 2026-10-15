#pragma once

#define PORT 65254
#define MAX_CONN 4
#define SOCK_READ_TIMEOUT (1000 * 10)
#define INVALID_PACKET_TOLERANCE 3

#define REBOOT_CMD "REBOOT"
#define SLEEP_CMD  "SLEEP"

#define LOGFILE "/var/log/infil/update.log"
#define DB_PATH "/var/www/infil/pico_db.sqlite3"
#define UNIX_SOCK_PATH "/run/infil/control.sock"
#define UNIX_SOCK_BF 256

