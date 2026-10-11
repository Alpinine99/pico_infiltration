#include "db.h"
#include "config.h"

#define TAG "db: "

static sqlite3      *db;
static sqlite3_stmt *ins;

int db_open(const char *path) {
  INFO(TAG "opening db at: %s", path);
  if (sqlite3_open(path, &db) != SQLITE_OK) {
    ERROR_ERRNO(TAG "open: %s\n", sqlite3_errmsg(db));
    return -1;
  }
  DEBUG(TAG "Waiting on db for any holding processes");
  sqlite3_busy_timeout(db, 5000); // wait if another process holds the lock

  const char *schema =
    "PRAGMA journal_mode=WAL;"        // readers don't block the writer
    "PRAGMA synchronous=NORMAL;"
    "CREATE TABLE IF NOT EXISTS readings("
    "  id INTEGER PRIMARY KEY,"
    "  ts INTEGER NOT NULL,"          // server receive time, unix seconds
    "  device TEXT NOT NULL,"
    "  uptime_s INTEGER, heap_free INTEGER, heap_min_free INTEGER,"
    "  battery_mv INTEGER,"
    "  d0 REAL, d1 REAL, d2 REAL);"
    "CREATE INDEX IF NOT EXISTS idx_dev_ts ON readings(device, ts);";
  char *err = NULL;

  DEBUG(TAG "Running schema:\n`%s`\n", schema);
  if (sqlite3_exec(db, schema, NULL, NULL, &err) != SQLITE_OK) {
    ERROR_ERRNO(TAG "schema: %s\n", err);
    sqlite3_free(err);
    return -1;
  }

  // Prepare once, reuse for every frame
  const char *sql =
    "INSERT INTO readings(ts,device,uptime_s,heap_free,heap_min_free,battery_mv,d0,d1,d2)"
    " VALUES(?,?,?,?,?,?,?,?,?)";

  DEBUG(TAG "preparing a query for reuse");
  if (sqlite3_prepare_v2(db, sql, -1, &ins, NULL) != SQLITE_OK) {
    ERROR_ERRNO(TAG "prepare: %s\n", sqlite3_errmsg(db));
    return -1;
  }
  return 0;
}

int db_log(const pico_dat_t *p) {
  char dev[sizeof p->uid + 1];
  memcpy(dev, p->uid, sizeof p->uid);
  dev[sizeof p->uid] = '\0';

  INFO(TAG "Writing to database");
  sqlite3_bind_int64(ins, 1, (sqlite3_int64) time(NULL));
  sqlite3_bind_text (ins, 2, dev, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(ins, 3, p->uptime);
  sqlite3_bind_int64(ins, 4, p->heap_free);
  sqlite3_bind_int64(ins, 5, p->heap_min_free);
  sqlite3_bind_int  (ins, 6, p->battery_mv);
  for (int i = 0; i < 3; i++) {
    float d = p->dist[i];
    if (d < 0) sqlite3_bind_null(ins, 7 + i);     // -1 = no echo, store as NULL
    else       sqlite3_bind_double(ins, 7 + i, d);
  }

  int rc = sqlite3_step(ins);
  if (rc != SQLITE_DONE) ERROR_ERRNO(TAG "insert: %s\n", sqlite3_errmsg(db));
  sqlite3_reset(ins);                    // required before the statement is reused
  sqlite3_clear_bindings(ins);
  return rc == SQLITE_DONE ? 0 : -1;
}

void db_close(void) {
  INFO(TAG "Closing database");
  sqlite3_finalize(ins);
  sqlite3_close(db);
}

