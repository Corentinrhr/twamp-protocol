# Deploying TWAMP on Ubuntu

This guide explains how to deploy the fork at `https://github.com/Corentinrhr/twamp-protocol` on an Ubuntu VM as a TWAMP reflector/server, running **two independent instances**: one for IPv4 and one for IPv6.

| Instance | TCP Control Port | UDP Test Range |
|----------|-----------------|----------------|
| IPv4     | **8862**        | **20000-24999** |
| IPv6     | **8863**        | **25000-30000** |

Both UDP ranges are inclusive on both bounds. The server selects the effective reflector UDP port from its configured range and returns it in `Accept-Session.Port`.

---

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

---

## Enable Time Synchronization

TWAMP measurements rely on accurate timestamps. Ubuntu ships with `systemd-timesyncd` enabled by default; disable it before installing `chrony` to avoid conflicts.

```bash
sudo systemctl disable --now systemd-timesyncd
sudo systemctl enable --now chrony
chronyc tracking
chronyc sources -v
```

---

## Create a Dedicated Service Account

The server code explicitly refuses to run as root. Create a system user with no home directory and no login shell.

```bash
sudo useradd --system \
  --no-create-home \
  --shell /usr/sbin/nologin \
  twamp
```

---

## Clone the Fork and Compile

Clone the repository into `/opt`, assign ownership to the `twamp` user, then build.

```bash
cd /opt
sudo git clone https://github.com/Corentinrhr/twamp-protocol.git
sudo chown -R twamp:twamp /opt/twamp-protocol
cd /opt/twamp-protocol
sudo -u twamp make
ls -lh
```

If the Makefile does not build correctly, compile manually with GCC (using the exact flags from the Makefile):

```bash
sudo -u twamp gcc -Wall -Wextra -Werror -g -static \
  -o server server.c timestamp.c
sudo -u twamp gcc -Wall -Wextra -Werror -g -static \
  -o client client.c timestamp.c
```
---

## Configure the Firewall

Open SSH, both TWAMP control ports, and both UDP test ranges.

```bash
sudo ufw allow 22/tcp
sudo ufw allow 8862/tcp        # IPv4 control
sudo ufw allow 8863/tcp        # IPv6 control
sudo ufw allow 20000:24999/udp # IPv4 test range
sudo ufw allow 25000:30000/udp # IPv6 test range
sudo ufw enable
sudo ufw status verbose
```

---

## Manual Validation Before systemd

Before creating the services, launch each instance manually to confirm it binds correctly.

**IPv4 instance:**
```bash
cd /opt/twamp-protocol
sudo -u twamp ./server -p 20000 -q 24999 -c 8862
```

**IPv6 instance :**
```bash
cd /opt/twamp-protocol
sudo -u twamp ./server -p 25000 -q 30000 -c 8863 -6
```

Verify both ports are listening:
```bash
ss -lntup | grep -E '8862|8863'
```

> Always use `sudo -u twamp` running as your own user is inconsistent with the production service and may fail if the directory is owned by `twamp`.

---

## Create the systemd Services

### IPv4 service

```bash
sudo tee /etc/systemd/system/twamp-server4.service > /dev/null << 'EOF'
[Unit]
Description=TWAMP Reflector Server (IPv4)
Documentation=https://github.com/Corentinrhr/twamp-protocol
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=twamp
Group=twamp
WorkingDirectory=/opt/twamp-protocol
ExecStart=/opt/twamp-protocol/server -p 20000 -q 24999 -c 8862
Restart=on-failure
RestartSec=5
NoNewPrivileges=true
PrivateTmp=true
ProtectSystem=full
ProtectHome=true

[Install]
WantedBy=multi-user.target
EOF
```

### IPv6 service

```bash
sudo tee /etc/systemd/system/twamp-server6.service > /dev/null << 'EOF'
[Unit]
Description=TWAMP Reflector Server (IPv6)
Documentation=https://github.com/Corentinrhr/twamp-protocol
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=twamp
Group=twamp
WorkingDirectory=/opt/twamp-protocol
ExecStart=/opt/twamp-protocol/server -p 25000 -q 30000 -c 8863 -6
Restart=on-failure
RestartSec=5
NoNewPrivileges=true
PrivateTmp=true
ProtectSystem=full
ProtectHome=true

[Install]
WantedBy=multi-user.target
EOF
```

### Enable and start both services

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now twamp-server4.service
sudo systemctl enable --now twamp-server6.service
sudo systemctl status twamp-server4.service --no-pager
sudo systemctl status twamp-server6.service --no-pager
```

Follow logs in real time:
```bash
sudo journalctl -u twamp-server4.service -f
sudo journalctl -u twamp-server6.service -f
```

> **Important:** If you previously had a `twamp-server.service` (without suffix) running on port 8862, stop and disable it first to avoid a port conflict:
> ```bash
> sudo systemctl stop twamp-server.service
> sudo systemctl disable twamp-server.service
> ```

---

## Test From the Same VM

### Client flag reference

| Flag | Meaning |
|------|---------|
| `-s <addr>` | Server IP address or IPv6 address |
| `-c <port>` | TWAMP-Control TCP port |
| `-n <N>` | Number of test sessions |
| `-m <N>` | Number of packets per session |
| `-i <ms>` | Inter-packet interval in milliseconds |
| `-d <DSCP>` | DSCP value (e.g. `46` = Expedited Forwarding) |
| `-6` | Use IPv6 |

### IPv4 basic test

```bash
cd /opt/twamp-protocol
./client -s 127.0.0.1 -c 8862 -n 1 -m 10 -i 100
```

### IPv6 basic test

```bash
./client -s ::1 -c 8863 -n 1 -m 10 -i 100 -6
```

### DSCP / QoS test

```bash
./client -s 127.0.0.1 -c 8862 -n 1 -m 20 -d 46 -i 50
```

---

## Test From a Remote Client

Replace `<VM_IPv4>` and `<VM_IPv6>` with the actual server addresses.

```bash
# IPv4
./client -s <VM_IPv4> -c 8862 -n 1 -m 10 -i 100

# IPv6
./client -s <VM_IPv6> -c 8863 -n 1 -m 10 -i 100 -6
```

The server accepts the TCP control session on the respective port and assigns a UDP reflector port from its configured range, returning it in `Accept-Session.Port`.

---

## Diagnostics

```bash
# Service status
sudo systemctl status twamp-server4.service --no-pager
sudo systemctl status twamp-server6.service --no-pager

# Recent logs
sudo journalctl -u twamp-server4.service -n 100 --no-pager
sudo journalctl -u twamp-server6.service -n 100 --no-pager

# Check all listening ports
ss -lntup | grep -E '8862|8863|2[0-9]{4}'

# Firewall rules
sudo ufw status verbose

# Live packet capture - IPv4 control + test traffic
sudo tcpdump -i any -n 'tcp port 8862 or (udp portrange 20000-24999)' -v

# Live packet capture - IPv6 control + test traffic
sudo tcpdump -i any -n 'tcp port 8863 or (udp portrange 25000-30000)' -v

# Quick TCP reachability check
nc -zv <VM_IPv4> 8862
nc -6 -zv <VM_IPv6> 8863
```
