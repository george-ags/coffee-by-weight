# Coffee by Weight

A small family of Raspberry Pi controllers that add **weight-based control** to home espresso gear using a Bluetooth coffee scale. A single shared scale/BLE/display stack drives multiple machine-specific apps.

> **Note:** This is a personal hobby project, provided as-is with **no support**. Contributions are welcome, but it is built around my own needs — for customization, forking and making it your own is the best path.

---

## What's in here

| App | What it does | Status |
|-----|--------------|--------|
| [**lm-bbw**](./lm-bbw/) | **Brew-by-weight** for the La Marzocco Linea Micra — proxies the paddle switch and cuts the shot when the cup hits the target weight (minus a learned drip-out margin). | Working build, daily-driven |
| [**grind-bw**](./grind-bw/grind-bw.py) | **Grind-by-weight** controller for a grinder — stop dosing at a target weight. | Skeleton / in development |

Both apps read weight from an **Acaia** (Lunar / Pyxis / Umbra) or **BooKoo** (Ultra / Mini) scale over Bluetooth LE, through the same shared driver layer.

👉 **For the full story — features, hardware, wiring, enclosure STLs, installation, and configuration — see the [LM-BBW README](./lm-bbw/README.md).**

---

## Repository layout

```
coffee-by-weight/
├── common/            shared package, copied into each app at deploy time
│   ├── scales.py        vendor-neutral Scale wrapper + combined scanner/factory
│   ├── scale_acaia.py   Acaia BLE driver (scan/connect/heartbeat, protocol)
│   ├── scale_bookoo.py  BooKoo BLE driver (Ultra/Mini protocol)
│   ├── ble.py           single lock serializing all BLE adapter scans
│   ├── lcd/             WaveShare 2.0"/2.4" SPI LCD drivers
│   └── font/            bundled fonts
│
├── lm-bbw/            brew-by-weight app (La Marzocco Micra)
│   ├── lm-bbw.py        entry point: main loop, shot cutoff, overshoot learning
│   ├── app/             control.py, display.py, webserver.py, images
│   ├── service/         systemd unit + default env
│   └── README.md        full project documentation
│
├── grind-bw/          grind-by-weight app (skeleton)
│   ├── grind-bw.py      entry point: connects + streams weight; grinder TODOs
│   ├── app/             grinder-specific modules go here
│   └── service/         systemd unit + default env
│
├── doc/               architecture, wiring diagrams, photos, enclosure STLs
└── deploy.sh          assemble + deploy one app to /opt/<app> on the Pi
```

The shared `common/` package is **copied into each app's deploy root** so that imports like `from common.scales import Scale` resolve from the service's working directory. Each app is otherwise self-contained (own entry point, systemd unit, and env file).

---

## Deploying

`deploy.sh` assembles one app — its own files plus a copy of `common/` — into `/opt/<app>/` on the Pi and restarts its service:

```bash
./deploy.sh lm-bbw              # deploy LM-BBW and restart its service
./deploy.sh grind-bw            # deploy GRIND-BW and restart its service
./deploy.sh lm-bbw --install    # also (re)install the systemd unit + env file
```

The resulting layout is `/opt/<app>/{<entry>.py, app/, common/, ...}`. See the [LM-BBW README](./lm-bbw/README.md#software-installation) for first-time Pi setup (SPI, Bluetooth, dependencies).

---

## Adding a scale vendor

Scales sit behind a common interface (`connect`, `disconnect`, `tare`, and `mac` / `connected` / `weight` / `battery` / `units`). To add a vendor, write a driver module exposing that interface and register its advertised-name prefixes and constructor in [`common/scales.py`](./common/scales.py). All BLE scans go through the single lock in [`common/ble.py`](./common/ble.py), so the adapter is never scanned by two paths at once.

---

## Credits & license

- LM-BBW was inspired by and originally based on [Apollo](https://github.com/mlsorensen/apollo) by Marcus Sorensen.
- BooKoo scale support is based on BooKoo's published protocol specs (see [`doc/BT_Scales/BooKooCode/`](./doc/BT_Scales/)).
- The WaveShare LCD drivers (`common/lcd/`) are by the WaveShare team and retain their original MIT license.

Licensed under the **GNU General Public License v3** — see [LICENSE](./LICENSE). If you are a commercial organization and want to use this project, please get in touch.
