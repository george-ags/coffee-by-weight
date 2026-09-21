#!/bin/bash
#
# Assemble and deploy one app to /opt/<app> on this Pi.
#
#   ./deploy.sh lm-bbw          deploy LM-BBW and restart its service
#   ./deploy.sh grind-bw        deploy GRIND-BW and restart its service
#   ./deploy.sh lm-bbw --install  also (re)install the systemd unit + env file
#
# The deployed layout is:  /opt/<app>/{<entry>.py, app/, common/, web/, ...}
# i.e. the shared common/ package is copied INTO each app's deploy root, so
# `from common.scales import ...` resolves from the service WorkingDirectory.

set -e
APP="$1"
if [ -z "$APP" ] || [ ! -d "$APP" ]; then
    echo "usage: $0 <lm-bbw|grind-bw> [--install]"; exit 1
fi
DEST="/opt/$APP"

echo "Deploying $APP -> $DEST"
sudo mkdir -p "$DEST"
sudo cp -r "$APP"/. "$DEST"/
sudo cp -r common "$DEST"/
sudo chmod +x "$DEST"/*.py 2>/dev/null || true

if [ "$2" = "--install" ]; then
    echo "Installing systemd unit + env"

    # The Pi has no RTC. Without fake-hwclock it boots with whatever timestamp
    # systemd restored from /var/lib/systemd/timesync/clock -- i.e. the time of
    # the LAST shutdown -- and then jumps forward by hours when NTP finally
    # answers. Journals from such a boot are unreadable: entries are stamped
    # with a clock that is hours behind, and the boot looks like it happened
    # last night. fake-hwclock keeps the restored time close to reality.
    # Not fatal: a Pi that can't reach apt right now should still get the app.
    if ! dpkg -s fake-hwclock >/dev/null 2>&1; then
        echo "Installing fake-hwclock (no RTC on this Pi)"
        sudo apt-get install -y fake-hwclock || echo "WARNING: fake-hwclock not installed - install it later, see README"
    fi
    sudo systemctl enable fake-hwclock 2>/dev/null || true
    sudo fake-hwclock save 2>/dev/null || true

    sudo cp "$APP/service/$APP.service" /etc/systemd/system/
    # Don't clobber a live env file; only install if absent.
    if [ ! -f "/etc/default/$APP.env" ]; then
        sudo cp "$APP/service/$APP.env" /etc/default/
    fi
    sudo systemctl daemon-reload
    sudo systemctl enable "$APP"
fi

sudo systemctl restart "$APP" 2>/dev/null && echo "Restarted $APP" || echo "Service $APP not installed yet (use --install)"
