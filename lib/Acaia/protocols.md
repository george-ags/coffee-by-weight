# ACAIA SCALE Transmission protocol

> This document describes the Bluetooth LE protocol for Acaia scales (Lunar, Pyxis, Umbra, and similar). Acaia does not publish an official public specification; the details below were reverse-engineered from working code and the community projects. Byte values are hexadecimal unless noted; payload index numbers are 0-based.

## 1. Bluetooth Protocol Basic Info And Check Sum

### Basic Info

Acaia uses one of two characteristic layouts depending on the scale generation. The driver discovers services after connecting and picks whichever it finds:

| Generation | Characteristic UUID | Notes |
| ----------- | ----------- | ----------- |
| Old style (original Lunar, Pearl) | `00002a80-0000-1000-8000-00805f9b34fb` | Single characteristic used for both writes (commands) and notifications (data). |
| New style (Pyxis, Lunar 2021, Umbra) | `49535343-8841-43f4-a8d4-ecbe34729bb3` | Single characteristic used for both writes and notifications. |

The enclosing **Service UUID is not hard-coded**: the driver scans every service, matches one of the characteristic UUIDs above, and then uses that characteristic's parent service. Commands are sent to the same characteristic that notifications are received on (write without response).

The scale is identified during scanning by its advertised name. Recognized name prefixes:

```
ACAIA, PYXIS, UMBRA, LUNAR, PROCH
```

### Packet Framing

Every message — in both directions — is wrapped in the same frame:

| BYTE1 | BYTE2 | BYTE3 | BYTE4 ... BYTE(N+3) | BYTE(N+4) | BYTE(N+5) |
| ----------- | ----------- | ----------- | ----------- | ----------- | ----------- |
| HEADER1 | HEADER2 | TYPE | PAYLOAD (N bytes) | CHECKSUM1 | CHECKSUM2 |
| `EF` | `DD` | message type | type-specific payload | even-index sum | odd-index sum |

- **HEADER1 / HEADER2** are always `EF DD` and mark the start of a frame.
- **TYPE** identifies the message (see sections 2 and 3).
- **PAYLOAD** is `N` bytes; `N` is implied by the message type / parser.
- **CHECKSUM1 / CHECKSUM2** are two running 8-bit sums over the payload (see below).

### Check Sum Method

Two one-byte checksums are computed over the **payload bytes only** (not the header or type). Payload bytes at even indices are summed into `CHECKSUM1`; bytes at odd indices into `CHECKSUM2`. Each is truncated to 8 bits.

```
cksum1 = 0
cksum2 = 0
for i in range(len(payload)):
    if i % 2 == 0:
        cksum1 += payload[i]
    else:
        cksum2 += payload[i]
CHECKSUM1 = cksum1 & 0xFF
CHECKSUM2 = cksum2 & 0xFF
```

> The receive parser in this project locates the frame by the `EF DD` header and reads the declared length; it does not reject on checksum mismatch.

## 2. Transmission Data (Host -> Scale: Commands)

All commands are built with the framing in section 1. The table lists the **TYPE** and the **PAYLOAD** (the bytes between TYPE and the two checksums). The two checksum bytes are appended automatically.

| TYPE | PAYLOAD (bytes) | DESCRIPTION |
| ----------- | ----------- | ----------- |
| `00` | `02 00` | Heartbeat / keep-alive. Sent every ~2 s; the connection drops if writes fail repeatedly. |
| `04` | `00` | Tare (zero the scale). |
| `0B` (11) | 15 bytes of app "ID" | Identify/authenticate the app to the scale (see ID payloads below). Resent periodically. |
| `0C` (12) | event-data block | Subscribe to scale events / request notifications (see notification request below). |

### ID payload (TYPE `0B`)

A fixed 15-byte identifier. Two variants depending on scale style:

| Style | 15-byte payload |
| ----------- | ----------- |
| New (Pyxis/Lunar 2021/Umbra) | `30 31 32 33 34 35 36 37 38 39 30 31 32 33 34` |
| Old | `2D 2D 2D 2D 2D 2D 2D 2D 2D 2D 2D 2D 2D 2D 2D` |

### Notification request (TYPE `0C`)

The notification subscription is an "event data" message: TYPE `0C`, whose payload is a length-prefixed inner block. The inner data sent by this driver is:

```
inner = [00, 01, 01, 02, 02, 05, 03, 04]
payload = [len(inner) + 1] followed by inner
```

i.e. the payload begins with a length byte (`09`) and then the 8 inner bytes above. This registers the scale to stream weight (and related) notifications.

### Connection handshake

After connecting and subscribing to notifications, the driver sends, in order:

1. ID message (TYPE `0B`)
2. Notification request (TYPE `0C`)
3. Notification request (TYPE `0C`) again
4. Heartbeat (TYPE `00`, payload `02 00`)

Thereafter a heartbeat is sent every ~2 s, and the ID + notification request are re-sent roughly every 20 s (every 10th heartbeat).

## 3. Receiving Data (Scale -> Host)

Incoming bytes are buffered and parsed frame-by-frame using the `EF DD` header. After the header and TYPE byte, **BYTE4 is the payload length**. The two message families this project decodes are:

### 3a. Event message (frame TYPE `0C` / 12)

When the frame TYPE is `0C` (12), the **next byte is an inner message type** and the remaining bytes are that inner message's payload:

```
frame: EF DD 0C <len> <inner_type> <inner_payload...> <cksum1> <cksum2>
```

| inner_type | Meaning | Payload handling |
| ----------- | ----------- | ----------- |
| `05` (5) | Weight update | Decode weight from payload (see weight format). This is the live weight stream. |
| `07` (7) | Timer update | Decode time from payload (see time format). |
| `0B` (11) | Combined | If `payload[2] == 5`: weight from `payload[3:]`. If `payload[2] == 7`: time from `payload[3:]`. |
| `08` (8) | Button / tare-start-stop event | See button events below. |

#### Weight format (6+ bytes)

| Offset | Field | Notes |
| ----------- | ----------- | ----------- |
| `[0:4]` | Raw weight | Unsigned 32-bit integer. Big-endian is tried first; if the result is out of range (abs > 4000), little-endian is used. |
| `[4]` | Unit / decimal scale | `1`=÷10, `2`=÷100, `3`=÷1000, `4`=÷10000. Default ÷10. |
| `[5]` | Flags | Bit `0x02` set => weight is negative. |

```
unit    = payload[4] & 0xFF
divisor = {1:10, 2:100, 3:1000, 4:10000}.get(unit, 10)
sign    = -1 if (payload[5] & 0x02) else +1
raw     = uint32(payload[0:4])          # big-endian first
weight  = sign * raw / divisor          # if abs > 4000, re-read raw as little-endian
```

#### Time format (3 bytes)

| Offset | Field |
| ----------- | ----------- |
| `[0]` | minutes |
| `[1]` | seconds |
| `[2]` | tenths of a second |

```
time_seconds = payload[0]*60 + payload[1] + payload[2]/10
```

#### Button events (inner_type `08`)

The first two payload bytes identify the event; weight/time follow:

| payload[0] | payload[1] | Event | Extra data |
| ----------- | ----------- | ----------- | ----------- |
| `00` | `05` | tare | weight from `payload[2:]` |
| `08` | `05` | start (timer) | weight from `payload[2:]` |
| `0A` | `07` | stop (timer) | time from `payload[2:]`, weight from `payload[6:]` |
| `09` | `07` | reset (timer) | time from `payload[2:]`, weight from `payload[6:]` |
| other | — | unknown button | — |

### 3b. Settings / status message (frame TYPE `08`)

When the frame TYPE is `08`, the payload carries device status, including battery:

| Offset | Field | Notes |
| ----------- | ----------- | ----------- |
| `[1]` | Battery | `payload[1] & 0x7F` = percent remaining (high bit masked off). |
| `[2]` | Units | `2`=grams, `5`=ounces. |
| `[4]` | Auto-off | value × 5 (minutes). |
| `[6]` | Beep | `1` = beeper on. |

## 4. Notes & Differences vs. BooKoo

- Acaia frames are **variable length** with a 2-byte header (`EF DD`), a type byte, a length byte, and **two** trailing checksum bytes — unlike BooKoo's fixed 20-byte weight packet with a single XOR checksum.
- Acaia encodes the weight's decimal places via a **unit/scale byte** (÷10 … ÷10000); BooKoo always sends grams × 100.
- Acaia requires an **active handshake and periodic heartbeat** to keep streaming; BooKoo streams weight without a keep-alive.
- Battery arrives in a **separate settings frame** (TYPE `08`) on Acaia, whereas BooKoo includes battery in every weight packet.
- Two characteristic layouts exist (old vs. Pyxis/Lunar-2021 style); the driver auto-detects which by scanning the connected device's characteristics.

---

* Acaia has no official public protocol document; treat these details as community-reverse-engineered and subject to change across firmware/models.*
