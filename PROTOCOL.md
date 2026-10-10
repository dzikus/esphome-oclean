# Protocol and architecture

How the component talks to the brushes, the wire formats, and how it is built
and extended. Installing and configuring it is in the [README](README.md).

## BLE topology

The TYPE1 layout, which the X Ultra 20 shares; the X Ultra 20 adds the BluFi
service for Wi-Fi provisioning.

| Service | Characteristic | Use |
|---|---|---|
| `8082caa8-41a6-4021-91c6-56f9b954cc18` | `9d84b9a3-000c-49d8-9183-855b673fbb85` (WRITE) | Tx, most commands |
| `8082caa8-41a6-4021-91c6-56f9b954cc18` | `5f78df94-798c-46f5-990a-855b673fbb86` (READ/NOTIFY) | Rx, status / settings / acks |
| `8082caa8-41a6-4021-91c6-56f9b954cc18` | `5f78df94-798c-46f5-990a-855b673fbb89` (WRITE) | Tx, session-download command |
| `8082caa8-41a6-4021-91c6-56f9b954cc18` | `5f78df94-798c-46f5-990a-855b673fbb90` (NOTIFY) | Rx, session record stream |
| `0x180F` | `0x2A19` (READ/NOTIFY) | battery percent, single byte |
| `0x180A` | `0x2A24` / `0x2A26` / `0x2A27` / `0x2A28` (READ) | model / firmware revision / HW revision / SW revision |
| `0xFFFF` | `0xFF01` (WRITE) | X Ultra 20: BluFi frames, Wi-Fi provisioning |
| `0xFFFF` | `0xFF02` (NOTIFY) | X Ultra 20: BluFi replies, the Wi-Fi connection report |

No pairing, no bonding, no auth: connect and write. Integers are big-endian,
frames carry no CRC. Write With Response is mandatory; Write No Response is
silently dropped by the brush. With the logger at `VERBOSE` the hub logs the
brush's GATT services and characteristics on each connect.

## Component layout

```
components/oclean/
  __init__.py              hub config + schema, adaptive-poll validation, model entity sets
  sensor.py                sensor table (per-model sets in __init__.py)
  binary_sensor.py         binary sensor table
  text_sensor.py           text sensor table
  switch.py                command and voice switches + the local bluetooth switch
  number.py                head_max_minutes + 8 custom-program parameters
  select.py                scheme presets + custom modes, language table
  button.py                capture / reset-head / sync-clock / poll-now

  oclean_protocol.{h,cpp}  pure C++: command table, session + settings
                           assemblers, record decode, scheme/clock/toggle/
                           cloud-host/blufi builders, cloud-upload body
                           parsing, weather mapping and reply, adaptive-poll
                           helpers
  oclean_profile.{h,cpp}   model-string to profile dispatch
  oclean.{h,cpp}           OcleanHub: BLE client node + PollingComponent +
                           poll state machine, NVS persistence
  oclean_cloud_receiver.{h,cpp}  in-node http session receiver and weather
                           answer (X Ultra 20, opt-in via cloud_receiver)
  oclean_switch.h          OcleanCommandSwitch / OcleanVoiceSwitch / OcleanBleSwitch
  oclean_number.h          OcleanHeadMaxNumber / OcleanCustomParamNumber
  oclean_button.h          the button classes
  oclean_select.h          OcleanSchemeSelect / OcleanLanguageSelect
```

`oclean_protocol.{h,cpp}` and `oclean_profile.{h,cpp}` have no ESPHome
dependencies; the PlatformIO unit tests build them on the host. Everything else
needs the ESPHome runtime.

## BLE lifecycle

```
[IDLE] --poll due (adaptive cadence) --> [CONNECTING] --open + discovery--> [POLLING]
   ^                                                                            |
   |        queries done + hold elapsed, or 60 s whole-poll watchdog            |
   +----------------------------------------------------------------------------+
```

- A poll cycle enables the BLE client, waits for the GATT open and service
  discovery and resolves all characteristic handles synchronously in the
  search-complete event. After a settle delay it reads battery and device
  information (cached for 24 h). The model string picks the profile, the
  profile picks the connection type, and only then does the hub register for
  notifies and send the profile's queries (TYPE1: STATUS, SETTINGS, session
  download).
- Pending writes queued by HA controls flush at the start of the query phase
  of the next connect; a write while idle raises the link immediately. A write
  the brush turns down while a session runs (`<opcode> 45 52`, "ER") is queued
  once more for the next round; a program split over `02 06` and `02 0B` is
  resent as a pair.
- Once a session stream has arrived whole, its events are out and the watermark
  is stored, TYPE1 confirms the batch with `02 02`. The brush then stops serving
  those records, so nothing is confirmed after a broken stream, a count=0 reply,
  or a batch holding a record dated implausibly far ahead.
- The clock is the exception: it is set at the end of the round, after the
  session download, and read back on the same link. A ring read after the set
  was stamped before it, in the old time base. When the set moved the clock
  back, the session watermark moves back by the same amount once the readback
  confirms it.
- The link is dropped after a short hold (8 s normal poll, 30 s capture). A
  60 s watchdog tears down a stuck connected cycle. A cycle that has not reached
  the brush keeps the client enabled and runs on the brush's next advertisement:
  the X Ultra 20 advertises only for about 2.5 min after a button, motion or
  charger wake.
- With `hold_connection_while_docked` a poll that reads back a docked state
  keeps the link, re-queries every `charging_interval` (each round under its
  own watchdog), and leaves the hold when STATUS reports off-dock, the link
  drops, or the bluetooth switch turns OFF.
- The brush sends nothing unasked, so leaving the dock during a hold shows up
  at the next held re-query, up to `charging_interval` later.

Timings (from `oclean_protocol.h`): post-connect settle 800 ms, whole poll 60 s,
boot stagger 90 s per hub, capture hold 30 s, poll hold 8 s, DIS cache 24 h,
enrichment wait 2.5 s, clock readback 2 s, queued-write spacing 300 ms, query
spacing 500 ms, backfill publish spacing 1.5 s.

## Command set

All commands go to the main write characteristic (`...fbb85`) as Write With
Response, except the session download, which goes to `...fbb89`, and the BluFi
frames, which go to `0xFF01`. Accepted
writes are acked with `<opcode> 4F 4B` ("OK") on the main notify
characteristic; rejected opcodes return a one-byte `02` stub.

| Bytes | Meaning |
|---|---|
| `03 03` | STATUS: 8-byte reply (6 on the X Ultra 20), battery at byte 5, cell voltage in mV at bytes 3-4 BE, dock/charge state at byte 2 (`01` charging, `02` off dock, `03` docked and full) |
| `03 02 01` | SETTINGS: replied as a two-frame transfer reassembled into a 34-byte buffer |
| `02 02` | confirm the downloaded sessions: the brush zeroes its unread count. Sent once a whole stream is ingested; on the X Ultra 20 it clears the store after the inline record, only while docked. Never sent by a `read_only` hub, the X Pro 20 / first X Ultra profile or an unrecognised model |
| `03 07` | session download (reply streams on the session notify characteristic) |
| `02 01` + 8B | set clock: `[year-2000][month][day][hour][min][sec][weekday][tzindex]`, plain decimal bytes, local time, weekday 0 = Sunday |
| `02 0F` | reset brush-head counter |
| `02 17` + 2B BE | brush-head time limit, minutes of brushing |
| `02 06` / `02 0B` | brushing-scheme program (split frames) |
| `02 16` + 1B | display language id (X Ultra 20: also the voice prompt language) |
| `02 0D` / `02 12` / `02 22` / `02 23` / `02 09` + 1B | config toggles (area reminder, over-pressure, brush pause, raise wake, brush mode; brush-mode off byte is `EC`) |
| `02 25` / `02 28` + 1B | X Ultra 20: auto mode, holiday reminder |
| `02 30` / `02 A0` + 1B | X Ultra 20: voice teaching (ack `02 30 4F 4B`), retail display mode (ack `02 A0 <value> 4F 4B`) |
| `02 31` + 4B | X Ultra 20: voice prompts, `[main][fast brushing][over-pressure][00]` in one frame; the zone-change prompt is `02 0D` |
| `03 14` / `03 16` / `03 A0` | X Ultra 20 reads: running state, zone guidance, retail display mode; reply `<op> <status> 4F 4B` |
| `02 34` | X Ultra 20 read: Wi-Fi provisioned (status 1), same reply shape; not sent by a `read_only` hub |
| `02 33 2A` + 2B + URL | X Ultra 20: the cloud host the brush uploads to, `[total_len][pkt_len] <ascii url>` (the lengths are equal in one packet), at most 59 bytes; an empty URL makes the brush fall back to its built-in host. No read-back |
| `02 11` + 4B | X Ultra 20: birthday greeting, `[gender][age][month][day]`; the brush shows its birthday screen on every wake that day. No read-back |
| BluFi | X Ultra 20: Wi-Fi provisioning on `0xFF01`, the unencrypted variant of Espressif BluFi (security mode, station opmode, SSID, password, connect, status request); the brush reports the connection on `0xFF02` |

## Settings buffer (34 bytes)

The SETTINGS reply is two `03 02` notifies: the start frame (`03 02 23 24` +
16 payload bytes) fills buffer `[0..16)`, the continuation (`03 02` + 18
payload bytes) fills `[16..34)`. `SettingsAssembler` accepts them in either
order. Confirmed offsets on the X Pro Elite:

| Offset | Field |
|---|---|
| 0 | device theme |
| 1 / 2 | brush pause / raise wake (!=0) |
| 3, 4, 8-10, 13 | X Pro Elite: no setting (constant zero, copies of bytes 0 and 1, a flag nothing writes, a pause flag cleared at each session start) |
| 11 | active scheme id (pNum) |
| 12 | brush mode (off sentinel 0xEC) |
| 14-15 | head used time, minutes (BE) |
| 16-21 | device clock (year-2000, month, day, hour, min, sec) |
| 22 / 23 | over-pressure / area reminder |
| 24 | timezone index (1-based, 33-entry GMT table) |
| 25-26 / 27-28 / 29-30 | head time limit in minutes / used days / used sessions (BE) |
| 31 | device language id |

The X Ultra 20 fills the start frame with other fields; its continuation frame
(16-33) has the layout above.

| Offset | Field |
|---|---|
| 0 | battery, percent |
| 1 | network flag (nothing in firmware 0.0.1.6 writes it) |
| 2 | raise wake (!=0) |
| 3 | auto-update flag (`02 32`; no update path reads it) |
| 4 | auto mode (!=0) |
| 5 | mode number, moves with byte 11 |
| 6 | voice teaching (!=0) |
| 7 / 8 / 9 | voice prompts / on fast brushing / on over-pressure (!=0) |
| 10 | holiday reminder (!=0) |
| 11 | active mode: 1-5 picked on the screen, 6 voice teaching, otherwise the id of a program written over BLE |
| 12 | brush mode (off sentinel 0xEC) |
| 13 | scheme type, raw |
| 14-15 | head used time (BE); firmware 0.0.1.6 never counts head use |

## Session stream and record

The `03 07` reply on the session notify characteristic starts with
`03 07 2A 42 23 [count_hi] [count_lo]`, then inline record bytes;
continuation notifies are raw bytes. `SessionAssembler` concatenates until
`count * 42` bytes are in, then cuts 42-byte records. The device ring holds 32
records (the assembler accepts up to 64); the ring is not chronological, the
newest record is found by timestamp.

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | year - 2000 |
| 1-5 | 5 | month / day / hour / min / sec (brush clock) |
| 6 | 1 | scheme id (pNum) |
| 7-8 | 2 BE | program length (s) |
| 9-10 | 2 BE | time actually brushed (s); equals the program length when it ran to the end |
| 11-16 | 6 | motion histogram from the IMU (not decoded) |
| 17 | 1 | time zone index of the brush clock when the session was recorded |
| 19-22 | 4 | share per quadrant, summing to 100: upper left, lower left, upper right, lower right |
| 23-30 | 8 | gesture array (left 0-3, right 4-7) |
| 33 | 1 | score 0-100 (`0xFF` = none, `1` = session voided: 14 s or less of brushing, or 85% or more of it without motion) |
| 34 | 1 | points (not decoded) |

The full ring is sent only when unread sessions exist. Otherwise the reply is
a single inline notify: the count=0 header plus the first 13 bytes of ring slot
0. Sessions after a confirmation are written from slot 0 on, so that is the
oldest session of the last batch handed over, not the newest. The fragment
holds timestamp, scheme, duration and valid duration (`decode_inline_0307`);
score and zones do not fit and are published only from full records. A
timestamp gate keeps an inline decode from replacing a newer session and from
overwriting the score and zones of an already-published one.

The X Ultra 20 (TYPE_V20) frames the same reply differently:
`03 07 2A 42 23 [count u16 BE] [stream length u16 BE]`, then length-prefixed
records of 58 to 182 bytes (`VarSessionAssembler`, at most 33 records). Firmware
0.0.1.6 never streams them: the reply is always count 0 followed by 11 bytes of
the store from its start (length, start time, mode, program length), the
oldest session since the store was last cleared. The record length is
min(182, brushed / 2 + 51), so the brushed time comes back from it to within
1 s below 182 bytes; a record cut at 182 only says 262 s or more. The hub
treats that record as a session: one event, and the entities when it is newer
than what they show, with no score or zones. While the brush is docked, as this
round's STATUS reads it, the hub then clears the store with `02 02` (once per
record, until the brush acks it), so the next session is written at the start
of the store and heads the next reply. Two sessions between docked polls show
only the first. A brush with Wi-Fi set up uploads its records to its cloud host
and drops them from the store instead; with `cloud_receiver` that host is the
node, which is how the full record below, score, zones and force log included,
reaches Home Assistant (see **In-node session receiver** in the
[README](README.md#in-node-session-receiver)).

| Offset | Size | Field |
|---|---|---|
| 0-1 | 2 BE | record length |
| 2-7 | 6 | year - 2000 / month / day / hour / min / sec (brush clock) |
| 8 | 1 | mode (the brush's own modes, not the cloud scheme ids) |
| 9-10 | 2 BE | program length (s) |
| 11-12 | 2 BE | brushed time (s) |
| 19 | 1 | time zone index of the brush clock when the session was recorded |
| 20-27 | 8 | seconds brushed in zones 1-8, the back teeth (firmware 0.0.1.6 fills only byte 20 and leaves `0xFF` in 21-27; 0.0.2.1 fills all eight) |
| 28 | 1 | score 0-100 (`0xFF` = none) |
| 32-35 | 4 | seconds brushed in zones 9-12, the front teeth |
| 51 on | 1 per 2 s | force log: force / 4, `0xFF` from 1000 up |

Each zone byte is the low byte of the brush's seconds count. The score is the
brush's own: every zone brushed 5 s or more counts 84, a shorter one pro rata,
and the sum over 10 is the score, which matches all nine records seen from a
brush running firmware 0.0.2.1.

## Scheme write format

A brushing scheme is a full per-step program, not an id:

```
02 06 [pNum][stepCount] (enc_gear, gear, duration_s)*N 00 05
```

`enc_gear` is a hint byte: gears 1-12 map to a fixed table, others encode as 0.
A program over 20 bytes is split: the first write carries the first 16 logical
bytes plus a `2A 2B` marker, the second starts `02 0B` and carries the rest.
Programs of up to 4 steps always fit a single frame; the split path is built
and unit-tested but has not been exercised on hardware. The firmware accepts
and persists arbitrary programs under non-preset ids (verified on hardware
with a custom id).

The X Ultra 20 firmware takes the same frame, ignores the hint byte, runs gears
1-54 and reports the program id in settings buffer 11. It turns auto mode off
on every program write, even one it refuses with `02 06 45 52` during a
session.

## Adding a new entity

Each settings-backed entity follows the same shape:

1. Decode the field in `parse_device_settings` (`oclean_protocol.cpp`) and add
   it to the `DeviceSettings` struct; add a host unit test against a real
   captured frame.
2. Add a `set_*` pointer setter and member on `OcleanHub` (`oclean.h`) and
   publish from the settings-readback path in `oclean.cpp`.
3. Add a row to the platform table in the matching `.py` file (key, setter
   name, icon, category, default name). Auto-create and `device_id`
   propagation come from the shared `_inject_defaults` pattern.
4. For a writable control, build the command in `oclean_protocol.{h,cpp}`
   (host-testable) and route it through `OcleanHub::send_command`, which
   queues while disconnected and wakes the link. Publish optimistically and
   let the settings readback correct the state.
5. List the key under `unavailable` in `MODEL_ENTITY_SETS` for every model
   whose firmware does not act on it, so each brush gets only what it supports.

Writes are mutations of someone's toothbrush: confirm the readback and the
physical effect on hardware before a new control ships.

## Testing

Unit tests under `tests/test_protocol/` build with PlatformIO + Unity. They
build `oclean_protocol` and `oclean_profile` and run on the host (no ESP32
required), covering the command builders, the session and settings assemblers,
record and inline decode, clock drift, timezone decode, the adaptive-poll
helpers and the cloud-upload, BluFi and weather helpers, with fixtures taken
from real captured frames.

```sh
pio test -d tests -e native
tests/.pio/build/native/program
```

The second line runs the produced binary directly for the authoritative Unity
summary and exit code. `python -m unittest discover -s tests/python` runs the
code generation and table tests; it needs esphome installed.

New functionality comes with tests in the same pull request: protocol,
decoding and command builders in `tests/test_protocol/`, code generation and
the entity tables in `tests/python/`. CI (`.github/workflows/test.yml`) runs ruff +
pre-commit, the unit tests, and a full-component compile on every push. A
devcontainer (`.devcontainer/`) provides esphome, platformio, ruff and
pre-commit.
