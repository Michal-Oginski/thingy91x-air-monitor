# Thingy:91 X Environment Monitor

Mosquitto (MQTT broker, TLS) + Node-RED (onboard environment dashboard and map) in Docker.

```
Thingy:91 X ──LTE──► MQTT/TLS broker ──► Mosquitto ──► Node-RED
                       topic: sensors/thingy91x/env           ├─ BME688 measurements
                       payload: temperature, pressure,        ├─ 24 h charts
                                 humidity, gas resistance      └─ location map
```

Replace `mqtt.yourdomain.com` and the passwords with your own values where relevant below.

The deployment steps describe an optional self-hosted broker. The firmware defaults target the existing Mikrus endpoint `srv72.mikr.us:40022`, which forwards to the Mosquitto TLS listener on port `8883`.

## 1. Create the server

1. In Akamai Cloud Manager, create a Linode: **Ubuntu 24.04 LTS**, **Shared CPU → Linode 2 GB ($12/month)** (recommended). Pick the region closest to you (Johannesburg if available, otherwise London or Frankfurt) and add your SSH key.
   - *Budget option:* **Nanode 1 GB ($5/month)** also works for one board, but add swap first so the Docker build doesn't run out of memory:
     ```bash
     fallocate -l 1G /swapfile && chmod 600 /swapfile && mkswap /swapfile && swapon /swapfile
     echo '/swapfile none swap sw 0 0' >> /etc/fstab   # keep swap after reboot
     ```
2. Create a **Cloud Firewall** and attach it to the Linode. Inbound rules:
   - TCP 22 (SSH) – ideally only from your own IP
   - TCP 80 (needed for Let's Encrypt certificate issue/renewal)
   - TCP 8883 (MQTT over TLS for a self-hosted broker; Mikrus exposes this listener as port 40022)
   - Default inbound policy: **Drop**
3. At your DNS provider, add an **A record** `mqtt.yourdomain.com` → the Linode's public IPv4 address. (No domain? The Linode's reverse DNS name, e.g. `172-105-x-x.ip.linodeusercontent.com`, works too.)

## 2. Install Docker and copy the project

```bash
ssh root@mqtt.yourdomain.com
curl -fsSL https://get.docker.com | sh
mkdir -p /opt/thingy91x-air-monitor
```

From your PC, copy this folder to the server:

```bash
scp -r thingy91x-air-monitor/* root@mqtt.yourdomain.com:/opt/thingy91x-air-monitor/
```

## 3. Get a TLS certificate

```bash
apt install -y certbot
chmod +x /opt/thingy91x-air-monitor/scripts/copy-certs.sh
certbot certonly --standalone --key-type rsa \
  -d mqtt.yourdomain.com \
  --deploy-hook /opt/thingy91x-air-monitor/scripts/copy-certs.sh
```

For a self-hosted broker, make sure the CA embedded in the firmware matches its certificate chain. The current firmware embeds the Mikrus CA; replace it with your own broker CA before using a different TLS endpoint. Renewals run automatically and the hook copies the renewed certificate into Mosquitto.

## 4. Create MQTT users

```bash
cd /opt/thingy91x-air-monitor
docker compose run --rm mosquitto mosquitto_passwd -c /mosquitto/config/passwd thingy91x
docker compose run --rm mosquitto mosquitto_passwd    /mosquitto/config/passwd nodered
docker compose run --rm mosquitto chown 1883:1883 /mosquitto/config/passwd
chmod 0700 mosquitto/config/passwd 2>/dev/null; chown -R 1000:1000 nodered/data
```

`thingy91x` may only publish to `sensors/thingy91x/env`. `nodered` may read environment messages from `sensors/+/env`; neither account needs calibration-command access (see `mosquitto/config/aclfile`).

## 5. Start the stack

```bash
docker compose up -d --build
docker compose logs -f mosquitto   # should show listeners on 1883 and 8883
```

## 6. Finish Node-RED setup

Node-RED is only reachable through an SSH tunnel (it is not exposed to the internet). From your PC:

```bash
ssh -L 1880:localhost:1880 root@mqtt.yourdomain.com
```

Open **http://localhost:1880** and:

1. Double-click **Thingy:91 X onboard environment** → edit the *Local Mosquitto* broker → **Security** tab → user `nodered` + its password.
2. Click **Deploy**.

The dashboard at **http://localhost:1880/ui** shows BME688 temperature, pressure, humidity and gas resistance, along with recent charts and the device location. The full-screen map is at **http://localhost:1880/worldmap**.

## 7. Test without the board

From any PC with `mosquitto-clients` installed:

```bash
mosquitto_pub -h srv72.mikr.us -p 40022 --cafile firmware/cert/mikrus_ca.pem \
  -u thingy91x -P 'YOUR_PASSWORD' -t sensors/thingy91x/env \
  -m '{"status":"ok","temperature":24.1,"pressure":101.2,"humidity":45.3,"gas_resistance":120000,"lat":52.198671,"lon":21.047436,"acc":12,"loc":"gnss","fix_age":30}'
```

## 8. Thingy:91 X firmware settings

| Setting | Value |
|---|---|
| Broker hostname | `srv72.mikr.us` |
| Port | `40022` (TLS; forwarded to Mosquitto port `8883`) |
| Username / password | `thingy91x` / the password from step 4 |
| Publish topic | `sensors/thingy91x/env` |
| Payload | `status`, BME688 `temperature` (C), `pressure` (kPa), `humidity` (%), `gas_resistance` (ohm), and optional location fields (`lat`, `lon`, `acc`, `loc`, `fix_age`). |
| CA certificate | The Mikrus CA at `firmware/cert/mikrus_ca.pem`, embedded and provisioned into the modem by the firmware. |
| Client ID | `thingy91x-<IMEI>` |

Set the MQTT password in the ignored local `firmware/secrets.conf`; do not commit it. The example configuration uses the `CONFIG_APP_MQTT_*` symbols.

## Safety note

This is an environmental monitoring system, not a certified safety instrument. Do not rely on its readings as the sole basis for health or safety decisions.
