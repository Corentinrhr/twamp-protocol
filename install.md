# Deploying TWAMP on Ubuntu

This guide explains how to deploy the fork at `https://github.com/Corentinrhr/twamp-protocol` on an Ubuntu VM as a TWAMP reflector/server, running a **single dual-stack instance** (IPv4 & IPv6) using the `-6` flag.

| Instance | TCP Control Port | UDP Test Range |
|----------|-----------------|----------------|
| IPv4 & IPv6 | **8862** | **20000-30000** |

The UDP range is inclusive on both bounds. The server selects the effective reflector UDP port from its configured range and returns it in `Accept-Session.Port`.

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

## Create a Dedicated Service Account

The server code explicitly refuses to run as root. Create a system user with no home directory and no login shell.

```bash
sudo useradd --system \
  --no-create-home \
  --shell /usr/sbin/nologin \
  twamp
```

***

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

***

## Configure the Firewall

Open SSH, the TWAMP control port, and the UDP test range.

```bash
sudo ufw allow 22/tcp
sudo ufw allow 8862/tcp        # control (IPv4 & IPv6)
sudo ufw allow 20000:30000/udp # test range (IPv4 & IPv6)
sudo ufw enable
sudo ufw status verbose
```

***

## Manual Validation Before systemd

Before creating the service, launch the instance manually to confirm it binds correctly.

```bash
cd /opt/twamp-protocol
sudo -u twamp ./server -p 20000 -q 30000 -c 8862 -6
```

Verify the port is listening:
```bash
ss -lntup | grep 8862
```

> Always use `sudo -u twamp` — running as your own user is inconsistent with the production service and may fail if the directory is owned by `twamp`.

***

## Create the systemd Service

```bash
sudo tee /etc/systemd/system/twamp-server.service > /dev/null << 'EOF'
[Unit]
Description=TWAMP Reflector Server (IPv4 & IPv6)
Documentation=https://github.com/Corentinrhr/twamp-protocol
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=twamp
Group=twamp
WorkingDirectory=/opt/twamp-protocol
ExecStart=/opt/twamp-protocol/server -p 20000 -q 30000 -c 8862 -6
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

### Enable and start the service

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now twamp-server.service
sudo systemctl status twamp-server.service --no-pager
```

Follow logs in real time:
```bash
sudo journalctl -u twamp-server.service -f
```

> **Important:** If you previously had `twamp-server4.service` or `twamp-server6.service` running, stop and disable them first to avoid port conflicts:
> ```bash
> sudo systemctl stop twamp-server4.service twamp-server6.service
> sudo systemctl disable twamp-server4.service twamp-server6.service
> ```

***

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
./client -s ::1 -c 8862 -n 1 -m 10 -i 100 -6
```

### DSCP / QoS test

```bash
./client -s 127.0.0.1 -c 8862 -n 1 -m 20 -d 46 -i 50
```

***

## Test From a Remote Client

Replace `<VM_IPv4>` and `<VM_IPv6>` with the actual server addresses.

```bash
# IPv4
./client -s <VM_IPv4> -c 8862 -n 1 -m 10 -i 100

# IPv6
./client -s <VM_IPv6> -c 8862 -n 1 -m 10 -i 100 -6
```

The server accepts the TCP control session on port `8862` for both address families and assigns a UDP reflector port from the configured range, returning it in `Accept-Session.Port`.

***

## Diagnostics

```bash
# Service status
sudo systemctl status twamp-server.service --no-pager

# Recent logs
sudo journalctl -u twamp-server.service -n 100 --no-pager

# Check all listening ports
ss -lntup | grep -E '8862|2[0-9]{4}|30000'

# Firewall rules
sudo ufw status verbose

# Live packet capture - control + test traffic (IPv4)
sudo tcpdump -i any -n 'tcp port 8862 or (udp portrange 20000-30000)' -v

# Live packet capture - IPv6 only
sudo tcpdump -i any -n ip6 -v

# Quick TCP reachability check
nc -zv <VM_IPv4> 8862
nc -6 -zv <VM_IPv6> 8862
```
