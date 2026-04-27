# Deploying TWAMP on Ubuntu

This guide explains how to deploy the fork at `https://github.com/Corentinrhr/twamp-protocol` on an Ubuntu VM as a TWAMP reflector/server, using **TCP port 8862** for the TWAMP-Control connection and **UDP range 20000-30000** for TWAMP-Test sessions.

## Target setup

The server will listen on **TCP 8862** for the control session, and it will allocate TWAMP test sockets from the **UDP range 20000-30000**.
This matches the modified server logic where the server selects the effective UDP reflector port from its configured range and returns that port in `Accept-Session.Port`.

## Install dependencies

On Ubuntu, install the build toolchain, Git, the firewall tool, and a few useful diagnostics utilities.

```bash
sudo apt update && sudo apt upgrade -y
sudo apt install -y \
  git \
  build-essential \
  gcc \
  make \
  ufw \
  net-tools \
  tcpdump \
  netcat-openbsd \
  chrony
```

## Enable time synchronization

TWAMP measurements depend on timestamps, so the VM clock should be synchronized before testing.

```bash
sudo systemctl enable --now chrony
chronyc tracking
chronyc sources -v
```

## Clone the fork and compile

Clone the fork into `/opt`, then build it with `make`.

```bash
cd /opt
sudo git clone https://github.com/Corentinrhr/twamp-protocol.git
sudo chown -R $USER:$USER /opt/twamp-protocol
cd /opt/twamp-protocol

make
ls -lh
```

If the Makefile does not build correctly, compile manually with GCC:

```bash
gcc -Wall -Wextra -O2 -o server server.c twamp.c -lm
gcc -Wall -Wextra -O2 -o client client.c twamp.c -lm
```

## Create a dedicated service account

The code is designed not to run as root, so create a dedicated system user and assign ownership of the application directory to it.

```bash
sudo useradd --system \
  --no-create-home \
  --shell /usr/sbin/nologin \
  twamp

sudo chown -R twamp:twamp /opt/twamp-protocol
```

## Configure the firewall

Open the SSH port, the TWAMP control port **8862/TCP**, and the full TWAMP test port range **20000-30000/UDP**.

```bash
sudo ufw allow 22/tcp
sudo ufw allow 8862/tcp
sudo ufw allow 20000:30000/udp
sudo ufw enable
sudo ufw status verbose
```

## Manual validation before systemd

Before creating the service, launch the server manually and confirm that it is listening on TCP 8862.

```bash
cd /opt/twamp-protocol
./server -p 20000 -q 30000 -c 8862
```

In another terminal:

```bash
ss -lntup | grep -E '8862|server'
netstat -tulnp | grep -E '8862|server'
```

## Create the systemd service

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

## Test from the same VM

A basic local validation can be done by running the client against `127.0.0.1` on TCP 8862.

```bash
cd /opt/twamp-protocol
./client -s 127.0.0.1 -c 8862 -n 1 -m 10 -i 100
```

A DSCP test can be run as follows:

```bash
./client -s 127.0.0.1 -c 8862 -n 1 -m 20 -d 46 -i 50
```

## Test from a remote client

From another Linux host where the same client binary is available, point the client at the VM address and TCP port 8862.

```bash
./client -s <VM_IP> -c 8862 -n 1 -m 10 -i 100
```

The server will accept the TCP control session on 8862 and will then assign a UDP reflector port somewhere in the configured 20000-30000 range.

## Diagnostics

Use the commands below to confirm that the process is running, ports are open, and packets are flowing.

```bash
sudo systemctl status twamp-server.service --no-pager
sudo journalctl -u twamp-server.service -n 100 --no-pager
ss -lntup | grep -E '8862|2000[0-9]|30000'
sudo ufw status verbose
sudo tcpdump -i any -n 'tcp port 8862 or (udp portrange 20000-30000)' -v
nc -zv <VM_IP> 8862
```
