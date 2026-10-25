# Pico Infiltration

A room-presence monitor. One or more **Raspberry Pi Pico W** boards with ultrasonic
distance sensors report readings over Wi-Fi to a **C server**, which stores them in
SQLite. A **Django** dashboard (served by gunicorn behind nginx) shows live charts,
exports data, and can **reboot** or **sleep** a Pico remotely.

```
 Pico W  (FreeRTOS firmware, up to 3 x HC-SR04)
   |
   |  TCP :65254   52-byte binary packet every UPDATE_TIME_S seconds
   v
 pico-server  (C, systemd: infil)  --writes-->  /var/www/infil/pico_db.sqlite3
   ^                                                      |
   |  unix socket /run/infil/control.sock.<pico id>       | read-only
   |  ("REBOOT" / "SLEEP <seconds>")                      v
   +------------------------------  Django  (gunicorn, systemd: infilUI)
                                                          ^
                                         unix socket      |
                                                       nginx :4499  <--  browser
```

| Port / path | What | Direction |
|---|---|---|
| `65254/tcp` | C server, receives Pico data | Pico -> server |
| `4499/tcp` | nginx, serves the dashboard | browser -> server |
| `/pico-infil-live-data/` | dashboard URL | |
| `/run/infil/control.sock.<id>` | per-Pico control socket, created while that Pico is connected | Django -> C server |

## Repository layout

```
pico_infiltration/
  common/include/common_types.h   packet format shared by firmware and server
  pico/                           Pico W firmware (CMake + Pico SDK + FreeRTOS)
    build.sh  build.env.example   build helper and its configuration template
  server/                         C server (CMake, needs libsqlite3)
    src/ include/ tests/
    infil.service                 systemd unit
    fail2ban/                     bans clients that send invalid packets
  web/                            Django project (settings in web/web/, app in web/main/)
    infilUI.service  nginx/  fail2ban/  .env.example  requirements.txt
  configure                       one-shot installer (Debian/Ubuntu, run as root)
  cleanup                         uninstaller
```

---

## 1. Pico firmware

Build this on your workstation, not on the server.

### 1.1 Toolchain (Debian/Ubuntu)

```bash
sudo apt install build-essential git python3 cmake \
     gcc-arm-none-eabi libnewlib-arm-none-eabi libstdc++-arm-none-eabi-newlib
```

CMake **3.23 or newer** is required. Ubuntu 22.04 packages 3.22, so use
`pip install cmake` or the Kitware apt repository there.

### 1.2 Pico SDK and FreeRTOS

```bash
mkdir -p ~/pico && cd ~/pico
git clone --recurse-submodules https://github.com/raspberrypi/pico-sdk.git
git clone https://github.com/FreeRTOS/FreeRTOS-Kernel.git

export PICO_SDK_PATH=$HOME/pico/pico-sdk
export FREERTOS_KERNEL_PATH=$HOME/pico/FreeRTOS-Kernel
```

Put the two `export` lines in your shell profile. The build reads them from the
environment. The firmware expects the RP2040 port at
`$FREERTOS_KERNEL_PATH/portable/ThirdParty/GCC/RP2040`; confirm it exists:

```bash
ls "$FREERTOS_KERNEL_PATH/portable/ThirdParty/GCC/RP2040/FreeRTOS_Kernel_import.cmake"
```

If that file is missing, check out a FreeRTOS-Kernel release that includes the port.

### 1.3 Configure

```bash
cd pico_infiltration/pico
cp build.env.example build.env
$EDITOR build.env
```

| Variable | Meaning |
|---|---|
| `ROOM_LOCATION` | Label for this board, e.g. `south entrance`. It becomes part of the device id shown in the dashboard (`<location>.<board id>`). The id is limited to 23 characters, so keep the label under about 18. |
| `WIFI_SSID`, `WIFI_PASSWORD` | A **2.4 GHz** WPA2 (AES) network. The Pico W has no 5 GHz radio. |
| `SERVER_ADDRESS` | IPv4 address of the machine running `pico-server`. |
| `SERVER_PORT` | Must be **`65254`** (`PORT` in `server/include/config.h`). |
| `NLOGGING` | `0`: log to USB serial (the board waits up to 10 s at boot for a serial terminal). `1`: no logging, no USB serial. Use `1` for deployed boards. |
| `UPDATE_TIME_S` | Seconds between reports to the server. |
| `REACTION_TIME_MS` | Averaging window applied to the raw sensor readings. |
| `MAX_SENSORS` | Number of sensors (max 3, since a packet carries three distances). |
| `MAX_DIST_CM` | Echoes beyond this distance count as "no reading". |

`WIFI_SSID`, `SERVER_ADDRESS` and `ROOM_LOCATION` are mandatory. CMake stops with an
error if any is missing.

Each report contains the **lowest** averaged distance seen per sensor during the interval.
`-1` means no echo, and the server stores it as `NULL`.

### 1.4 Wiring

Sensor pins are set in `pico/src/main.cc`:

| Sensor | TRIG | ECHO |
|---|---|---|
| 1 | GP19 (pin 25) | GP16 (pin 21) |
| 2 | GP20 (pin 26) | GP17 (pin 22) |
| 3 | GP21 (pin 27) | GP18 (pin 24) |

Power the sensors from **VBUS** (pin 40, 5 V) and any GND pin.

> **Warning:** a standard HC-SR04 drives ECHO at 5 V, but Pico GPIOs are 3.3 V only.
> Put a voltage divider (for example 1 kΩ + 2 kΩ) on every ECHO line, or use a
> 3.3 V-compatible sensor such as the HC-SR04P.

Battery voltage is read from VSYS and reported in millivolts.

### 1.5 Build and flash

```bash
./build.sh            # configure + build
./build.sh afresh     # wipe build/ first (do this after editing build.env)
./build.sh clean      # just delete build/
```

The output is `build/src/infil.uf2`. To flash it, hold **BOOTSEL** while plugging in
the USB cable, wait for the `RPI-RP2` drive, and copy the file onto it. The board
reboots by itself. With `picotool` installed: `picotool load -f build/src/infil.uf2`.

With `NLOGGING=0` you can read the logs over USB:

```bash
screen /dev/ttyACM0 115200
```

LED patterns: a short blink every ~15 s means the board is alive; fast blinking
means an error (Wi-Fi or server connection failed); a solid 3 s light means it is
about to sleep after a server command.

---

## 2. C server

`pico-server` listens on `65254`, validates packets, writes them to SQLite, and
exposes a control socket per connected Pico.

### 2.1 Dependencies

```bash
# Debian / Ubuntu
sudo apt install build-essential cmake libsqlite3-dev
# Void
sudo xbps-install base-devel cmake sqlite-devel      # the repo's `deps` file lists sqlite3-devel; check with: xbps-query -Rs sqlite
```

### 2.2 Build

```bash
cd pico_infiltration/server
mkdir -p build && cd build
cmake .. && make -j2
```

The server binary is `server/build/src/pico-server`.

### 2.3 Paths are compile-time constants

`server/include/config.h` hard-codes where the server reads and writes:

| Constant | Default |
|---|---|
| `PORT` | `65254` |
| `LOGFILE` | `/var/log/infil/update.log` |
| `DB_PATH` | `/var/www/infil/pico_db.sqlite3` |
| `UNIX_SOCK_PATH` | `/run/infil/control.sock` |
| `MAX_CONN` | `4` simultaneous Picos |

Change them, rebuild, and keep the Django settings in step (`DB_PATH`,
`PICO_CONTROL_ADDR`, `PICO_SERVER_LOG` in `web/web/settings.py`).

### 2.4 Run it by hand

```bash
sudo mkdir -p /var/log/infil /var/www/infil /run/infil
sudo chown "$USER" /var/log/infil /var/www/infil /run/infil
./build/src/pico-server
```

Stop it with Ctrl-C. Logs go to `/var/log/infil/update.log`.

To test the control channel while a Pico is connected (the trailing newline matters):

```bash
printf 'SLEEP 30\n' | socat - UNIX-CONNECT:"/run/infil/control.sock.<pico id>"
```

> **Careful with the test programs.** `build/tests/db_test` and `build/tests/control_test`
> are standalone executables, not registered with `ctest`. They use the real paths from
> `config.h`, and `db_test` inserts ten fake rows (`south coridor`) into `DB_PATH`.
> Point `config.h` at scratch paths before running them, never on a live database.

---

## 3. Django backend

### 3.1 Local development

```bash
cd pico_infiltration/web
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt         # Django, gunicorn, python-dotenv
cp .env.example .env                    # development values
python3 manage.py migrate
python3 manage.py collectstatic --noinput
python3 manage.py runserver
```

Open <http://127.0.0.1:8000/pico-infil-live-data/>.

Settings are read from the environment, and `web/.env` is loaded automatically by
`settings.py` (real environment variables win over the file):

| Variable | Purpose |
|---|---|
| `DJANGO_SECRET_KEY` | Required; the app refuses to start without it. Generate one: `python3 -c "import secrets; print(secrets.token_urlsafe(50))"` |
| `DJANGO_DEBUG` | `1` for development, `0` in production |
| `DJANGO_ALLOWED_HOSTS` | Comma-separated hostnames/IPs the site answers to |
| `DJANGO_CSRF_TRUSTED_ORIGINS` | Comma-separated origins **with scheme and port**, e.g. `https://pico.example.com:4499` |

**Two databases.** `default` is a small `db.sqlite3` for Django's own tables. `pico` is the
C server's database, opened **read-only**. Django never creates or alters the
`readings` table. Its location is hard-coded as `DB_PATH` in `web/web/settings.py`.

To develop without a live server, point `DB_PATH` at a scratch file and seed it:

```bash
python3 - <<'EOF'
import sqlite3, time
c = sqlite3.connect("/tmp/pico_db.sqlite3")
c.executescript("""
CREATE TABLE IF NOT EXISTS readings(
  id INTEGER PRIMARY KEY, ts INTEGER NOT NULL, device TEXT NOT NULL,
  uptime_s INTEGER, heap_free INTEGER, heap_min_free INTEGER,
  battery_mv INTEGER, d0 REAL, d1 REAL, d2 REAL);
CREATE INDEX IF NOT EXISTS idx_dev_ts ON readings(device, ts);
""")
now = int(time.time())
for i in range(20):
    c.execute("INSERT INTO readings(ts,device,uptime_s,heap_free,heap_min_free,battery_mv,d0,d1,d2)"
              " VALUES(?,?,?,?,?,?,?,?,?)",
              (now - 200 + i * 10, "south entrance.e661", 100 + i * 10, 90000, 80000, 4100,
               120 + i, None, 88.0))
c.commit()
EOF
```

Reboot and sleep buttons need the C server's control socket, so without it they return
`socket does not exist: /run/infil/control.sock.<id>`. That is expected in development.

### 3.2 Production settings

Create `web/.env` with production values **before** installing (see section 4):

```bash
DJANGO_SECRET_KEY=<generated key>
DJANGO_DEBUG=0
DJANGO_ALLOWED_HOSTS=pico.example.com
DJANGO_CSRF_TRUSTED_ORIGINS=https://pico.example.com:4499
```

With `DJANGO_DEBUG=0` the CSRF and session cookies are marked **Secure**, so browsers
only keep them over HTTPS. Reboot/sleep (POST requests) need that cookie, so enable
TLS in nginx (section 4.3). Never commit `.env`.

### 3.3 Static files

nginx serves static files itself from `web/appstatic/`:

```bash
python3 manage.py collectstatic --noinput
```

Run it after every change to `web/static/`. The dashboard will look unstyled
without it.

---

## 4. Deployment (Debian/Ubuntu with systemd)

### 4.1 Prerequisites

```bash
sudo apt install nginx fail2ban ufw python3-venv python3-pip build-essential
```

plus CMake 3.23 or newer (see 1.1). The `configure` script installs `libsqlite3-dev`,
`cmake` and `make` itself, but not the packages above.

### 4.2 Install

1. Create `web/.env` in your checkout (section 3.2). The installer copies it along
   with everything else; without it the web service fails to start.
2. Make sure `web/requirements.txt` lists `python-dotenv` (settings imports it).
3. Run the installer from the repository root:

   ```bash
   sudo ./configure
   ```

   `collectstatic` stops and asks for confirmation, so type `yes` when prompted.

What it does:

1. Installs build dependencies and copies the whole tree to `/var/www/infil`.
2. Builds the C server in `/var/www/infil/server/build`.
3. Links the fail2ban filter and jail, then links and starts `infil.service`.
4. Creates `/var/www/infil/web/.venv`, installs requirements, runs `migrate` and
   `collectstatic`.
5. Links the nginx site, the `infilUI` fail2ban files and `infilUI.service`, starts
   everything and restarts nginx.
6. Opens ports `65254/tcp` and `4499/tcp` in ufw.

### 4.3 Where things live

| Component | Installed copy (edit these) | Runs as |
|---|---|---|
| C server | `/var/www/infil/server/`, unit `infil.service` | `www-data` |
| Django | `/var/www/infil/web/`, unit `infilUI.service`, socket `/run/infilUI/infil.sock` | `www-data` |
| nginx site | `/var/www/infil/web/nginx/sites-available/infil.conf` (symlinked into `sites-enabled`) | nginx |
| Database | `/var/www/infil/pico_db.sqlite3` | written by C server |
| Logs | `/var/log/infil/update.log` (C server), `/var/log/infilUI/` (nginx access/error); gunicorn output goes to `journalctl -u infilUI` | |

The systemd units and fail2ban files are **symlinks into the installed copy**, so edit
them under `/var/www/infil`, not in your git checkout. Re-running `./configure`
overwrites that copy.

**Enable HTTPS** by uncommenting the `ssl` lines in
`/var/www/infil/web/nginx/sites-available/infil.conf`, replacing `listen 4499;` with
`listen 4499 ssl;` and setting the certificate paths. Then:

```bash
sudo nginx -t && sudo systemctl reload nginx
```

### 4.4 Verify

```bash
systemctl status infil infilUI
sudo tail -f /var/log/infil/update.log          # a Pico's packets show up here
journalctl -u infilUI -n 30 --no-pager
curl -s -o /dev/null -w '%{http_code}\n' http://127.0.0.1:4499/pico-infil-live-data/    # expect 200
```

Then open `http://<server>:4499/pico-infil-live-data/` (or `https://` if enabled).
Only that path and `/static/` are proxied; every other URL (including `/` and
`/admin/`) returns 404 by design.

### 4.5 Update, uninstall

```bash
git pull && sudo ./configure                    # rebuilds and redeploys; the database is kept
sudo chown -R www-data:www-data /var/www/infil
```

```bash
sudo cp /var/www/infil/pico_db.sqlite3 ~/pico_db.backup   # back up first!
sudo ./cleanup
```

---

## 5. Security notes

- **The dashboard has no login.** Anyone who can reach port 4499 can read all data and
  send reboot/sleep commands. Restrict it before exposing it: put it behind a VPN or
  an allow-list, or add basic auth in the nginx `location` blocks:

  ```nginx
  auth_basic           "pico";
  auth_basic_user_file /etc/nginx/pico.htpasswd;     # htpasswd -c /etc/nginx/pico.htpasswd you
  ```

- **Port 65254 is unauthenticated.** The Pico protocol only checks a magic number
  (`0xA55A`). Anyone who can reach the port can submit fake readings. Limit the
  firewall rule to your Wi-Fi subnet: `sudo ufw allow from 192.168.100.0/24 to any port 65254 proto tcp`.
- **fail2ban** is preconfigured twice. `infil` bans hosts that send repeated invalid
  packets to 65254. `infilUI` bans hosts producing **three 404s within a minute** on the
  dashboard port. Opening `http://<server>:4499/` (a 404) counts toward that ban. Unban
  yourself with `sudo fail2ban-client set infilUI unbanip <your ip>`.
- **Secrets in the firmware.** `build.env` holds your Wi-Fi password and it is compiled
  into the firmware image. Keep `build.env` and the `.uf2` files private (`build.env` is
  already in `.gitignore`).

---

## 6. Troubleshooting

| Symptom | Cause and fix |
|---|---|
| `ImproperlyConfigured: Set the DJANGO_SECRET_KEY ...` | `web/.env` is missing or empty in `/var/www/infil/web/`. |
| `ModuleNotFoundError: No module named 'dotenv'` | `python-dotenv` is not installed. Add it to `requirements.txt` and `pip install -r requirements.txt` in the venv. |
| `infilUI` exits with `unrecognized arguments: --control-socket` | gunicorn is too old for that flag. `pip install -U gunicorn` in the venv. |
| gunicorn log: `Control server error ... Permission denied` | The unit passes `--control-socket /run/infilUI/gunicorn.ctl`; make sure `RuntimeDirectory=infilUI` is present, or add `--no-control-socket`. |
| Browser shows `Bad Request (400)` | The hostname you used is not in `DJANGO_ALLOWED_HOSTS`. |
| Reboot/sleep returns 403 (CSRF) | `DJANGO_CSRF_TRUSTED_ORIGINS` must match scheme, host and port exactly; with `DEBUG=0` you also need HTTPS (cookies are Secure). |
| Dashboard unstyled | Run `collectstatic`, and check `/var/www/infil/web/appstatic` is readable by `www-data`. |
| Dashboard empty | No Pico has reported yet. Check `update.log`, then `sqlite3 /var/www/infil/pico_db.sqlite3 'select count(*) from readings;'`. |
| `socket does not exist: /run/infil/control.sock.<id>` | That Pico is not currently connected, or the `infil` service is down. The socket exists only while a Pico is online. |
| Pico never shows up | `SERVER_PORT` is not `65254`; wrong `SERVER_ADDRESS`; firewall; Pico and server not reachable on the same network. Rebuild with `NLOGGING=0` and read the serial log. |
| Firmware build: `Error make sure to set ENVs ...` | `build.env` is missing, or you are not building through `./build.sh`. |
| CMake: `Cannot find ... pico_sdk_import.cmake` | `PICO_SDK_PATH` or `FREERTOS_KERNEL_PATH` is not exported in this shell. |
| Server build: `sqlite3.h: No such file` | Install `libsqlite3-dev` (Debian/Ubuntu) or the Void equivalent. |
| `Permission denied` opening the log or database | The directory is not owned by the user running `pico-server`. Check `ls -ld /var/log/infil /var/www/infil`. |

