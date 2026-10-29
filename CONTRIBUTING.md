# Contributing

Thanks for helping. CardputerOS is small on purpose: an OS that owns the hardware, and apps that stay
plain Arduino/M5Cardputer code.

## Quick start

```bash
# firmware (ESP-IDF 5.4.x)
cd os && idf.py set-target esp32s3 && idf.py build

# one app -> dist/<name>-vX.Y.Z.elf
python3 sdk/tools/build_app.py apps/snake
```

Protocol cores that do not touch the OS (for example `apps/robot-control/src/*_core.h`) have host tests:
`g++ -std=c++17 apps/robot-control/test/sphero2_test.cpp -o /tmp/t && /tmp/t`.

## Adding or changing an app

1. `apps/<name>/app.json` (`name`, `version`, `perms`, `stack`) and `apps/<name>/src/`.
2. Keep the source Arduino/M5Cardputer compatible; call the hardware through the SDK headers only.
3. Bump `version` and add a line to the app's `CHANGELOG.md`. A push to `master` publishes any version
   that has no release yet (tag `<name>-vX.Y.Z`).

## Changing the OS or the ABI

* `sdk/include/cpos/abi.h` is the single source of truth. Only **append** to tables (every table has
  reserved slots) and bump `CPOS_ABI_MINOR`; removing or changing meaning needs a major bump.
* Bump `os/version.txt` when the OS changes, so the store offers the update.
* The emulator build (`CONFIG_CPOS_EMULATOR`, QEMU) is the fastest way to check UI and loader changes.

## Pull requests

Describe what changed and how you tested it (emulator, host test, or real hardware, and which
device). Say clearly what you could not test. Small, focused PRs are easier to review.

## Reporting bugs

Use the issue templates. Include the OS version (System screen), the app and its version, and, if you
have one, the serial log.
