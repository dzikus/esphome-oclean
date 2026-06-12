# ESPHome Oclean

![tests](https://github.com/dzikus/esphome-oclean/actions/workflows/test.yml/badge.svg?branch=main)

<a href="https://www.buymeacoffee.com/dzikus" target="_blank"><img src="https://cdn.buymeacoffee.com/buttons/v2/default-yellow.png" alt="Buy Me A Coffee" style="height: 60px !important;width: 217px !important;" ></a>

ESPHome external component that exposes a Xiaomi **Oclean** BLE electric
toothbrush to Home Assistant. One component instance ("hub") per brush; several
hubs run on a single ESP32 with their first polls staggered so the radio is not
contended.

The component reads battery, dock/charge state, device settings and the buffered
brushing sessions, and writes back a small set of controls: brushing mode
(including custom programs), over-pressure alert, raise-to-wake, brush-head
replacement days, brush-head counter reset, display language, clock.

It connects only to poll and then disconnects (connect-poll-disconnect), so it
does not hold the brush's BLE radio open and keeps brush battery drain low. The
brush buffers sessions internally and never streams while brushing; the
component downloads the records after the fact.

The document is split in two:

- **Part 1 - Integrator** (yaml only): how to wire a brush into an ESPHome
  device and what entities you get.
- **Part 2 - Extender** (C++ + python): how the component is structured, the
  BLE lifecycle, the wire formats, and how to add a new entity.

---

## Installation

This repository ships three pieces that install by different mechanisms.

### 1. ESPHome component (the brush firmware component)

Not a HACS item: ESPHome pulls external components straight from GitHub. Add to
your ESPHome device YAML:

```yaml
external_components:
  - source: github://dzikus/esphome-oclean
    ref: v1.0.0
    components: [oclean]
```

Pinning `ref` to a release tag is the recommended form: a floating `main`
(optionally with `refresh: 1d`) pulls whatever is on the branch at build time.
Manual alternative: copy `components/oclean/` next to your device YAML and use
`source: components`. Part 1 covers the YAML in full.

### 2. Coverage card (HACS "Dashboard", optional)

HACS -> top-right menu -> **Custom repositories** -> URL
`https://github.com/dzikus/esphome-oclean`, category **Dashboard** -> **Add**.
Open the new entry, **Download**. On storage-mode dashboards HACS registers the
resource automatically; add a card of type `custom:oclean-coverage-card`. If the
card does not resolve (YAML-mode dashboards), add the resource by hand: Settings
-> Dashboards -> menu -> **Resources** -> **Add**, URL
`/hacsfiles/esphome-oclean/oclean-coverage-card.js`, type **JavaScript module**.

Manual alternative: copy `dist/oclean-coverage-card.js` to `config/www/` and add
the resource `/local/oclean-coverage-card.js`.

### 3. Statistics bridge (HACS "Integration", optional)

Backfills brushing history into long-term statistics under real past timestamps.
HACS -> **Custom repositories** -> the **same** URL, category **Integration** ->
**Add** -> open -> **Download** -> restart Home Assistant. Then map your brushes
in `configuration.yaml` (see [Session history](#session-history-in-home-assistant)).

Manual alternative: copy `custom_components/oclean_stats/` to
`config/custom_components/` and restart.

The card and the bridge are added as two separate custom repositories with the
same URL because HACS keys a repository by (URL, category).

---

## What this is

### Hardware

The protocol profile is selected at runtime from the device model string (DIS
characteristic `0x2A24`), so one build serves the whole family rather than
being hardcoded to a single model.

| Line | Model id (DIS 0x2A24) | Profile | Status |
|---|---|---|---|
| X / X Pro / Pro Elite / Ultra / Pro 20 | `OCLEANY3`, `OCLEANY3M*`, `OCLEANY3P*`, `OCLEANV1`, `OCLEANX20` | TYPE1 | X Pro Elite (`OCLEANY3P` / `OCLEANY3PD`) verified on hardware; others untested |
| Z1 | `OCLEANY5` | TYPE_Z1 | untested (needs a capture to freeze the record layout) |
| other / new firmware | unmatched | UNKNOWN fallback | battery + status only |

Everything below was verified empirically on two X Pro Elite brushes unless
noted. For untested models the session-record layout is not considered frozen;
battery and status are the safe baseline. Reports and PRs welcome.

ESP32 side: any board capable of `ble_client`. The default esp-idf BLE host
limit is 3 concurrent connections; raise `max_connections` only when the node
hosts more BLE clients than that.

### BLE topology (TYPE1 profile)

| Service | Characteristic | Use |
|---|---|---|
| `8082caa8-41a6-4021-91c6-56f9b954cc18` | `9d84b9a3-000c-49d8-9183-855b673fbb85` (WRITE) | Tx, most commands |
| `8082caa8-41a6-4021-91c6-56f9b954cc18` | `5f78df94-798c-46f5-990a-855b673fbb86` (READ/NOTIFY) | Rx, status / settings / acks |
| `8082caa8-41a6-4021-91c6-56f9b954cc18` | `5f78df94-798c-46f5-990a-855b673fbb89` (WRITE) | Tx, session-download command |
| `8082caa8-41a6-4021-91c6-56f9b954cc18` | `5f78df94-798c-46f5-990a-855b673fbb90` (NOTIFY) | Rx, session record stream |
| `0x180F` | `0x2A19` (READ/NOTIFY) | battery percent, single byte |
| `0x180A` | `0x2A24` / `0x2A27` / `0x2A28` (READ) | model / HW revision / SW revision |

No pairing, no bonding, no auth: connect and write. Integers are big-endian,
frames carry no CRC. Write With Response is mandatory; Write No Response is
silently dropped by the brush.

### What it exposes per brush

Numbers below are with the default `expose_dev_sensors: false`. With
`expose_dev_sensors: true` you also get dev entities (settings readbacks and
toggles with no observable effect on the verified brushes, the
session-capture button). Several always-on entities are created
with `disabled_by_default: true`, so they stay hidden in HA until enabled per
entity.

- 18 sensors always on: battery, last-session score / duration / valid
  duration / coverage, 8 per-zone gesture values, brush-head used days /
  sessions / used time, device theme, clock drift. 1 dev sensor: volume index.
  (device theme, head used time and clock drift are hidden by default.)
- 4 binary sensors always on: charging, docked, BLE connected (hidden),
  auto mode (hidden). 4 dev binary sensors: volume / calendar / splash
  prevention / fill brush readbacks.
- 9 text sensors always on: last session, last session mode, device clock,
  hardware revision, software version, last seen, timezone, MAC address, model
  (the last six hidden).
- 3 switches always on: over-pressure alert, raise to wake, bluetooth (BLE
  link master switch). 3 dev switches: area reminder, brush pause, brush mode.
- 9 numbers: head replacement days plus 8 custom-program step parameters.
- 2 selects: brushing mode, display language.
- 3 buttons always on: reset brush head, sync clock (needs `time_id`),
  poll now (hidden). 1 dev button: capture sessions.

---

## Part 1 - Integrator (YAML)

### Minimum config

This component uses the ESPHome sub-device API and current entity APIs, so it
needs **ESPHome 2026.1.0 or newer**. Pin it with
`esphome: { min_version: 2026.1.0 }` so an older install fails fast instead of
erroring deep in code generation.

Replace the MAC with the brush's MAC (any BLE scanner shows it while the brush
is awake). `v1.0.0` is the first release tag, cut by the release workflow when
the repo is published; drop the pin to track main.

```yaml
external_components:
  - source: github://dzikus/esphome-oclean@v1.0.0
    components: [oclean]

time:
  - platform: homeassistant
    id: ha_time

ble_client:
  - id: ble_brush
    mac_address: AA:BB:CC:DD:EE:FF

oclean:
  - id: brush_hub
    ble_client_id: ble_brush
    time_id: ha_time

sensor:
  - platform: oclean
    oclean_id: brush_hub

binary_sensor:
  - platform: oclean
    oclean_id: brush_hub

text_sensor:
  - platform: oclean
    oclean_id: brush_hub

switch:
  - platform: oclean
    oclean_id: brush_hub

number:
  - platform: oclean
    oclean_id: brush_hub

select:
  - platform: oclean
    oclean_id: brush_hub

button:
  - platform: oclean
    oclean_id: brush_hub
```

That creates every default entity, named in English, with default icons and
categories. Each platform auto-creates its entities; nothing has to be listed
key by key. Every individual entity can still be customised; see **Override
per-entity** below. A complete single-brush config is in
[`example.yaml`](example.yaml).

### Hub options

Set on the `oclean:` entry, not on the platforms.

| Option | Type | Default | Effect |
|---|---|---|---|
| `ble_client_id` | id | - | Required. Points to the `ble_client` entry with this brush's MAC. |
| `update_interval` | time | `3600s` (min `60s`) | Off-dock cadence: gap between connect-poll-disconnect cycles while the brush runs on battery. |
| `charging_interval` | time | `600s` (min `60s`) | Docked cadence: faster polls while the brush sits on the dock (charging or fully charged). Clamped down to `update_interval` if set larger; set both equal for fixed-interval polling. |
| `hold_connection_while_docked` | bool | `true` | Keep the BLE link open while the brush is docked instead of disconnecting after each poll; re-queries on the live link every `charging_interval`. The link drops when the brush leaves the dock. Docked means charging, so this costs no brush battery. Set `false` for plain connect-poll-disconnect. |
| `time_id` | id | none | A `time:` platform id (local time source). Enables the sync-clock button, auto clock-sync and the wall-clock stamps (last seen, session timestamps). |
| `tzindex` | int 1-33 | `16` | 1-based index into the brush's 33-entry GMT-offset table, written together with the clock. 16 = CEST (UTC+2), 15 = CET (UTC+1). |
| `auto_sync_time` | bool | on when `time_id` is set, off otherwise | Resync the brush clock during a poll when it has drifted past `sync_drift_threshold`. Explicit `true` without `time_id` fails validation. |
| `sync_drift_threshold` | time | `120s` | Drift that triggers an auto resync. `0s` resyncs whenever the clocks differ by at least one second. |
| `expose_dev_sensors` | bool | `false` | Creates the dev-gated entities (see the per-platform tables). |

The brushing-mode select additionally accepts `custom_modes` (a list of named
programs); that option lives under the `select:` platform, not the hub. See
**Entities (select)**.

Dock-aware adaptive polling is always on: the hub polls at `charging_interval`
while the brush is docked and at `update_interval` while it is off the dock.
Dock presence (not the charge phase) selects the cadence, so a fully charged
brush still on the dock keeps the fast cadence. With several hubs on one node
the first poll of hub N is deferred by N * 90 s after boot so the cycles do not
race for the single scanner.

### Entities (sensor)

All auto-created. "Hidden" means `disabled_by_default: true` in HA (enable per
entity). "Dev" rows exist only on hubs with `expose_dev_sensors: true`.

| Key | Default name | Source | Notes |
|---|---|---|---|
| `battery` | Battery | battery characteristic / STATUS | percent, diagnostic |
| `last_session_score` | Score | session record byte 33 | 0-100; the no-score sentinel (0xFF) reads as unknown |
| `last_session_duration` | Duration | session record bytes 7-8 BE | seconds |
| `last_session_valid_duration` | Valid duration | session record bytes 9-10 BE | seconds counted as effective |
| `last_session_coverage` | Coverage | derived | valid / duration, percent |
| `gesture_zone_1` .. `gesture_zone_8` | Zone 1 .. Zone 8 | session record bytes 23-30 | per-region values; 1-4 left, 5-8 right (upper-outer / upper-inner / lower-outer / lower-inner per side) |
| `head_used_days` | Brush head used days | settings buffer 27-28 BE | cumulative since head reset |
| `head_used_times` | Brush head sessions | settings buffer 29-30 BE | cumulative since head reset |
| `head_used_time` | Brush head used time | settings buffer 14-15 BE | hidden; unit unconfirmed |
| `device_theme` | Device theme | settings buffer 0 | hidden; raw index |
| `volume_index` | Volume index | settings buffer 9 | **dev**; hidden; raw index |

The last decoded session survives reboots: the newest record is persisted in
NVS per hub and re-published on boot.

### Entities (binary_sensor)

| Key | Default name | Source | Notes |
|---|---|---|---|
| `charging` | Charging | STATUS byte 2 == 0x01 | actively charging on the dock |
| `docked` | Docked | STATUS byte 2 == 0x01 or 0x03 | on the dock, charging or fully charged |
| `connected` | BLE connected | link state | hidden; off almost always by design (the link is up only seconds per poll); use Last seen for freshness |
| `auto_mode` | Auto mode | settings buffer 4 | hidden; read-only (the brush rejects the write opcode) |
| `volume_enabled` | Volume enabled | settings buffer 8 (inverted) | **dev** |
| `calendar_enabled` | Calendar enabled | settings buffer 10 (inverted) | **dev** |
| `splash_prevent` | Splash prevention | settings buffer 13 | **dev** |
| `fill_brush` | Fill brush | settings buffer 3 | **dev**; read-only (write opcode rejected) |

### Entities (text_sensor)

| Key | Default name | Source | Notes |
|---|---|---|---|
| `last_session_time` | Last session | session record bytes 0-5 | timestamp of the newest buffered session (brush clock) |
| `last_session_mode` | Last session mode | session record byte 6 | scheme id decoded to the brushing-mode name; unknown ids fall back to the number |
| `device_clock` | Device clock | settings buffer 16-21 | the brush's own clock |
| `last_seen` | Last seen | wall clock | hidden; timestamp device class, renders "x ago" in HA; stamped on every successful poll, the freshness signal for the slow cadence |
| `timezone` | Timezone | settings buffer 24 | hidden; decoded GMT offset, e.g. "GMT+02:00" |
| `hw_revision` | Hardware revision | DIS 0x2A27 | hidden |
| `sw_version` | Software version | DIS 0x2A28 | hidden |
| `mac_address` | MAC address | BLE | hidden |
| `model` | Model | DIS 0x2A24 | hidden; the raw model id that drives profile selection |

### Entities (switch)

All device-backed switches publish optimistically and are then corrected by the
settings readback; their restore mode is `DISABLED` so nothing is written on
boot. The brush acks every accepted write with `<opcode> 4F 4B` ("OK").

| Key | Default name | Write | Notes |
|---|---|---|---|
| `over_pressure` | Over-pressure alert | `02 12` + 01/00 | readback at settings buffer 22 |
| `raise_wake` | Raise to wake | `02 23` + 01/00 | readback at settings buffer 2 |
| `bluetooth` | Bluetooth | local only | master switch for the BLE link; OFF drops pending writes and tears the link down; `RESTORE_DEFAULT_ON` so a reboot never leaves the brush silently unreachable |
| `area_reminder` | Area reminder | `02 0D` + 01/00 | **dev**; no observable effect on the verified brushes |
| `brush_pause` | Brush pause | `02 22` + 01/00 | **dev** |
| `brush_mode` | Brush mode | `02 09` + 01/EC | **dev**; off byte is the 0xEC sentinel, not 0x00 |

### Entities (number)

| Key | Default name | Range | Notes |
|---|---|---|---|
| `head_max_days` | Head replacement days | 1-365 | writes `02 17` + 2B BE; box input (a slider would fire a write per step) |
| `custom_step1_gear` .. `custom_step4_gear` | Custom step N gear | 1-41, default 8 | parameters of the runtime Custom program; stored on the node (flash-persisted), written to the brush only when Custom is selected |
| `custom_step1_duration` .. `custom_step4_duration` | Custom step N duration | 5-120 s, step 5, default 30 | same; changing a parameter while Custom is active re-programs the brush (debounced) |

### Entities (select)

| Key | Default name | Options | Notes |
|---|---|---|---|
| `brush_scheme` | Brushing mode | 19 presets + named `custom_modes` + "Custom" | writes the full per-step program (`02 06` / `02 0B`); current option read back from settings buffer 11 |
| `device_language` | Display language | 17 languages | writes `02 16` + language id; readback from settings buffer 31 |

Preset options are labelled "name (duration)", e.g. "Quick cleaning (1m20s)".
Named custom modes are declared under the select:

```yaml
select:
  - platform: oclean
    oclean_id: brush_hub
    brush_scheme:
      custom_modes:
        - name: "Evening strong"
          program:
            - { gear: 16, duration: 30 }
            - { gear: 16, duration: 30 }
            - { gear: 24, duration: 30 }
            - { gear: 16, duration: 30 }
        - name: "Morning express"
          program:
            - { gear: 8, duration: 20 }
            - { gear: 8, duration: 20 }
            - { gear: 8, duration: 20 }
            - { gear: 8, duration: 20 }
```

Up to 20 modes, 1-4 steps each, gear 1-41, duration 5-120 s. Modes get ids
121+ in list order (reordering shifts the ids, which only affects how old
session records decode). The runtime "Custom" option (id 120) builds its
program from the custom-step number entities at selection time. Step
boundaries double as the brush's pause signals and summary segments, so a
program wants four steps to keep the four-quadrant guidance.

### Entities (button)

| Key | Default name | Effect | Notes |
|---|---|---|---|
| `reset_head` | Reset brush head | writes `02 0F` | irreversible: zeroes the brush-head usage counters |
| `sync_time` | Sync clock | writes `02 01` + 8 bytes | created only when the hub has `time_id`; writes on press only |
| `poll_now` | Poll now | immediate poll cycle | hidden by default; read-only on the brush |
| `capture_sessions` | Capture sessions | session download + 30 s hold | **dev**; keeps the link open so the raw record stream lands in the log |

### Override per-entity

Every key on every platform accepts the normal ESPHome entity config. Override
the name, icon, category or any other entity field directly under the key:

```yaml
sensor:
  - platform: oclean
    oclean_id: brush_hub
    battery:
      name: "Brush Battery"
    last_session_score:
      name: "Brushing Score"
      icon: "mdi:star"
```

Schema defaults are injected before validation, so omitted fields keep their
defaults. If you do not set `name`, the default in the tables above is used.

### Two brushes on one ESP32

Two `ble_client` entries and two `oclean` hubs. Use `device_id` to put each
brush's entities under a separate sub-device in HA:

```yaml
esphome:
  devices:
    - id: dev_brush_a
      name: "Oclean A"
    - id: dev_brush_b
      name: "Oclean B"

ble_client:
  - id: ble_a
    mac_address: AA:BB:CC:DD:EE:FF
  - id: ble_b
    mac_address: AA:BB:CC:DD:EE:00

oclean:
  - id: hub_a
    ble_client_id: ble_a
    time_id: ha_time
  - id: hub_b
    ble_client_id: ble_b
    time_id: ha_time

sensor:
  - platform: oclean
    oclean_id: hub_a
    device_id: dev_brush_a
  - platform: oclean
    oclean_id: hub_b
    device_id: dev_brush_b
```

Repeat the platform pair for binary_sensor, text_sensor, switch, number,
select and button. Boot polls are staggered automatically.

### Session history in Home Assistant

Each new session from the brush's ring buffer fires an `esphome.oclean_session`
event (score, duration, valid duration, coverage, scheme, per-zone values,
timestamp). A per-brush watermark stored in NVS prevents re-emitting old
sessions across reboots.

The optional `oclean_stats` integration (Installation, path 3) writes these into
long-term statistics under their real past timestamps, so brushing history charts
even for sessions that happened while Home Assistant was down. Map each brush MAC
to a slug in `configuration.yaml`:

```yaml
oclean_stats:
  brushes:
    "AA:BB:CC:DD:EE:FF": alice
    "AA:BB:CC:DD:EE:00": bob
```

The MAC must match what the component reports (upper-case, colons); the slug
becomes part of the statistic id (`oclean:<slug>_score`), so keep it to
`[a-z0-9_]`. The bridge is read-only to the brush and creates no entities; the
statistics show up in a Statistics card pointed at `oclean:<slug>_score` and in
Settings -> Dashboards -> ... -> Statistics.

### Coverage card

`custom:oclean-coverage-card` draws the eight per-zone gesture values of the last
session as a colored mouth map (upper and lower arch, left/right side, outer/inner
surface). Read-only: it reads the zone / score / coverage entities and recorder
history and never talks to the brush. Install it through HACS (Installation,
path 2) or by hand.

```yaml
type: custom:oclean-coverage-card
title: Brushing coverage
zone_prefix: sensor.oclean_zone_   # expands to _1 .. _8
score_entity: sensor.oclean_score
coverage_entity: sensor.oclean_coverage
time_entity: sensor.oclean_last_session
```

| Option | Default | Meaning |
|---|---|---|
| `zones` | - | explicit list of 8 entities in gesture_zone_1..8 order (instead of `zone_prefix`) |
| `zone_prefix` | - | entity prefix that `1`..`8` is appended to |
| `title` | - | card header |
| `mirror` | `false` | swap the on-screen left / right sides |
| `normalize` | `share` | colouring: `share` (vs an even 1/8), `max` (vs the best surface), `absolute` (vs `target`) |
| `target` | `15` | per-surface target for `normalize: absolute` |
| `score_entity` / `coverage_entity` / `time_entity` | - | values shown in the header |
| `labels` | EN | override the on-card labels |

Clicking a surface opens the more-info dialog for that zone entity. Arrows and a
slider step through the sessions found in recorder history.

### Latency of writes

A control change calls into the hub, which raises the BLE link immediately if
idle; the latency is the time until the brush is connectable, not the poll
interval. A sleeping brush is not connectable: the queued write flushes on the
next successful connect (next poll, or wake the brush by pressing its button).

### App vs ESPHome

The brush accepts one BLE central at a time. While the component is connected
or connecting, the official app cannot pair. To use the app, turn the
`bluetooth` switch OFF on the brush's HA device, do the app work, then turn it
back ON.

---

## Part 2 - Extender (Architecture)

### Component layout

```
components/oclean/
  __init__.py              hub config + schema, adaptive-poll validation, dev gating
  sensor.py                17 + 1 dev sensor keys, schema + to_code
  binary_sensor.py         4 + 4 dev binary sensor keys
  text_sensor.py           8 + 1 dev text sensor keys
  switch.py                5 command switches + the local bluetooth switch
  number.py                head_max_days + 8 custom-program parameters
  select.py                scheme presets + custom modes, language table
  button.py                capture / reset-head / sync-clock / poll-now

  oclean_protocol.{h,cpp}  pure C++: command table, session + settings
                           assemblers, record decode, scheme/clock/toggle
                           builders, adaptive-poll helpers
  oclean_profile.{h,cpp}   model-string to profile dispatch
  oclean.{h,cpp}           OcleanHub: BLE client node + PollingComponent +
                           poll state machine, NVS persistence
  oclean_switch.h          OcleanCommandSwitch / OcleanBleSwitch
  oclean_number.h          OcleanHeadDaysNumber / OcleanCustomParamNumber
  oclean_button.h          the four button classes
  oclean_select.h          OcleanSchemeSelect / OcleanLanguageSelect
```

`oclean_protocol.{h,cpp}` has no ESPHome dependencies and is what the
PlatformIO unit tests link against. Everything else needs the ESPHome runtime.

### BLE lifecycle

```
[IDLE] --poll due (adaptive cadence) --> [CONNECTING] --open + discovery--> [POLLING]
   ^                                                                            |
   |        queries done + hold elapsed, or 60 s whole-poll watchdog            |
   +----------------------------------------------------------------------------+
```

- A poll cycle enables the BLE client, waits for the GATT open and service
  discovery, resolves all characteristic handles synchronously in the
  search-complete event, registers for notifies, then after a settle delay
  issues the query sequence: battery, device information (cached for 24 h),
  STATUS, SETTINGS, session download.
- Pending writes queued by HA controls flush at the start of the query phase
  of the next connect; a write while idle raises the link immediately.
- The link is dropped after a short hold (8 s normal poll, 30 s capture). A
  60 s whole-poll watchdog tears down a stuck cycle; a cycle killed before the
  GATT open retries at the next tick instead of waiting a full interval.
- With `hold_connection_while_docked` a poll that reads back a docked state
  keeps the link, re-queries every `charging_interval` (each round under its
  own watchdog), and leaves the hold when STATUS reports off-dock, the link
  drops, or the bluetooth switch turns OFF.
- The brush pushes a spontaneous STATUS on dock changes while connected, so
  leaving the dock is detected immediately during a hold.

Timings (from `oclean_protocol.h`): post-connect settle 800 ms, whole poll 60 s,
boot stagger 90 s per hub, capture hold 30 s, poll hold 8 s, DIS cache 24 h,
enrichment wait 2.5 s, queued-write spacing 300 ms, query spacing 500 ms,
backfill publish spacing 1.5 s.

### Command set

All commands go to the main write characteristic (`...fbb85`) as Write With
Response, except the session download which goes to `...fbb89`. Accepted
writes are acked with `<opcode> 4F 4B` ("OK") on the main notify
characteristic; rejected opcodes return a one-byte `02` stub.

| Bytes | Meaning |
|---|---|
| `03 03` | STATUS: 8-byte reply, battery at byte 5, dock/charge state at byte 2 (`01` charging, `02` off dock, `03` docked and full) |
| `03 02 01` | SETTINGS: replied as a two-frame transfer reassembled into a 34-byte buffer |
| `02 02` | device info (ack only on the verified family) |
| `03 07` | session download (reply streams on the session notify characteristic) |
| `02 01` + 8B | set clock: `[year-2000][month][day][hour][min][sec][weekday][tzindex]`, plain decimal bytes, local time, weekday 0 = Sunday |
| `02 0F` | reset brush-head counter |
| `02 17` + 2B BE | head replacement days |
| `02 06` / `02 0B` | brushing-scheme program (split frames) |
| `02 16` + 1B | display language id |
| `02 0D` / `02 12` / `02 22` / `02 23` / `02 09` + 1B | config toggles (area reminder, over-pressure, brush pause, raise wake, brush mode; brush-mode off byte is `EC`) |

### Settings buffer (34 bytes)

The SETTINGS reply is two `03 02` notifies: the start frame (`03 02 23 24` +
16 payload bytes) fills buffer `[0..16)`, the continuation (`03 02` + 18
payload bytes) fills `[16..34)`. `SettingsAssembler` accepts them in either
order. Confirmed offsets:

| Offset | Field |
|---|---|
| 0 | device theme |
| 1 / 2 / 3 / 4 | brush pause / raise wake / fill brush / auto mode (!=0) |
| 8 / 10 | volume / calendar enabled (inverted: 0 = enabled) |
| 9 | volume index |
| 11 | active scheme id (pNum) |
| 12 | brush mode (off sentinel 0xEC) |
| 13 | splash prevent |
| 14-15 | head used time (BE) |
| 16-21 | device clock (year-2000, month, day, hour, min, sec) |
| 22 / 23 | over-pressure / area reminder |
| 24 | timezone index (1-based, 33-entry GMT table) |
| 25-26 / 27-28 / 29-30 | head max days / used days / used sessions (BE) |
| 31 | device language id |

### Session stream and record

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
| 7-8 | 2 BE | duration (s) |
| 9-10 | 2 BE | valid duration (s) |
| 11-15 | 5 | area values |
| 23-30 | 8 | gesture array (left 0-3, right 4-7) |
| 33 | 1 | score 0-100 (`0xFF` = none) |

The full ring is sent only when unread sessions exist. Otherwise the reply is
a single inline notify: the count=0 header plus the first 13 bytes of the
newest already-read record, enough for timestamp, scheme, duration and valid
duration (`decode_inline_0307`); score and zones do not fit and are published
only from full records. A timestamp gate keeps a partial inline decode from
overwriting the score/zones of an already-published session.

### Scheme write format

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

### Adding a new entity

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
5. Gate it behind `expose_dev_sensors` (add the key to the platform's dev-key
   set) until its effect is verified on hardware.

Writes are mutations of someone's toothbrush: keep new controls dev-gated
until the readback and the physical effect are both confirmed.

### Testing

Unit tests under `tests/test_protocol/` build with PlatformIO + Unity. They
link only `oclean_protocol.{h,cpp}` and run on the host (no ESP32 required),
covering the command builders, the session and settings assemblers, record and
inline decode, clock drift, timezone decode and the adaptive-poll helpers,
with fixtures taken from real captured frames.

```sh
pio test -d tests -e native
tests/.pio/build/native/program
```

The second line runs the produced binary directly for the authoritative Unity
summary and exit code. CI (`.github/workflows/test.yml`) runs ruff +
pre-commit, the unit tests, and a full-component compile on every push. A
devcontainer (`.devcontainer/`) provides esphome, platformio, ruff and
pre-commit.

---

## Constraints and quirks

| Constraint | Effect / workaround |
|---|---|
| Passive advertisements carry no data | Only name / MAC / RSSI; even battery needs an active GATT connection, hence connect-poll-disconnect. |
| The brush does not stream while brushing | Sessions are buffered and downloaded after the fact; expect them at the next poll, or press Poll now. |
| One BLE central at a time | The official app cannot connect while the component holds the link. Use the `bluetooth` switch to release it. |
| Session timestamps use the brush clock | Drift shifts session times; `auto_sync_time` (with a `time_id`) keeps the clock within `sync_drift_threshold`. |
| Write No Response is dropped | All writes go out as Write With Response. |
| Some toggle opcodes are rejected by firmware | Fill brush and auto mode read back fine but their writes return an error stub; they are exposed as binary sensors, not switches. |
| The brush intensity level (display button) has no BLE representation | It can be neither read nor written; no entity exists for it. |
| Holding the link drains the brush | `hold_connection_while_docked` only ever holds while docked (charging, so no drain); off the dock the component always disconnects after each poll. On by default. |

## License

GPL-3.0. This component derives from a GPL-3.0 ESPHome component and inherits
that license. See [`LICENSE`](LICENSE).
