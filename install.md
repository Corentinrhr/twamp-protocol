# Deploying TWAMP on Ubuntu

This guide explains how to deploy the fork at `https://github.com/Corentinrhr/twamp-protocol` on an Ubuntu VM as a TWAMP reflector/server, using **TCP port 8862** for the TWAMP-Control connection and **UDP range 20000–30000** for TWAMP-Test sessions.

***

## Target Setup

The server listens on **TCP 8862** for the control session and allocates TWAMP test sockets from the **UDP range 20000–30000** (both bounds inclusive).
This matches the server logic where the server selects the effective UDP reflector port from its configured range and returns it in `Accept-Session.Port`.

***

## Install Dependencies

Install the build toolchain, Git, the firewall tool, and a few useful diagnostics utilities.

> **Note:** `build-essential` already pulls in `gcc` and `make`; they are not listed separately.

```bash
sudo apt update && sudo apt upgrade -y
sudo apt install -y \
  git \
  build-essential \
  ufw \
  net-tools \
  tcpdump \
  chrony
```

***

## Enable Time Synchronization

TWAMP measurements rely on accurate timestamps. Ubuntu ships with `systemd-timesyncd` enabled by default; disable it before installing `chrony` to avoid conflicts.

```bash
sudo systemctl disable --now systemd-timesyncd
sudo systemctl enable --now chrony
chronyc tracking
chronyc sources -v
```

***

## Clone the Fork and Compile

Clone the repository into `/opt`, then build with `make`.

```bash
cd /opt
sudo git clone https://github.com/Corentinrhr/twamp-protocol.git
sudo chown -R twamp:twamp /opt/twamp-protocol   # see "Create a Dedicated Service Account" below
cd /opt/twamp-protocol
sudo -u twamp make
ls -lh
```

> **Order note:** The `chown` above assumes the `twamp` user already exists. If you are following this guide from top to bottom, create the user first (next section), then come back to clone and build, or simply run `make` as your own user and re-run `chown` after the user is created.

If the Makefile does not build correctly, compile manually with GCC (using the exact flags from the Makefile):

```bash
sudo -u twamp gcc -Wall -Wextra -Werror -g -static \
  -o server server.c timestamp.c twamp.h
sudo -u twamp gcc -Wall -Wextra -Werror -g -static \
  -o client client.c timestamp.c twamp.h
```

***

## Create a Dedicated Service Account

The server code explicitly refuses to run as root. Create a system user with no home directory and no login shell, then assign ownership of the application directory to it.

```bash
sudo useradd --system \
  --no-create-home \
  --shell /usr/sbin/nologin \
  twamp

sudo chown -R twamp:twamp /opt/twamp-protocol
```

***

## Configure the Firewall

Open the SSH port, the TWAMP control port **8862/TCP**, and the full TWAMP test port range **20000–30000/UDP**.

```bash
sudo ufw allow 22/tcp
sudo ufw allow 8862/tcp
sudo ufw allow 20000:30000/udp
sudo ufw enable
sudo ufw status verbose
```

***

## Manual Validation Before systemd

Before creating the service, launch the server **as the `twamp` user** and confirm it is listening on TCP 8862.

```bash
cd /opt/twamp-protocol
sudo -u twamp ./server -p 20000 -q 30000 -c 8862
```

In another terminal:

```bash
ss -lntup | grep -E '8862|server'
netstat -tulnp | grep -E '8862|server'
```

> Running the server without `sudo -u twamp` would start it as your own user instead of `twamp`, which is inconsistent with the production service and may fail if the directory is already owned by `twamp`.

***

## Create the systemd Service

Create a persistent service so the reflector starts automatically after boot.

```bash
sudo tee /etc/systemd/system/twamp-server.service > /dev/null << 'EOF2'
[Unit]
Description=TWAMP Reflector Server
Documentation=https://github.com/Corentinrhr/twamp-protocol
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=twamp
Group=twamp
WorkingDirectory=/opt/twamp-protocol
ExecStart=/opt/twamp-protocol/server -p 20000 -q 30000 -c 8862
Restart=on-failure
RestartSec=5
NoNewPrivileges=true
PrivateTmp=true
ProtectSystem=full
ProtectHome=true

[Install]
WantedBy=multi-user.target
EOF2

sudo systemctl daemon-reload
sudo systemctl enable --now twamp-server.service
sudo systemctl status twamp-server.service --no-pager
```

To follow logs in real time:

```bash
sudo journalctl -u twamp-server.service -f
```

***

## Test From the Same VM

A basic local validation can be done by running the client against `127.0.0.1` on TCP 8862.

### Client flag reference

| Flag | Meaning |
|------|---------|
| `-s <addr>` | Server IP address |
| `-c <port>` | TWAMP-Control TCP port |
| `-n <N>` | Number of test sessions |
| `-m <N>` | Number of packets per session |
| `-i <ms>` | Inter-packet interval in milliseconds |
| `-d <DSCP>` | DSCP value (e.g. `46` = Expedited Forwarding) |

### Basic test

```bash
cd /opt/twamp-protocol
./client -s 127.0.0.1 -c 8862 -n 1 -m 10 -i 100
```

### DSCP / QoS test

```bash
./client -s 127.0.0.1 -c 8862 -n 1 -m 20 -d 46 -i 50
```

***

## Test From a Remote Client

From another Linux host where the same client binary is available, point the client at the VM address and TCP port 8862.

```bash
./client -s <VM_IP> -c 8862 -n 1 -m 10 -i 100
```

The server accepts the TCP control session on 8862 and assigns a UDP reflector port from the configured 20000–30000 range, returning it in `Accept-Session.Port`.

***

## Diagnostics

Use the commands below to confirm the process is running, ports are open, and packets are flowing.

```bash
# Service status and recent logs
sudo systemctl status twamp-server.service --no-pager
sudo journalctl -u twamp-server.service -n 100 --no-pager

# Check listening ports
ss -lntup | grep -E '8862|2000[0-9]|30000'

# Firewall rules
sudo ufw status verbose

# Live packet capture (control + test traffic)
sudo tcpdump -i any -n 'tcp port 8862 or (udp portrange 20000-30000)' -v

# Quick TCP reachability check
nc -zv <VM_IP> 8862
```
