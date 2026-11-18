#!/bin/sh
# Certbot deploy hook: copies the Let's Encrypt certificate to Mosquitto
# and restarts the broker. Runs on first issue and on every renewal.
DIR=/opt/o2-monitor/mosquitto/certs
cp "$RENEWED_LINEAGE/fullchain.pem" "$RENEWED_LINEAGE/privkey.pem" "$DIR/"
chown 1883:1883 "$DIR"/*.pem      # 1883 = mosquitto user inside the container
chmod 600 "$DIR/privkey.pem"
docker restart o2-mosquitto || true
