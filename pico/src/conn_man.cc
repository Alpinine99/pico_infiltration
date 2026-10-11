#include "conn_man.hpp"


Conn_man::Conn_man(const char *ip, uint16_t port) {
  snprintf(this->ip, INET_ADDRSTRLEN, "%s", ip);
  this->port = port;
}

bool Conn_man::s_connect() {
  if ((this->fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) < 0) {
    ERROR("Failed to create socket: %d", errno);
    this->is_connected = false;
    return false;
  }

  this->addr = {
    .sin_family = AF_INET,
    .sin_port = htons(port)
  };
  inet_aton(ip, &this->addr.sin_addr);

  if (connect(this->fd, (struct sockaddr*) &this->addr, sizeof(addr)) < 0) {
    ERROR("Failed to connect: %d", errno);
    close(this->fd);
    this->is_connected = false;
    return false;
  }

  setsockopt(this->fd, SOL_SOCKET, SO_RCVTIMEO, &this->tv, sizeof(this->tv));
  setsockopt(this->fd, SOL_SOCKET, SO_SNDTIMEO, &this->tv, sizeof(this->tv));
  this->is_connected = true;
  return true;
}

bool Conn_man::s_send(pico_dat_t* data) {
  if (!this->is_connected) {
    ERROR("Connection not established");
    return false;
  }
  if (send(fd, data, sizeof(pico_dat_t), 0) < 0) {
    ERROR("Failed to send: %d", errno);
    this->is_connected = false;
    return false;
  }
  return true;
}

int Conn_man::s_read(pico_command_t* cmd) {
  uint8_t *p = (uint8_t*) cmd;
  int n = sizeof(pico_command_t);
  while (n) {
    int r = recv(this->fd, p, n, 0);
    if (r == 0) return 0;
    else if (r < 0) {
      ERROR("s_read failed: %d", r);
      return -1;
    }
    p += r;
    n -= r;
  }
  return 1;
}

bool Conn_man::s_is_connected() {
  return this->is_connected;
}

void Conn_man::s_close() {
  this->is_connected = false;
  close(this->fd);
}

Conn_man::~Conn_man() {
  this->is_connected = false;
  close(this->fd);
}

