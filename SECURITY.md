# Security Policy

## Supported versions

Latest release only.

## Reporting a vulnerability

Use GitHub private vulnerability reporting:
https://github.com/dzikus/esphome-oclean/security/advisories/new

Do not open a public issue.

Include the component version or commit, the ESPHome version, the brush model,
the configuration and the node log.

There is no response-time commitment.

## Scope

The code in this repository: the ESPHome component, the `oclean_stats`
integration and the coverage card.

## Out of scope

- ESPHome, ESP-IDF, Home Assistant and HACS. Report to those projects.
- The brush's BLE protocol. It has no pairing, bonding or authentication. Any
  BLE central in range can read the brush and change its settings without this
  component.
- Access control on the entities. Who can use them is set in Home Assistant.
