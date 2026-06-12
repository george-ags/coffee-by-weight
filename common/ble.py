#!/usr/bin/env python
# -*- coding: utf-8 -*-
#
# Shared Bluetooth-adapter coordination.
#
# Only one BLE scan may run on the adapter at a time. Several code paths scan:
#   - background discovery (control.py)
#   - the manual web "scan for scales" (control.py)
#   - each scale driver's connect routine (scale_acaia.py / scale_bookoo.py),
#     which must scan to acquire the peripheral before connecting.
#
# If two of these call adapter.scan_for() concurrently, BlueZ returns
# "Operation already in progress" and the adapter can wedge (scanning and
# connecting both stop working). Every scan_for() call in the codebase acquires
# this single lock so they are strictly serialized.
#
# It lives in its own module (no other imports) so every component can import it
# without creating circular dependencies.

import threading

# Re-entrant so a thread that already holds it can call a helper that re-acquires
# it without deadlocking.
adapter_scan_lock = threading.RLock()