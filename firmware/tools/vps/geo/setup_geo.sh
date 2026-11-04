#!/usr/bin/env bash
# City search on the update host. Runs ON the server, as root, from the directory holding the three
# Python files; safe to run again (it updates the code and keeps the database).
#
# The first application on a box that was built to serve static files only (setup.sh). It holds no
# secret either: the data is public (GeoNames, CC BY 4.0), the process runs as its own unprivileged
# user, listens on localhost only, and nginx in front terminates TLS and limits the request rate.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP=/opt/smartmeteo-geo
DATA=/var/lib/smartmeteo-geo
USER_NAME=smgeo
SITE=/etc/nginx/sites-available/ota.conf

echo "== user and directories =="
id -u "$USER_NAME" >/dev/null 2>&1 || useradd --system --home "$APP" --shell /usr/sbin/nologin "$USER_NAME"
install -d -o root -g root -m 755 "$APP"
install -d -o "$USER_NAME" -g "$USER_NAME" -m 755 "$DATA"
install -o root -g root -m 644 "$HERE/common.py" "$HERE/build_db.py" "$HERE/geo_server.py" "$APP/"

echo "== systemd =="
cat >/etc/systemd/system/smartmeteo-geo.service <<UNIT
[Unit]
Description=Smart Meteo city search
After=network.target

[Service]
User=$USER_NAME
Environment=GEO_DB=$DATA/cities.sqlite GEO_PORT=8081
ExecStart=/usr/bin/python3 $APP/geo_server.py
Restart=on-failure
RestartSec=5
NoNewPrivileges=yes
ProtectSystem=strict
ProtectHome=yes
PrivateTmp=yes
PrivateDevices=yes
ProtectKernelTunables=yes
ProtectControlGroups=yes
RestrictAddressFamilies=AF_INET AF_INET6 AF_UNIX
MemoryMax=256M
# Tightened 2026-09-18 with the rest of the security review. None of these cost anything here: this
# is one stdlib Python process that opens a read-only SQLite file and listens on loopback.
CapabilityBoundingSet=
AmbientCapabilities=
RestrictSUIDSGID=yes
RestrictNamespaces=yes
RestrictRealtime=yes
LockPersonality=yes
ProtectKernelModules=yes
ProtectKernelLogs=yes
ProtectClock=yes
ProtectProc=invisible
SystemCallFilter=@system-service
SystemCallErrorNumber=EPERM
UMask=0077
# A log line per request goes to the journal, and the journal had no ceiling of its own. One
# malformed request used to write a whole traceback (fixed in geo_server.py), but the general
# lesson stands: an unauthenticated endpoint must not be able to fill a disk one line at a time.
LogRateLimitIntervalSec=30s
LogRateLimitBurst=500

[Install]
WantedBy=multi-user.target
UNIT

cat >/etc/systemd/system/smartmeteo-geo-build.service <<UNIT
[Unit]
Description=Smart Meteo city search: rebuild the database from GeoNames
After=network-online.target
Wants=network-online.target

[Service]
Type=oneshot
User=$USER_NAME
ExecStart=/usr/bin/python3 $APP/build_db.py $DATA/cities.sqlite
NoNewPrivileges=yes
ProtectSystem=strict
ReadWritePaths=$DATA
ProtectHome=yes
PrivateTmp=yes
PrivateDevices=yes
MemoryMax=512M
UNIT

cat >/etc/systemd/system/smartmeteo-geo-build.timer <<UNIT
[Unit]
Description=Monthly rebuild of the Smart Meteo city database

[Timer]
OnCalendar=monthly
RandomizedDelaySec=6h
Persistent=true

[Install]
WantedBy=timers.target
UNIT
systemctl daemon-reload

if [ ! -s "$DATA/cities.sqlite" ]; then
    echo "== first database build =="
    systemctl start smartmeteo-geo-build.service
fi
systemctl enable --now smartmeteo-geo-build.timer
systemctl enable smartmeteo-geo.service
systemctl restart smartmeteo-geo.service

echo "== nginx =="
cat >/etc/nginx/conf.d/smartmeteo-geo-zone.conf <<'CONF'
# Five searches a second per address, with a burst for typing: the device asks once per keystroke pause.
limit_req_zone $binary_remote_addr zone=smgeo:10m rate=5r/s;
# The search text is what a person typed about where they live. The service itself logs path and
# status only; nginx's default format would undo that by writing the full URI with ?q=. $uri has no
# query string.
log_format smgeo_noquery '$remote_addr - [$time_local] "$request_method $uri" $status $body_bytes_sent';
CONF
cat >/etc/nginx/snippets/smartmeteo-geo.conf <<'CONF'
location = /v1/geo/search {
    limit_req zone=smgeo burst=20 nodelay;
    limit_req_status 429;
    proxy_pass http://127.0.0.1:8081;
    proxy_set_header Host $host;
    proxy_connect_timeout 3s;
    proxy_read_timeout 10s;
    access_log /var/log/nginx/ota.access.log smgeo_noquery;
}
CONF
if ! grep -q "snippets/smartmeteo-geo.conf" "$SITE"; then
    cp -a "$SITE" "$SITE.bak-$(date +%Y%m%d%H%M%S)"
    # Before the catch-all 404, in every server block that has one (the TLS block after certbot).
    sed -i 's|^\(\s*\)location / { return 404; }|\1include /etc/nginx/snippets/smartmeteo-geo.conf;\n\1location / { return 404; }|' "$SITE"
fi
if ! nginx -t; then
    last=$(ls -1t "$SITE".bak-* 2>/dev/null | head -1)
    [ -n "$last" ] && cp -a "$last" "$SITE" && echo "nginx config restored from $last"
    rm -f /etc/nginx/snippets/smartmeteo-geo.conf /etc/nginx/conf.d/smartmeteo-geo-zone.conf
    nginx -t && systemctl reload nginx
    exit 1
fi
systemctl reload nginx

echo "== check =="
sleep 1
curl -fsS "http://127.0.0.1:8081/v1/geo/search?q=Darmstadt&n=1"; echo
