# Changelog

## Unreleased

- Breaking: the number `head_max_days` is now `head_max_minutes`, "Brush head
  time limit", 1-65535 min. The brush counts the limit in minutes of brushing;
  the old key fails validation with a pointer to the new one. Home Assistant
  gets a new entity for it.
- Breaking: the X Pro Elite no longer builds `fill_brush`, `auto_mode`,
  `volume_enabled`, `calendar_enabled`, `splash_prevent` and `volume_index`:
  their settings bytes hold no setting in the firmware.
- `head_used_time` has the unit `min`. Its long-term statistics were recorded
  without a unit, so Home Assistant may ask to fix them.
- `0202` goes out only after a whole session stream is in, the events are sent
  and the watermark is stored, and never with a record held back as
  implausible; before, it went on a timer in every poll.
- A session the firmware voids (score 1) reads as no score.
- New sensors: battery voltage on every model, and four hidden per-quadrant
  shares on the X Pro Elite.
- The session event takes its UTC offset from the record's own time zone,
  so a session read after a daylight-saving change keeps its real hour.
- A language past the brush firmware's last one (14 on `OCLEANY3P`, 13 on
  `OCLEANY3PD`) is refused instead of turning the display English.
- A write the brush turns down while a session runs is sent once more in the
  next round.
- Record packets that follow a count=0 header during a session log at debug
  level instead of a warning.
- X Ultra 20 support (`model: x_ultra_20`): status, settings, clock, voice
  prompts, auto mode, holiday reminder, language and the other setting writes.
- X Ultra 20: the record its count=0 reply carries, the oldest in its store,
  is a session: one event, the brushed time from the record length, and the
  entities when it is newer than what they show. While the brush is docked the
  hub clears the store with `0202`, so the next session is read next.
- X Ultra 20: brushing-mode select with the screen modes and voice teaching as
  readback and custom programs up to gear 54; voice teaching (`0230`) and the
  retail display mode (`02A0`) are switches instead of binary sensors.
- X Ultra 20: the brush-head counters, head limit, head reset and network flag
  are not built; firmware 0.0.1.6 never fills them. A fast-brushing or
  over-pressure voice flag is refused while voice prompts are off, as the brush
  would drop it.
- X Ultra 20: Wi-Fi over BluFi (service `0xFFFF`, unencrypted variant). With
  `wifi_provisioning: true` the hub keeps the brush on the `wifi_ssid` /
  `wifi_password` hub options (baked into the firmware, falling back to the
  node's own `wifi:`), not entities, so a Wi-Fi password never reaches the
  recorder. The code is not compiled in otherwise.
- X Ultra 20: the birthday greeting, the cloud host and the Wi-Fi come from the
  yaml with no button. None can be read back over BLE, so the hub keeps a
  fingerprint of what the brush confirmed in the node's flash (never an
  entity) and writes only when it no longer matches: the birthday frame
  (`0211`) on the brush's ack, the cloud host (`0233`, this node's address with
  `cloud_receiver: true`) on the `Host` header of the brush's next request, the
  Wi-Fi on the brush's BluFi connected report or its first request after the
  join. A value still unconfirmed is sent again once a day
  (`retry_unconfirmed`, on by default). A hidden, read-only `cloud_host` text
  sensor shows the host the brush really uploads to, and hidden
  `birthday_written`, `gender_written` and `age_written` binary sensors
  whether the frame the brush acked last carried each current yaml value.
- X Ultra 20: an X Ultra 20 hub option fails validation on an `x_pro_elite`
  hub, and the birthday frame, the cloud host and the Wi-Fi go only to a brush
  that reports an X Ultra 20 family model.
- X Ultra 20: `cloud_receiver: true` takes the brush's cloud session uploads
  on the node's web server and publishes them as the session entities: score,
  durations, timestamp and, from the full record, the eight `gesture_zone`
  values. BLE never carries the score on this firmware. An upload goes to the
  hub whose brush MAC it names; a `web_server:` is required.
- X Ultra 20: the receiver answers the brush's clock with the node's local
  time, so the brush clock also corrects over Wi-Fi.
- X Ultra 20: `cloud_drop_future` (on by default) acks an upload dated
  implausibly far in the future, from a brush whose clock was not corrected
  yet, so the brush drops it instead of re-sending it on every connect. Such a
  record is never published.
- X Ultra 20: `weather: <weather entity>` answers the brush's weather request
  through the session receiver, so its clock page shows the Home Assistant
  condition, Today/Tomorrow and the day's low and high. The forecast needs the
  node allowed to perform Home Assistant actions; without that the brush gets
  the current condition and temperature.
- X Ultra 20: a row whose only source is a hub option is built only with it:
  the score and the eight zones (cloud record only) and `cloud_host` with
  `cloud_receiver: true`, `birthday_written` with `birthday`, `gender_written`
  and `age_written` with `birthday` and their own option. Not built: the
  capture and poll buttons (BLE never hands over the stored sessions), the
  `auto_update` binary sensor and the `over_pressure` switch (nothing in
  firmware 0.0.1.6 reads either flag; the pressure prompt is
  `voice_pressure`).
- X Ultra 20: birthday greeting. The `birthday` hub option is the greeting date
  (`0211`), sent with the `gender` and `age` options; on every wake that day the
  brush shows its birthday screen. All three are yaml options, not entities, so
  none of them reaches the recorder; use `!secret`.
- The ESPHome floor is now 2026.6.0: the hub options that hold a secret
  (`wifi_password`, `birthday`, `gender`, `age`) are marked sensitive with
  `cv.sensitive` (2026.6.0), the session receiver registers on the web server
  outside its login, which the brush could not answer (2026.3.0), and the
  cloud host takes the node's own IP from the network API of 2026.2.0.
- The X Pro 20 (`OCLEANX20`) and the first X Ultra (`OCLEANV1*`) use the X
  Ultra 20 reply formats instead of TYPE1.

## v1.4.1 (2026-09-13)

- No change to the component.
- CI: tool pins read from the requirements file, per-job permissions, workflow
  audit and dependency review on pull requests, CodeQL and Scorecard.
- Dependency updates.

## v1.4.0 (2026-08-31)

- Breaking: `name_prefix` is opt-in. Default entity names are no longer
  prefixed from the hub id.
- The component clears every clang-tidy finding and follows the ESPHome naming
  rules.
- CI: clang-format, shellcheck and actionlint hooks; component warnings fail
  the build; a strict host warning pass; builds on four boards; host tests
  built as gnu++20.

## v1.3.0 (2026-08-14)

- The control platforms are built only when they are declared.
- A stored session from another layout is rejected. A failed poll is reported
  as a warning.
- Entity name prefixes are resolved during validation.
- The poll tick and ring ingest decisions moved into the protocol layer, with
  tests.
- Documentation of the session event requirement; the read-only claim is
  corrected.

## v1.2.0 (2026-08-09)

- One entity-defaults helper shared by the platforms.
- The two-brush example shows every platform.

## v1.1.0 (2026-08-09)

- Default entity names are prefixed when a node has more than one brush.

## v1.0.0 (2026-08-01)

- First release.
