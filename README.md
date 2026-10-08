# ESPHome Oclean

[![tests](https://github.com/dzikus/esphome-oclean/actions/workflows/test.yml/badge.svg?branch=main)](https://github.com/dzikus/esphome-oclean/actions/workflows/test.yml)
[![codeql](https://github.com/dzikus/esphome-oclean/actions/workflows/codeql.yml/badge.svg?branch=main)](https://github.com/dzikus/esphome-oclean/actions/workflows/codeql.yml)
[![scorecard](https://api.scorecard.dev/projects/github.com/dzikus/esphome-oclean/badge)](https://scorecard.dev/viewer/?uri=github.com/dzikus/esphome-oclean)
[![release](https://img.shields.io/github/v/release/dzikus/esphome-oclean?sort=semver)](https://github.com/dzikus/esphome-oclean/releases/latest)
[![license](https://img.shields.io/github/license/dzikus/esphome-oclean)](LICENSE)

<a href="https://www.buymeacoffee.com/dzikus" target="_blank"><img src="https://cdn.buymeacoffee.com/buttons/v2/default-yellow.png" alt="Buy Me A Coffee" style="height: 60px !important;width: 217px !important;" ></a>

ESPHome external component that exposes an **Oclean** BLE electric
toothbrush to Home Assistant. One component instance ("hub") per brush; several
hubs run on a single ESP32 with their first polls staggered so the radio is not
contended.

The component reads battery, dock/charge state, device settings and the buffered
brushing sessions, and writes back a small set of controls: brushing mode
(including custom programs), over-pressure alert, raise-to-wake, brush-head
time limit, brush-head counter reset, display language, clock.

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
    components: [oclean]
```

Add `ref:` with a release tag to pin a version; without it the build follows
`main` (optionally with `refresh: 1d`).
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
being hardcoded to a single model. The entity set cannot wait for that read,
because ESPHome creates entities at build time, so it comes from the hub's
`model:` option instead.

| Line | Model id (DIS 0x2A24) | Profile | Status |
|---|---|---|---|
| X / X Pro / Pro Elite | `OCLEANY3`, `OCLEANY3M*`, `OCLEANY3P*` | TYPE1 | X Pro Elite verified on hardware with `OCLEANY3P` firmware 1.0.0.30 and `OCLEANY3PD` firmware 1.0.0.31; other models and firmware versions untested |
| X Ultra 20 | `OCLEANV20*` | TYPE_V20 | status, settings, the clock and the setting writes verified on hardware (firmware 0.0.2.1). The brush never streams its sessions: each poll reads the oldest stored one, without score or zones (see **Session stream and record**). The program, voice-teaching and retail-mode writes follow the firmware image and are not yet tried on the brush |
| X Pro 20, X Ultra (first generation) | `OCLEANX20`, `OCLEANV1*` | TYPE_V20_FAMILY | untested; captures from both show the X Ultra 20 reply shapes. Use `model: x_ultra_20`. The store is never cleared, so only its oldest session shows up |
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
| `0x180A` | `0x2A24` / `0x2A26` / `0x2A27` / `0x2A28` (READ) | model / firmware revision / HW revision / SW revision |

No pairing, no bonding, no auth: connect and write. Integers are big-endian,
frames carry no CRC. Write With Response is mandatory; Write No Response is
silently dropped by the brush.

### What it exposes per brush

The entity set follows the hub's `model:` option, so each brush gets only the
entities it has data or an opcode for. A row whose only source is a hub option
is built only with that option (on the X Ultra 20: `cloud_receiver` for the
score, the zones, the cloud host and cloud host written, `birthday` for user
info written, `wifi_provisioning` for Wi-Fi written); naming it in yaml without
the option fails validation. Counts below are with the default
`expose_dev_sensors: false`, which leaves out the dev entities: rows with no
observable effect on that brush, and the session-capture button. Several
entities are created with `disabled_by_default: true`, so they stay hidden in
HA until enabled per entity.

X Pro Elite (`model: x_pro_elite`, the default):

- 23 sensors: battery, battery voltage, last-session score / duration / valid
  duration / coverage, 8 per-zone gesture values, 4 per-quadrant shares,
  brush-head used days / sessions / used time, device theme, clock drift (the
  quadrants, device theme, head used time and clock drift hidden).
- 3 binary sensors: charging, docked, BLE connected (hidden).
- 9 text sensors: last session, last session mode, device clock, hardware
  revision, software version, last seen, timezone, MAC address, model (the last
  six hidden).
- 3 switches: over-pressure alert, raise to wake, bluetooth (BLE link master
  switch). 3 dev switches: area reminder, brush pause, brush mode.
- 9 numbers: brush head time limit plus 8 custom-program step parameters.
- 2 selects: brushing mode, display language.
- 3 buttons: reset brush head, sync clock (needs `time_id`), poll now (hidden).
  1 dev button: capture sessions.

X Ultra 20 (`model: x_ultra_20`):

- 9 sensors: battery, battery voltage, last-session duration / valid duration
  / coverage, device mode, mode number, running state, clock drift (mode
  number, running state and clock drift hidden). With `cloud_receiver: true`
  also the last-session score and the 8 per-zone gesture values, which reach
  the node only in the brush's cloud upload. No quadrant sensors: the record's
  12-zone map and quadrants are not decoded. No brush-head counters: firmware
  0.0.1.6 never counts head use.
- 5 binary sensors: charging, docked, BLE connected (hidden), Wi-Fi
  provisioned, zone guidance; hidden, one per write the hub keeps on the brush:
  user info written (`birthday`), Wi-Fi written (`wifi_provisioning`), cloud
  host written (`cloud_receiver`).
- 9 text sensors, as above; with `cloud_receiver: true` also cloud host
  (hidden).
- 10 switches: raise to wake, voice on zone change (the `area_reminder` key),
  auto mode, holiday reminder, voice teaching, retail display mode, voice
  prompts, voice on fast brushing, voice on over-pressure, bluetooth. No
  over-pressure alert: firmware 0.0.1.6 stores its flag (`02 12`) and reads it
  nowhere else; the pressure prompt is the voice switch.
- 8 numbers: custom-program step parameters.
- 2 selects: brushing mode, language (the `device_language` key, which also
  sets the voice prompt language).
- 1 button: sync clock (needs `time_id`). No capture or poll button: BLE
  never hands over the stored sessions. A clock write stamps the sessions
  until the brush next asks the cloud for the time (`UploadingMacWiFi`, not
  after every brushing; the session receiver answers it with the node's time).

On the X Ultra 20 the brushing-mode select shows the mode picked on the screen
("Screen mode 1" .. "Screen mode 5") or "Voice teaching", and writes only
programs: "Custom" and the named `custom_modes`. A program write turns auto mode
off and moves the brush to its program slot; nothing over BLE moves it back to a
screen mode, which is picked on the brush again. The voice-teaching switch has
the same catch: on selects the teaching program, off selects screen mode 5.

---

## Part 1 - Integrator (YAML)

### Minimum config

This component uses the ESPHome sub-device API and current entity APIs, so it
needs **ESPHome 2026.6.0 or newer**. Pin it with
`esphome: { min_version: 2026.6.0 }` so an older install fails fast instead of
erroring deep in code generation.

Replace the MAC with the brush's MAC (any BLE scanner shows it while the brush
is awake). Append `@<tag>` to the source to pin a release.

```yaml
external_components:
  - source: github://dzikus/esphome-oclean
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

For an X Ultra 20 add `model: x_ultra_20` under `oclean:`; without it the hub
builds the X Pro Elite entity set.

That creates every default entity, named in English, with default icons and
categories. Each platform auto-creates its entities; nothing has to be listed
key by key. Every individual entity can still be customised; see **Override
per-entity** below. A complete single-brush config is in
[`example.yaml`](example.yaml).

Auto-creation is a deliberate departure from the usual ESPHome style, where
every entity is spelled out in YAML. One brush exposes around fifty of them, and
listing each by hand would be pages of boilerplate for a device whose entity set
is fixed by the protocol. The escape hatches are per-entity overrides and
`false` to drop one.

The four control platforms are optional. Leave `switch`, `number`, `select` or
`button` out and their code is not compiled into the firmware at all.

That alone does not make the node read-only: `auto_sync_time` writes the brush
clock (`0201`) on its own, and a poll that downloads new sessions confirms them
with `0202`. For a hub that never writes anything, set `read_only: true`.

### Hub options

Set on the `oclean:` entry, not on the platforms.

| Option | Type | Default | Effect |
|---|---|---|---|
| `ble_client_id` | id | - | Required. Points to the `ble_client` entry with this brush's MAC. |
| `model` | `x_pro_elite` or `x_ultra_20` | `x_pro_elite` | The brush on this hub, which picks the entity set at build time (see **What it exposes per brush**). The X / X Pro and the Z1 use `x_pro_elite`; the X Pro 20 and the first X Ultra use `x_ultra_20`. The protocol is still chosen from the model id the brush reports, so a wrong value costs entities, never data; the log names the right value after the first poll. An entity listed in yaml that the model does not have fails validation. |
| `update_interval` | time | `3600s` (min `60s`) | Off-dock cadence: gap between connect-poll-disconnect cycles while the brush runs on battery. |
| `charging_interval` | time | `600s` (min `60s`) | Docked cadence: faster polls while the brush sits on the dock (charging or fully charged). Clamped down to `update_interval` if set larger; set both equal for fixed-interval polling. |
| `hold_connection_while_docked` | bool | `true` | Keep the BLE link open while the brush is docked instead of disconnecting after each poll; re-queries on the live link every `charging_interval`. The link drops when the brush leaves the dock. Docked means charging, so this costs no brush battery. Set `false` for plain connect-poll-disconnect. |
| `time_id` | id | none | A `time:` platform id (local time source). Enables the sync-clock button, auto clock-sync and the wall-clock stamps (last seen, session timestamps). |
| `tzindex` | int 1-33 | `16` | 1-based index into the brush's 33-entry GMT-offset table, written together with the clock. 16 = CEST (UTC+2), 15 = CET (UTC+1). |
| `auto_sync_time` | bool | on when `time_id` is set, off otherwise | Resync the brush clock during a poll when it has drifted past `sync_drift_threshold`. Explicit `true` without `time_id` fails validation. |
| `sync_drift_threshold` | time | `120s` | Drift that triggers an auto resync. `0s` resyncs whenever the clocks differ by at least one second. |
| `expose_dev_sensors` | bool | `false` | Creates the dev entities of the hub's model (see the per-platform tables) and logs the brush's GATT services and characteristics on each connect. |
| `read_only` | bool | `false` | The brush only receives the profile's `03` read queries; the X Ultra 20's Wi-Fi check `02 34` is held back too. Every write is refused and logged: controls, clock sync, `0202`. |
| `name_prefix` | string, max 48 chars | unset | Prepended to every default entity name on this hub, so two brushes do not both call a sensor `Battery`. Opt-in: nothing is prefixed unless you write it here. Names you write yourself are never touched. `""` keeps the bare names and silences the multi-hub warning. See **Two brushes on one ESP32**. |
| `cloud_receiver` | bool | `false` | Receives the brush's cloud session uploads and publishes them as the session entities (routing by the MAC in each upload). This is the only way to the brushing score and full record on X Ultra 20 firmware, which BLE does not expose. It does **not** start its own server: it registers a handler on the shared ESPHome web server, so a `web_server:` must be configured (validation requires it) and the uploads arrive on the web server's port. The code is not compiled in unless this is true. The hub points the brush's cloud host at this node itself (see **Values kept on the brush**); the brush has to be able to reach the node over the network. See **In-node session receiver**. |
| `cloud_drop_future` | bool | `true` | What the receiver does with an upload dated implausibly far in the future, which comes from a brush whose clock was not corrected yet. `true` acks it, so the brush erases it instead of re-uploading it on every connect; `false` leaves it on the brush. Never published either way. Only matters with `cloud_receiver: true`. X Ultra 20 only. |
| `weather` | `weather.*` entity id | unset | Answers the brush's weather request from this Home Assistant weather entity, so its clock page shows an icon, Today/Tomorrow and the day's low and high. Needs `cloud_receiver: true`, `time_id` and an `api:` block; one hub per node. The forecast needs the device to be allowed to perform Home Assistant actions; without that it shows the current condition and temperature. X Ultra 20 only. See **Weather on the brush**. |
| `birthday` | `MM-DD` | unset | Birthday greeting date the hub keeps on the brush (see **Values kept on the brush**): on every wake that day the brush shows its birthday screen with the date, whether or not the holiday greetings are on. A yaml option baked into the firmware, never an entity, so the date stays out of the Home Assistant recorder; use `!secret`. X Ultra 20 only. |
| `gender` | `unknown`, `male`, `female` | `unknown` | Goes in the same frame as the date. Baked in like `birthday`, never an entity; use `!secret`. The brush stores it and shows nothing of it. X Ultra 20 only. |
| `age` | int 3-18 | `18` | Goes in the same frame. 3-18 is the app's range, where any adult is 18. Baked in, never an entity; use `!secret`. The brush stores it and shows nothing of it. X Ultra 20 only. |
| `retry_unconfirmed` | bool | `true` | A value the hub keeps on the brush (birthday greeting, cloud host, Wi-Fi) that the brush has not confirmed goes out once per boot; with this on, again once a day while it stays unconfirmed. See **Values kept on the brush**. |
| `wifi_provisioning` | bool | `false` | The hub keeps the brush on the Wi-Fi below over BluFi (see **Values kept on the brush**). The BluFi code is not compiled in unless this is true. With it true the hub needs an SSID (below, or a `wifi:` network), or validation fails. |
| `wifi_ssid` | string | the node's `wifi:` SSID | The network the hub joins the brush to. Needs `wifi_provisioning: true`. Required on a node with no `wifi:` to fall back on (e.g. an Ethernet node). |
| `wifi_password` | string | the node's `wifi:` password | Passphrase for `wifi_ssid`. Needs `wifi_provisioning: true`. Baked into the firmware, not an entity, so it never reaches the recorder; use `!secret`. |

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
entity). "Dev" rows exist only on hubs with `expose_dev_sensors: true`. Rows
naming a model exist only on hubs with that `model:`.

| Key | Default name | Source | Notes |
|---|---|---|---|
| `battery` | Battery | battery characteristic / STATUS | percent, diagnostic |
| `battery_voltage` | Battery voltage | STATUS bytes 3-4 BE | volts from the millivolt reading, diagnostic; a reading outside 2-5 V is not published |
| `last_session_score` | Score | session record byte 33 | 0-100; the no-score sentinel (0xFF) and the score 1 the firmware gives a void session (14 s or less of brushing, or 85% or more of it without motion) read as unknown. On the X Ultra 20 only the cloud record carries it, so it is built only with `cloud_receiver: true` |
| `last_session_duration` | Duration | session record bytes 7-8 BE | seconds |
| `last_session_valid_duration` | Valid duration | session record bytes 9-10 BE | seconds counted as effective |
| `last_session_coverage` | Coverage | derived | valid / duration, percent |
| `gesture_zone_1` .. `gesture_zone_8` | Zone 1 .. Zone 8 | Elite record bytes 23-30; X Ultra 20 gestureArray bytes 20-27 | per-region values, 1-4 upper / 5-8 lower, outer/inner per side. On the X Ultra 20 only the full cloud record carries them (the inline BLE record does not), so they are built only with `cloud_receiver: true`. |
| `quadrant_upper_left`, `quadrant_lower_left`, `quadrant_upper_right`, `quadrant_lower_right` | Quadrant upper left .. Quadrant lower right | session record bytes 19-22 | X Pro Elite only; hidden; percent of the session per quadrant, summing to 100; each is about the sum of its two zones, rounded on the brush |
| `head_used_days` | Brush head used days | settings buffer 27-28 BE | X Pro Elite only; days with brushing since head reset |
| `head_used_times` | Brush head sessions | settings buffer 29-30 BE | X Pro Elite only; valid sessions since head reset |
| `head_used_time` | Brush head used time | settings buffer 14-15 BE | X Pro Elite only; hidden; minutes of valid brushing since head reset |
| `device_theme` | Device theme | settings buffer 0 | X Pro Elite only; hidden; raw index |
| `device_mode` | Device mode | settings buffer 11 | X Ultra 20 only; 1-5 = mode picked on the screen, 6 = voice teaching, otherwise the id of a program written over BLE (0 from the app) |
| `mode_number` | Mode number | settings buffer 5 | X Ultra 20 only; hidden; raw, moved with the device mode so far |
| `running_state` | Running state | `03 14` reply | X Ultra 20 only; hidden; raw value, 3 while charged on the dock |

The last decoded session survives reboots: the newest record is persisted in
NVS per hub and re-published on boot.

### Entities (binary_sensor)

| Key | Default name | Source | Notes |
|---|---|---|---|
| `charging` | Charging | STATUS byte 2 == 0x01 | actively charging on the dock |
| `docked` | Docked | STATUS byte 2 == 0x01 or 0x03 | on the dock, charging or fully charged |
| `connected` | BLE connected | link state | hidden; off almost always by design (the link is up only seconds per poll); use Last seen for freshness |
| `wifi_configured` | Wi-Fi provisioned | `02 34` reply | X Ultra 20 only; off means no SSID is stored, and without one the brush never starts Wi-Fi |
| `user_info_written` | User info written | the brush's `02 11` ack | X Ultra 20 only, built when the hub has `birthday`; hidden; on while the brush has acked the `02 11` frame of the yaml birthday, gender and age as they are now (one write), off from a change of any of them until it acks the new frame |
| `wifi_written` | Wi-Fi written | the brush's BluFi connected report or its first request after the join | X Ultra 20 only, built with `wifi_provisioning: true`; hidden; on while the brush has confirmed the yaml Wi-Fi as it is now; Wi-Fi provisioned shows any stored network, this one ours |
| `cloud_host_written` | Cloud host written | the `Host` header of the brush's request | X Ultra 20 only, built with `cloud_receiver: true`; hidden; on while the brush uploads to this node's current address |
| `area_guidance` | Zone guidance | `03 16` reply | X Ultra 20 only |

The X Pro Elite firmware keeps no setting in settings bytes 3, 4, 8-10 and 13:
constant zero, copies of bytes 0 and 1, a flag nothing writes, and a pause flag
cleared at the start of every session. The keys that read them (`fill_brush`,
`auto_mode`, `volume_enabled`, `calendar_enabled`, `splash_prevent` and the
`volume_index` sensor) are built on no model, nor is `network`: nothing in the
X Ultra 20 firmware writes its byte. Nor is `auto_update`: the X Ultra 20 keeps
the flag of byte 3 (`02 32`), but no update path in its firmware reads it.
`voice_teaching` and `demo_mode` are
switches on the X Ultra 20. A yaml that lists one of these binary sensors fails
validation.

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
| `cloud_host` | Cloud host | `Host` header of the brush's requests | hidden, read-only; X Ultra 20 only, built with `cloud_receiver: true`; the host the brush uploads to, as read from its own requests to the receiver (see **Values kept on the brush**) |

### Entities (switch)

All device-backed switches publish optimistically and are then corrected by the
settings readback; their restore mode is `DISABLED` so nothing is written on
boot. The brush acks every accepted write with `<opcode> 4F 4B` ("OK").

| Key | Default name | Write | Notes |
|---|---|---|---|
| `over_pressure` | Over-pressure alert | `02 12` + 01/00 | X Pro Elite only; readback at settings buffer 22 |
| `raise_wake` | Raise to wake | `02 23` + 01/00 | readback at settings buffer 2 |
| `bluetooth` | Bluetooth | local only | master switch for the BLE link; OFF drops pending writes and tears the link down; `RESTORE_DEFAULT_ON` so a reboot never leaves the brush silently unreachable |
| `area_reminder` | Area reminder | `02 0D` + 01/00 | **dev** on the X Pro Elite, where it has no observable effect. On the X Ultra 20 it is named Voice on zone change: it picks the cue at each 30 s zone change, a short motor stutter when off and a spoken prompt when on (only with voice prompts on); readback at settings buffer 23 |
| `brush_pause` | Brush pause | `02 22` + 01/00 | X Pro Elite only; **dev** |
| `brush_mode` | Brush mode | `02 09` + 01/EC | X Pro Elite only; **dev**; off byte is the 0xEC sentinel, not 0x00 |
| `auto_mode` | Auto mode | `02 25` + 01/00 | X Ultra 20 only; readback at settings buffer 4. On also moves the brush to mode 1 (03:01-12:00) or 2 (the rest of the day) whenever it is idle on the main screen; off does not bring the earlier mode back |
| `festival_reminder` | Holiday reminder | `02 28` + 01/00 | X Ultra 20 only; readback at settings buffer 10 |
| `voice_teaching` | Voice teaching | `02 30` + 01/00 | X Ultra 20 only; readback at settings buffer 6. On selects the firmware's single-step teaching program (gear 16, 180 s), off selects screen mode 5; neither returns to the mode picked on the screen |
| `demo_mode` | Retail display mode | `02 A0` + 01/00 | X Ultra 20 only; readback from the `03 A0` reply. A shop mode in which the brush never sleeps on battery; turning it on during a session ends the session |
| `voice_prompts` | Voice prompts | `02 31` + 4B | X Ultra 20 only; readback at settings buffer 7 |
| `voice_fast_brushing` | Voice on fast brushing | `02 31` + 4B | X Ultra 20 only; the prompt that warns of brushing too fast; readback at settings buffer 8. Takes only while Voice prompts is on, so the hub refuses it otherwise |
| `voice_pressure` | Voice on over-pressure | `02 31` + 4B | X Ultra 20 only; readback at settings buffer 9; same rule as the fast-brushing prompt. The frame carries all three voice flags, so each switch resends the other two as last read |

### Entities (number)

| Key | Default name | Range | Notes |
|---|---|---|---|
| `head_max_minutes` | Brush head time limit | 1-65535 min | X Pro Elite only; minutes of valid brushing on one head before the brush shows its replacement reminder, once; 240 out of the box, about 120 two-minute sessions. Writes `02 17` + 2B BE, readback at settings buffer 25-26; box input (a slider would fire a write per step). Replaces `head_max_days`, which fails validation with a pointer here |
| `custom_step1_gear` .. `custom_step4_gear` | Custom step N gear | 1-41 (1-54 on the X Ultra 20), default 8 | parameters of the runtime Custom program; stored on the node (flash-persisted), written to the brush only when Custom is selected |
| `custom_step1_duration` .. `custom_step4_duration` | Custom step N duration | 5-120 s, step 5, default 30 | changing a parameter while Custom is active re-programs the brush (debounced) |

### Entities (select)

| Key | Default name | Options | Notes |
|---|---|---|---|
| `brush_scheme` | Brushing mode | X Pro Elite: 19 presets + named `custom_modes` + "Custom"; X Ultra 20: "Screen mode 1" .. "Screen mode 5" and "Voice teaching" + named `custom_modes` + "Custom" | writes the full per-step program (`02 06` / `02 0B`); current option read back from settings buffer 11. The X Ultra 20's screen modes and voice teaching only show the brush's state: picking one is refused |
| `device_language` | Display language | 17 languages | writes `02 16` + language id; readback from settings buffer 31. An id past the brush firmware's last language would show English, so it is refused and logged: `OCLEANY3P` stops at 14 (Korean), `OCLEANY3PD` at 13 (Arabic). On the X Ultra 20 it is named Language: the same write also switches the voice prompts |

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

Up to 20 modes, 1-4 steps each, gear 1-41 (1-54 on the X Ultra 20), duration
5-120 s. Modes get ids
121+ in list order (reordering shifts the ids, which only affects how old
session records decode). The runtime "Custom" option (id 120) builds its
program from the custom-step number entities at selection time. Step
boundaries double as the brush's pause signals and summary segments, so a
program wants four steps to keep the four-quadrant guidance.

### Entities (button)

| Key | Default name | Effect | Notes |
|---|---|---|---|
| `reset_head` | Reset brush head | writes `02 0F` | X Pro Elite only; irreversible: zeroes the brush-head usage counters |
| `sync_time` | Sync clock | writes `02 01` + 8 bytes | created only when the hub has `time_id`; writes on press only |
| `poll_now` | Poll now | immediate poll cycle | X Pro Elite only; hidden by default; read-only on the brush |
| `capture_sessions` | Capture sessions | session download + 30 s hold | **dev**, X Pro Elite only (the X Ultra 20 download never streams); keeps the link open so the raw record stream lands in the log |

### Values kept on the brush

On the X Ultra 20 the hub keeps three things on the brush from its yaml, with no
button: the birthday greeting (`birthday`, `gender`, `age`), the cloud host
(with `cloud_receiver: true`, this node's own address) and the Wi-Fi (with
`wifi_provisioning: true`). None of them is an entity, so none reaches the Home
Assistant recorder, and the brush cannot report any of them back over BLE. The
hub therefore keeps a fingerprint of what the brush confirmed in the node's
flash (the brush MAC is part of it) and writes again only when it no longer
matches: another yaml value, another node address, another brush. It writes
them only to a brush that reports an X Ultra 20 family model, so a wrong
`model:` never sends them to an X Pro Elite.

| Value | Written | Confirmed by |
|---|---|---|
| birthday greeting | `02 11 <gender> <age> <month> <day>` | the brush's `02 11 4F 4B` |
| cloud host | `02 33` + `http://<node IPv4>:<web server port>` | the next request the brush sends to the receiver, whose `Host` header is the host the brush has stored |
| Wi-Fi | BluFi join over service `0xFFFF` | the brush's BluFi report that it is connected, or its first request to the receiver after the join was sent (a docked brush stores the network and joins on its next wake); a `02 34` reply of 0 (no Wi-Fi stored, e.g. after a factory reset) makes the hub provision again |

A request counts for a brush by the MAC in its record upload; requests that
carry no MAC count only while one X Ultra 20 hub shares the receiver, so two
brushes never confirm each other. A value the brush has not confirmed goes out
on the first link after boot or after it changes, and again once a day while it
stays unconfirmed (`retry_unconfirmed`, on by default; off: once per boot). A
cloud host is confirmed by the brush's next upload, after a brushing, so it is
written again a day later only if no upload came in between.
`read_only: true` writes none of them. User info written, Wi-Fi written and
Cloud host written (hidden) show in Home Assistant whether the brush confirmed
each current value, as the node's config log does (never the value). The
Oclean app sends its own account's birthday on every connection and the hub
cannot see that, so after the app has been used the hub keeps its stale
confirmation until the yaml value changes.

The brush takes `02 33` with no pairing, so treat it as a redirect, not a
control: block the brush's internet on the router to keep its uploads off the
vendor cloud. The hub points it at this node's own IPv4; a brush that reaches
the node only through another address (a NAT on a router) is not supported.

### In-node session receiver

On X Ultra 20 firmware the brushing score and the full per-session record never
come over BLE; the brush only uploads them to its cloud host. `cloud_receiver:
true` stands in for that cloud: it takes the brush's `UploadBrushRecord`, decodes
the record, and publishes the same session entities a BLE download would (score,
durations, timestamp). A record is routed to the hub whose brush MAC matches the
upload, so several hubs on one node share it.

It does not start its own HTTP server. It registers a handler on the shared
ESPHome web server (the one `web_server` and `captive_portal` use), so a
`web_server:` must be configured and the brush uploads to that server's port
(80 by default). Reusing the one server avoids a second listener competing for
the device's limited sockets.

The hub joins the brush to a network (`wifi_provisioning`) and points its cloud
host at the node by itself (see **Values kept on the brush**); what is left is a
route that lets the brush reach the node (the brush and the node are often on
different VLANs). The transport is HTTP only; the brush accepts a plain
`http://` host, and the node does not serve TLS.

The brush keeps a record until the server answers "ok", then drops it and sends
the next. So the receiver answers "ok" only after a record has been published,
and answers "keep it" the first time it sees one; the brush re-sends it on its
next upload and that copy is acked. A record is thus never dropped before it is
in Home Assistant, at the cost of one extra upload per record. The brush's clock
is answered from the node's clock, so it also corrects over Wi-Fi. The other
requests get a reply that offers nothing: no firmware update and no image for
the date page. The brush asks for that image each time it goes to sleep and
reboots on an empty reply, so the receiver answers it with an empty slot.

### Weather on the brush

The X Ultra 20 has a clock page (swipe right from a mode page) with a weather
icon, a Today/Tomorrow banner and the day's low and high. The brush fetches it
from its cloud host each time it joins Wi-Fi, so with `cloud_receiver` the
node answers that request too. Name a weather entity and the hub does the rest;
Home Assistant needs no template sensors:

```yaml
oclean:
  - id: oclean_x20
    model: x_ultra_20
    cloud_receiver: true
    time_id: ha_time
    weather: weather.forecast_home
```

- The condition and the current temperature come from the entity over the
  native API state subscription, with no setting on the Home Assistant side.
- The daily forecast comes from the `weather.get_forecasts` action. Home
  Assistant performs actions only for a device allowed to: Settings, Devices &
  services, ESPHome, the node, Configure, "Allow the device to perform Home
  Assistant actions". Without it the brush gets the current condition with the
  current temperature as both numbers, and the log says so once.
- Until 18:00 the brush gets today's forecast, from 18:00 tomorrow's (banner
  "Tomorrow"). Temperatures go in the entity's unit, rounded, two digits at most.
- The brush has seven icons, so Home Assistant conditions map onto the closest:

| Home Assistant condition | Brush icon |
|---|---|
| `sunny`, `clear-night` | sun |
| `partlycloudy`, `cloudy`, `fog` | sun behind a cloud |
| `rainy`, `pouring` | rain |
| `lightning`, `lightning-rainy` | thunderstorm |
| `snowy`, `snowy-rainy`, `hail` | snow |
| `windy`, `windy-variant` | wind |
| `exceptional` | dust |

The brush fetches the weather only when it connects, which on battery is after
a brushing, so the page shows what was current then.

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

binary_sensor:
  - platform: oclean
    oclean_id: hub_a
    device_id: dev_brush_a
  - platform: oclean
    oclean_id: hub_b
    device_id: dev_brush_b

text_sensor:
  - platform: oclean
    oclean_id: hub_a
    device_id: dev_brush_a
  - platform: oclean
    oclean_id: hub_b
    device_id: dev_brush_b

switch:
  - platform: oclean
    oclean_id: hub_a
    device_id: dev_brush_a
  - platform: oclean
    oclean_id: hub_b
    device_id: dev_brush_b

number:
  - platform: oclean
    oclean_id: hub_a
    device_id: dev_brush_a
  - platform: oclean
    oclean_id: hub_b
    device_id: dev_brush_b

select:
  - platform: oclean
    oclean_id: hub_a
    device_id: dev_brush_a
  - platform: oclean
    oclean_id: hub_b
    device_id: dev_brush_b

button:
  - platform: oclean
    oclean_id: hub_a
    device_id: dev_brush_a
  - platform: oclean
    oclean_id: hub_b
    device_id: dev_brush_b
```

Boot polls are staggered automatically.

`device_id` decides which HA device an entity belongs to, but it does not make
the entity's **name** unique, and on some transports the name is the identity.
MQTT builds its state topic, discovery topic and `unique_id` from the name
alone, with no device in any of them, so two brushes both exposing `Battery`
publish over each other. The native API is unaffected: it passes `device_id`
next to the key and Home Assistant 2025.8+ tracks entities as
`(device_id, key)`.

Nothing is renamed for you. A second hub without `name_prefix` logs a warning
during validation and leaves the names as they are. Set it per hub to separate
them:

```yaml
oclean:
  - id: hub_a
    ble_client_id: ble_a
    name_prefix: "Brush A"
  - id: hub_b
    ble_client_id: ble_b
    name_prefix: "Brush B"
```

`Battery` on `hub_a` then reads `Brush A Battery`. The prefix goes on before
validation, so unlike a rename in code generation it also settles the
duplicate-name check, and `esphome config` shows the names the firmware
registers.

`name_prefix: ""` keeps the bare names and silences the warning for that hub.
That is a permanent choice, not a workaround, and it is safe if the native API
is all you use.

Two things to know before adding the option to a brush already in use:

- It renames every entity that still carries a default name, so Home Assistant
  sees new entity ids and the history, dashboards and automations built on the
  old ones stop following. Set it on every hub in one edit and flash once.
- Under a sub-device Home Assistant already puts the device name in front of the
  entity name, so a prefix equal to the device name reads twice in the entity
  id. Keep it short, or leave it unset.

Names you write yourself are never touched, in either direction.

### Session history in Home Assistant

Each new session from the brush's ring buffer fires an `esphome.oclean_session`
event (score, duration, valid duration, coverage, scheme, per-zone values,
timestamp). A per-brush watermark stored in NVS prevents re-emitting old
sessions across reboots. A session dated after the brush's own clock, as read
in the same poll, is dropped: it was stamped before the clock was set back.
The timestamp is UTC, converted with the time zone the record was made in, so a
session read after a daylight-saving change still lands at its real hour; a
record without one uses the node's offset at the time of reading.

The events need `homeassistant_services: true` under `api:` (it is off by
default in ESPHome). Without it the firmware still builds and every entity
works; only the events are compiled out, and validation prints a warning saying
so.

Independently of the event, each new session also fires the `on_session`
trigger, so a node can act on a session without Home Assistant in the loop. `x`
is the decoded record (`score`, `duration_s`, `valid_duration_s`, `scheme`,
`zones[8]`, `quadrants[4]`, `tz_index`, the `year`..`second` fields,
`has_score`). Trigger and event both
fire oldest session first, and both run before the session entities are updated,
so read the session from `x` rather than from the entity states:

```yaml
oclean:
  - id: brush
    ble_client_id: brush_ble
    on_session:
      - logger.log:
          format: "brushed %us, score %u"
          args: ["(unsigned) x.duration_s", "(unsigned) x.score"]
```

```yaml
api:
  encryption:
    key: !secret api_encryption_key
  homeassistant_services: true
```

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
  sensor.py                sensor table (per-model sets in __init__.py)
  binary_sensor.py         binary sensor table
  text_sensor.py           text sensor table
  switch.py                command and voice switches + the local bluetooth switch
  number.py                head_max_minutes + 8 custom-program parameters
  select.py                scheme presets + custom modes, language table
  button.py                capture / reset-head / sync-clock / poll-now / cloud host / provision
  text.py                  cloud host (X Ultra 20, opt-in)

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
  oclean_switch.h          OcleanCommandSwitch / OcleanBleSwitch
  oclean_number.h          OcleanHeadMaxNumber / OcleanCustomParamNumber
  oclean_button.h          the button classes
  oclean_select.h          OcleanSchemeSelect / OcleanLanguageSelect
  oclean_text.h            OcleanStoredText
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

### Command set

All commands go to the main write characteristic (`...fbb85`) as Write With
Response, except the session download which goes to `...fbb89`. Accepted
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

### Settings buffer (34 bytes)

The SETTINGS reply is two `03 02` notifies: the start frame (`03 02 23 24` +
16 payload bytes) fills buffer `[0..16)`, the continuation (`03 02` + 18
payload bytes) fills `[16..34)`. `SettingsAssembler` accepts them in either
order. Confirmed offsets:

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
only the first. A brush with Wi-Fi set up uploads its records to the cloud and
drops them from the store instead.

| Offset | Size | Field |
|---|---|---|
| 0-1 | 2 BE | record length |
| 2-7 | 6 | year - 2000 / month / day / hour / min / sec (brush clock) |
| 8 | 1 | mode (the brush's own modes, not the cloud scheme ids) |
| 9-10 | 2 BE | program length (s) |
| 11-12 | 2 BE | brushed time (s) |
| 19 | 1 | time zone index of the brush clock when the session was recorded |
| 28 | 1 | score 0-100 (`0xFF` = none) |

The zone bytes are not mapped, so `model: x_ultra_20` has no zone sensors.
This layout has not yet been checked against a full record from a brush.

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

The X Ultra 20 firmware takes the same frame, ignores the hint byte, runs gears
1-54 and reports the program id in settings buffer 11. It turns auto mode off
on every program write, even one it refuses with `02 06 45 52` during a
session.

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
summary and exit code. `python -m unittest discover -s tests/python` runs the
code generation and table tests; it needs esphome installed.

New functionality comes with tests in the same pull request: protocol,
decoding and command builders in `tests/test_protocol/`, code generation and
the entity tables in `tests/python/`. CI (`.github/workflows/test.yml`) runs ruff +
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

## Issues and pull requests

Report problems at <https://github.com/dzikus/esphome-oclean/issues>. Include
the component version or commit, the ESPHome version, the brush model, the
configuration and the node log. Report security issues privately, see
[SECURITY.md](SECURITY.md).

Pull requests go against `main`:

- Run `pre-commit install` once. The hooks format C++ and Python and check that
  commit messages follow Conventional Commits.
- A pull request that adds functionality adds tests for it, see
  [Testing](#testing).
- CI runs pre-commit, the unit tests, clang-tidy and the ESP32 builds on every
  pull request. All of them must pass before merge.
- User-visible changes get an entry in [CHANGELOG.md](CHANGELOG.md).

## License

GPL-3.0. This component derives from a GPL-3.0 ESPHome component and inherits
that license. See [`LICENSE`](LICENSE).
