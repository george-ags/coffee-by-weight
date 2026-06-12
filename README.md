# coffee-by-weight

Weight-based coffee automation on Raspberry Pi. One repo, multiple small controllers ("apps") that share a common core: Bluetooth scale drivers (Acaia and BooKoo), a vendor-neutral scale interface, BLE adapter coordination, and SPI LCD display drivers. Each app runs as its own systemd service on its own Pi.

## Apps

### ☕ [LM-BBW — Brew-by-Weight](./lm-bbw/README.md)

Adds brew-by-weight to the La Marzocco Linea Micra: reads a Bluetooth scale, shows the live shot on a 2" display, and proxies the paddle switch to stop the shot at the target weight. Adaptive overshoot learning, memory banks, shot-history web gallery, web configuration, and a Bluetooth scale setup page. See the [full LM-BBW README](./lm-bbw/README.md) and the [architecture overview](./doc/lm-bbw/LM-BBW_Architecture.md).

▶️ **[Watch the demo](./doc/lm-bbw/lm-bbw.mp4)** — LM-BBW landing a shot on target.

### ⚙️ GRIND-BW — Grind-by-Weight *(in development)*

Grind-by-weight controller for a coffee grinder, built on the same shared core. Currently a working skeleton: it scans for a supported scale, connects, and streams live weight; dosing and motor control are in progress. See the [architecture & status](./doc/grind-bw/GRIND-BW_Architecture.md).

## Repository layout

```
coffee-by-weight/
├── common/            shared package: scale drivers (Acaia, BooKoo), vendor-neutral
│                      Scale interface, BLE scan lock, WaveShare LCD drivers, fonts
├── doc/
│   ├── lm-bbw/        LM-BBW docs: architecture, wiring photos, enclosure STLs, demo video
│   ├── grind-bw/      GRIND-BW docs: architecture & status
│   └── BT_Scales/     Bluetooth scale protocol specs (Acaia, BooKoo Ultra/Mini)
├── lm-bbw/            espresso controller: entry point, app modules, service files, web assets
├── grind-bw/          grinder controller (skeleton)
└── deploy.sh          assembles and deploys one app to /opt/<app> on a Pi
```

The `common/` package is shared by all apps. A fix there (for example in a scale driver) lands in every app with one commit; each Pi picks it up on its next deploy. In the repo, `common/` sits beside the apps; at deploy time it is copied inside the app's directory on the Pi (`/opt/<app>/common/`) so imports resolve from the service working directory.

## Supported scales

Acaia (Lunar, Pyxis, Umbra) and BooKoo (Ultra, Mini), behind one common interface. New vendors can be added with a small driver module registered in `common/scales.py`; the protocol references collected under [`doc/BT_Scales/`](./doc/BT_Scales/) document the Acaia and BooKoo wire protocols.

## Deploying an app

On the target Pi, from a checkout of this repo:

```bash
git clone https://github.com/george-ags/coffee-by-weight.git
cd coffee-by-weight
./deploy.sh lm-bbw --install     # first time: copies code, installs systemd unit + env
./deploy.sh lm-bbw               # afterwards: update + restart
```

Use `grind-bw` in place of `lm-bbw` on the grinder Pi. Per-app setup details (Pi configuration, SPI, dependencies, wiring) are in each app's own README.

> **Note:** This is a hobby project — provided as-is, with **no support**. Contributions are welcome, but it is built around my own needs; for customization, forking and making it your own is the best path.

## Credits & license

Inspired by and originally based on [Apollo](https://github.com/mlsorensen/apollo) by Marcus Sorensen. BooKoo scale support is based on BooKoo's published protocol specs. The WaveShare LCD drivers (`common/lcd/`) are by the WaveShare team and retain their original MIT license. See the [LM-BBW README](./lm-bbw/README.md#credits--license) for details. If you are a commercial organization and want to use this project please contact me.