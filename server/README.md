# O2 Monitor – Thingy:91 X → Akamai Cloud (Linode)

Mosquitto (MQTT broker, TLS) + Node-RED (dashboard + low-O2 email alarm) in Docker.

```
Thingy:91 X ──LTE──► mqtts://mqtt.yourdomain.com:8883 ──► Mosquitto ──► Node-RED
                       topic: sensors/thingy91x/o2                       ├─ gauge + 24 h chart
                       payload: {"o2":20.8}                              ├─ email when O2 < 19.5 %
                                                                         ├─ email when it recovers
                                                                         └─ email if no data for 10 min
```

Replace `mqtt.yourdomain.com`, `you@example.com` and the passwords with your own values everywhere below.

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
   - TCP 8883 (MQTT over TLS for the board)
   - Default inbound policy: **Drop**
3. At your DNS provider, add an **A record** `mqtt.yourdomain.com` → the Linode's public IPv4 address. (No domain? The Linode's reverse DNS name, e.g. `172-105-x-x.ip.linodeusercontent.com`, works too.)

## 2. Install Docker and copy the project

```bash
ssh root@mqtt.yourdomain.com
curl -fsSL https://get.docker.com | sh
mkdir -p /opt/o2-monitor
```

From your PC, copy this folder to the server:

```bash
scp -r o2-monitor/* root@mqtt.yourdomain.com:/opt/o2-monitor/
```

## 3. Get a TLS certificate

```bash
apt install -y certbot
chmod +x /opt/o2-monitor/scripts/copy-certs.sh
certbot certonly --standalone --key-type rsa \
  -d mqtt.yourdomain.com \
  --deploy-hook /opt/o2-monitor/scripts/copy-certs.sh
```

`--key-type rsa` keeps the chain on **ISRG Root X1**, the root certificate the Thingy:91 X will need. Renewals run automatically and the hook copies the new certificate into Mosquitto.

## 4. Create MQTT users

```bash
cd /opt/o2-monitor
docker compose run --rm mosquitto mosquitto_passwd -c /mosquitto/config/passwd thingy91x
docker compose run --rm mosquitto mosquitto_passwd    /mosquitto/config/passwd nodered
docker compose run --rm mosquitto chown 1883:1883 /mosquitto/config/passwd
chmod 0700 mosquitto/config/passwd 2>/dev/null; chown -R 1000:1000 nodered/data
```

`thingy91x` may only publish to `sensors/thingy91x/#` and read its command topic; `nodered` may read all sensor data and send commands to `sensors/+/cmd` (see `mosquitto/config/aclfile`).

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

1. Double-click **Thingy:91 X O2** → edit the *Local Mosquitto* broker → **Security** tab → user `nodered` + its password.
2. Double-click **Send alarm email** → set the recipient in *To*, and your SMTP login. For Gmail, use an **App Password** (Google account → Security → 2-Step Verification → App passwords), not your normal password.
3. To change the alarm level, open **O2 alarm logic** and edit `ALARM_LEVEL` / `RECOVER_LEVEL` at the top. The O2 value on the dashboard turns red automatically while the alarm is active.
4. Click **Deploy**.

The dashboard is at **http://localhost:1880/ui** (O2 value in blue – red during an alarm – plus chart, status and a map). The full-screen map is at **http://localhost:1880/worldmap**. The marker turns red during an alarm, and alarm emails include the location with a Google Maps link.

## 7. Test without the board

From any PC with `mosquitto-clients` installed:

```bash
# Normal reading
mosquitto_pub -h mqtt.yourdomain.com -p 8883 --capath /etc/ssl/certs \
  -u thingy91x -P 'YOUR_PASSWORD' -t sensors/thingy91x/o2 -m '{"o2":20.8}'

# Reading with GPS location -> marker appears on the map
mosquitto_pub ... -m '{"o2":20.8,"lat":-26.1076,"lon":28.0567,"acc":8}'

# Low reading -> ALARM email (includes last known location)
mosquitto_pub ... -m '{"o2":18.9}'

# Back to normal -> "Recovered" email
mosquitto_pub ... -m '{"o2":20.9}'
```

## 8. Thingy:91 X firmware settings

| Setting | Value |
|---|---|
| Broker hostname | `mqtt.yourdomain.com` |
| Port | `8883` (TLS) |
| Username / password | `thingy91x` / the password from step 4 |
| Publish topic | `sensors/thingy91x/o2` |
| Payload | `{"o2":20.80,"lat":-26.10760,"lon":28.05670,"acc":8}` – `o2` in %, `lat`/`lon` in decimal degrees, `acc` = GNSS accuracy in metres. Location fields are optional; send them whenever you have a fix. |
| CA certificate | ISRG Root X1 – provision into the modem's TLS security tag |
| Client ID | any unique string, e.g. the board's IMEI |

## Air Quality dashboard (iAQ-Core demo)

The **Air Quality** tab (first tab at `/ui`) shows CO2-equivalent (ppm) and TVOC-equivalent (ppb) as large numbers. Each turns **red** when it is causing the alarm. Below them are a status line, two 24-hour charts and the map. Data arrives on `sensors/+/iaq`.

Alarm levels are at the top of the **Air quality alarm logic** node (CO2-eq 1400 / clear 1200 ppm, TVOC-eq 450 / clear 350 ppb). Keep them the same as the firmware's `CONFIG_IAQ_*` values. The email node is **disabled** because Linode blocks SMTP; enable it once notifications are sorted out.

Test without the board:

```bash
# Warm-up
docker compose exec mosquitto mosquitto_pub -u thingy91x -P 'YOUR_PASSWORD' -t sensors/thingy91x/iaq -m '{"status":"warmup","alarm":false}'
# Normal reading with location
docker compose exec mosquitto mosquitto_pub -u thingy91x -P 'YOUR_PASSWORD' -t sensors/thingy91x/iaq -m '{"co2":612,"tvoc":143,"status":"ok","p":83.1,"t":23.5,"lat":-26.1076,"lon":28.0567,"acc":8}'
# Alarm (CO2-eq high)
docker compose exec mosquitto mosquitto_pub -u thingy91x -P 'YOUR_PASSWORD' -t sensors/thingy91x/iaq -m '{"co2":1520,"tvoc":210,"status":"ok"}'
# Back to normal
docker compose exec mosquitto mosquitto_pub -u thingy91x -P 'YOUR_PASSWORD' -t sensors/thingy91x/iaq -m '{"co2":800,"tvoc":160,"status":"ok"}'
```

## Calibration panel

The dashboard has a **Calibration** section. It shows:
- calibration status, with **CALIBRATION DUE** after 30 days or if never calibrated
- the stored span and zero values with their dates
- **sensor health** (output compared to a new sensor, with a warning below 70 %)
- pressure compensation and the live raw sensor output

The buttons send commands to the board on `sensors/<device>/cmd`, and each asks for confirmation first:

| Button | What the board does |
|---|---|
| Fresh air (20.9 %) | Measures 20 s and stores it as 20.9 % O2 |
| Span calibrate | Same, with a certified span gas at the % you enter |
| Zero (N2) | Measures 30 s in pure nitrogen and stores it as 0 % O2 |
| Enter values manually | Stores zero/span values from a calibration certificate |
| Reset to datasheet | Deletes all stored calibration |
| Refresh | Asks the board to resend its calibration info |

The board rejects a calibration that is unstable or outside the datasheet limits, and the panel shows the reason. The board only receives commands while it's online; if it's offline, the panel keeps showing "waiting".

Test without the board (simulates the board's answer):

```bash
docker compose exec mosquitto mosquitto_pub -u thingy91x -P 'YOUR_PASSWORD' -r -t sensors/thingy91x/status \
  -m '{"type":"cal_info","has_span":true,"has_zero":false,"zero_uv":0,"span_uv":10500,"span_pct":20.9,"span_time":1791360000,"zero_time":0,"p_cal":83.1,"t_cal":22,"health":105,"age_d":3,"due":false,"interval_d":30,"health_warn":70,"p_comp":true,"t_coeff_ppm":0}'
```

## How the alarm behaves

- **Alarm** email as soon as O2 drops below `ALARM_LEVEL` (19.5 %).
- **Reminder** every 15 min while it stays low.
- **Recovered** email only once it rises above `RECOVER_LEVEL` (20.0 %). The gap stops a reading hovering around 19.5 % from sending dozens of emails.
- **Offline** email if no data arrives for 10 minutes. A silent sensor should never look like "everything is fine".

## Safety note

This is a monitoring/notification system, not a certified gas detector. If people's safety depends on O2 levels, use it alongside a certified detector with its own local alarm. Calibrate the O2 sensor in fresh air (20.9 %) regularly.
