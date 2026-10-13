#include "control.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/un.h>
#include <strings.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdbool.h>


#define TAG "control: "

void* control_pico_thread(void* conn) {
  conn_t* picoconn = (conn_t*) conn;
  char ip[INET_ADDRSTRLEN] = {0};
  struct sockaddr_in picoaddr = picoconn->addr;
  inet_ntop(AF_INET, &picoaddr.sin_addr, ip, INET_ADDRSTRLEN);
  uint16_t port = ntohs(picoaddr.sin_port);

  INFO(TAG "starting control on client %s:%d (%s)", ip, port, picoconn->uid);
  char path[sizeof(UNIX_SOCK_PATH) + MAX_UID_LEN + 3];
  snprintf(path, sizeof(path), "%s.%s", UNIX_SOCK_PATH, picoconn->uid);
  INFO(TAG "unix sock path '%s' client %s:%d (%s)", path, ip, port, picoconn->uid);

  int fd, clientfd, readbytes, picofd = picoconn->fd;
  struct sockaddr_un s_addr;
  memset(&s_addr, 1, sizeof(s_addr));
  s_addr.sun_family = AF_UNIX;
  strncpy(s_addr.sun_path, path, sizeof(s_addr.sun_path));
  uint32_t sleep_s = 0;
  char buf[UNIX_SOCK_BF];
  char *token;
  const char *delim = " \t\r\n";
  char buf_ok[] = "OK: querry ran successfully";
  char buf_pico_err[] = "ERR: failed to send querry to pico";
  char buf_err[] = "ERR: invalid input";
  pico_command_t pico_cmd;

  if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
    ERROR(TAG "error creating unix socket, client %s:%d (%s)", ip, port, picoconn->uid);
    goto unix_cleanup;
  }

  unlink(path); // from premature shutdown
  if (bind(fd, (struct sockaddr*) &s_addr, sizeof(s_addr)) < 0) {
    ERROR(TAG "error binding unix socket, client %s:%d (%s)", ip, port, picoconn->uid);
    goto unix_cleanup;
  }

  if (listen(fd, 1) < 0) {
    ERROR(TAG "error listenning on unix socket, client %s:%d (%s)", ip, port, picoconn->uid);
    goto unix_cleanup;
  }

  INFO(TAG "Unix socket listening on path '%s' client %s:%d (%s)", path, ip, port, picoconn->uid);
  while (picoconn->alive) {
    if ((clientfd = accept(fd, NULL, NULL)) < 0) {
      WARNING(TAG "invalid unix connection client %s:%d (%s)", ip, port, picoconn->uid);
      continue;
    }
    INFO(TAG "connection on unix path client %s:%d (%s)", ip, port, picoconn->uid);
    if ((readbytes = read(clientfd, buf, sizeof(buf))-1) <= 0) {
      WARNING(TAG "read %d bytes unix path on client %s:%d (%s)", readbytes, ip, port, picoconn->uid);
      write(clientfd, buf_err, sizeof(buf_err));
    } else {
      buf[readbytes] = '\0';
      INFO(TAG "query '%s' unix path client %s:%d (%s)", buf, ip, port, picoconn->uid);
      token = strtok(buf, delim);
      if (strncasecmp(token, REBOOT_CMD, sizeof(REBOOT_CMD)) == 0) {
        WARNING(TAG "rebooting pico client %s:%d (%s)", ip, port, picoconn->uid);
        pico_cmd.magic = PACKET_MAGIC;
        pico_cmd.type = CMD_REBOOT;
        pico_cmd.sleep_n_sec = 0;
        if (control_pico_send_cmd(picofd, &pico_cmd) <= 0)
          write(clientfd, buf_pico_err, sizeof(buf_pico_err));
        else
          write(clientfd, buf_ok, sizeof(buf_ok));
      } else if (strncasecmp(token, SLEEP_CMD, sizeof(SLEEP_CMD)) == 0) {
        token = strtok(NULL, delim);
        if ((sleep_s = atoi(token)) <= 0) {
          ERROR(TAG "invalid sleep value from command client %s:%d (%s)", ip, port, picoconn->uid);
          write(clientfd, buf_err, sizeof(buf_err));
        } else {
          WARNING(TAG "sending sleep(%d) to pico client %s:%d (%s)", sleep_s, ip, port, picoconn->uid);
          pico_cmd.magic = PACKET_MAGIC;
          pico_cmd.type = CMD_SLEEP;
          pico_cmd.sleep_n_sec = sleep_s;
          if (control_pico_send_cmd(picofd, &pico_cmd) <= 0)
            write(clientfd, buf_pico_err, sizeof(buf_pico_err));
          else
            write(clientfd, buf_ok, sizeof(buf_ok));
        }
      } else {
        ERROR(TAG "invalid command from command client %s:%d (%s)", ip, port, picoconn->uid);
        write(clientfd, buf_err, sizeof(buf_err));
      }
    }
    memset(buf, 0, sizeof(buf));
    close(clientfd);
  }

unix_cleanup:
  WARNING(TAG "control stopped on client %s:%d (%s)", ip, port, picoconn->uid);
  picoconn->ismanaged = false;
  unlink(path);
  close(fd);
  return NULL;
}

int control_pico_send_cmd(int picofd, pico_command_t* cmd) {
  int n;
  if ((n = send(picofd, cmd, sizeof(pico_command_t), 0)) <= 0)
    ERROR(TAG "control_pico_send_cmd failed: %d", n);
  else INFO(TAG "control_pico_send_cmd sent pico command");
  return n;
}

int control_pico_read_data(int picofd, pico_dat_t* data) {
  int n = sizeof(pico_dat_t);
  uint8_t *p = (uint8_t*) data;
  
  struct timeval tv = { .tv_sec = 10 };
  setsockopt(picofd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  
  while (n) {
    int r = recv(picofd, p, n, 0);
    if (r == 0) return 0;
    if (r < 0) {
      ERROR(TAG "recv failed: %d", r);
      return r;
    }
    p += r;
    n -= r;
  }
  return sizeof(pico_dat_t);
}

void control_pico_log_dat(pico_dat_t picodata) {
  DEBUG(TAG "\n==== pico data ====\n"
       "  uptime: %d\n"
       "  heap_free: %d\n"
       "  heap_min_free: %d\n"
       "  uid: %s\n"
       "  battery_mv: %d\n"
       "  dist: {%f, %f, %f}\n"
       "===================\n",
       picodata.uptime, picodata.heap_free, picodata.heap_min_free, picodata.uid, picodata.battery_mv, picodata.dist[0], picodata.dist[1], picodata.dist[2]);
}

