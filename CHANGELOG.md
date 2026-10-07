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
- X Ultra 20: an opt-in `cloud_host` text with Apply and Clear buttons sets the
  server the brush uploads to (`0233`), or clears it back to the firmware
  default. The firmware has no read-back, so the text shows the last value
  written, not the brush's own.
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
