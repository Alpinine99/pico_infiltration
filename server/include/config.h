#pragma once

#define PORT 9090
#define MAX_CONN 4
#define SOCK_READ_TIMEOUT (1000 * 10)
#define LOGFILE "../server.log"
#define DB_PATH "../pico_db.sqlite3"
// #define UNIX_SOCK_PATH "/run/infil/control.sock"
#define UNIX_SOCK_PATH "../control.sock"

