# Security

Apps run in RAM under permissions declared in `app.json` and enforced by the OS (`sd`, `wifi`, `ble`,
`audio_out`, `audio_in`, `imu`, `ir`, `gpio`). The OS can update itself from this repository's releases.

If you find a way to bypass permissions, escape the app sandbox, or tamper with updates, please report it
privately through GitHub's **Security > Report a vulnerability** on this repository rather than a public issue.
Releases are not signed yet (it is on the roadmap), so install apps and firmware only from this repository.
