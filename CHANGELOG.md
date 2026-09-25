# Changelog

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
