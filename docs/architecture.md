# Architecture (as built)

This replaces the phased plan in `research.md`: the decisions below are what the code does today.

## What changed from the original plan, and why

| Plan | Now | Reason |
|---|---|---|
| ELF in SRAM **and** XIP from a flash slot | ELF in RAM only | Simplest thing that can work on the hardware; Flipper does the same. XIP stays an option (see below). |
| Intents, back stack, suspend/resume, pub/sub | Not implemented | First deliverable is "install, list, launch apps from SD". The ABI has reserved room to add them without breaking apps. |
| Boot-to-firmware for heavy apps, coexist with M5Launcher | Dropped | Decision from the author: no M5Launcher, no OTA hand-off. |
| `espressif/elf_loader` as a dependency | Own loader (`loader.c`, ~250 lines) | The component registry is not reachable from CI sandboxes we used, and the loader only needs one relocation type because apps never import symbols. |
| LVGL | Own canvas engine (`gfx.c`) | Apps already draw into 16-bit sprites with M5GFX calls; keeping that API is what lets them run unchanged. |
| Apps in a separate repo | Apps and OS in one repo | One place to version and release both; the store reads this repo's tags. |

## Memory and loading

* App **code** is copied into executable RAM (`heap_caps_malloc(.., MALLOC_CAP_EXEC)`, the instruction-bus alias of
  the shared SRAM) using only aligned 32-bit stores; **data/bss** go to normal internal RAM.
  `CONFIG_ESP_SYSTEM_MEMPROT_FEATURE` is off for this.
* Apps are linked at fixed addresses (`sdk/app.ld`: code at `0x10000000`, data at `0x20000000`) with
  `--emit-relocs`. The loader adds the load offset to every `R_XTENSA_32` site; nothing else needs fixing because
  apps do not import symbols. They receive a pointer to the ABI table in their entry point.
* Everything an app allocates through the ABI is tracked per app (heap blocks, `FILE*`/`DIR*`, sockets, canvases,
  PWM pins, NVS handles, audio voices) and released when the app ends, however it ends.
* Exit: **Fn + `** sets a flag; the next call into the OS (`delay`, keyboard, draw, ...) `longjmp`s back to the
  runner. If an app never calls the OS for 2 s the system restarts. A crash restarts the system and the launcher
  reports which app crashed.

## Hardware ownership

| Service | Implementation | Permission |
|---|---|---|
| Display ST7789 135x240 (landscape 240x135) | `cpos_hal/hal_display.c`, esp_lcd over SPI2, DMA bands | always |
| Keyboard TCA8418 | `hal_input.c`, polled on I2C, key map 4x14, Fn+` = leave app | always |
| microSD (SPI3, `/sdcard`) | `hal_sd.c`; apps only see `/sdcard/...` | `sd` |
| Battery (ADC GPIO10) | `hal_power.c` | always |
| WiFi + sockets | `cpos_core/net.c`; state survives app changes | `wifi` |
| IMU BMI270 (I2C 0x69) | `cpos_core/imu.c` | `imu` |
| Speaker / microphone ES8311 (I2S) | `cpos_core/audio.c`, 8-voice mixer; one of the two open at a time | `audio_out`, `audio_in` |
| IR LED (GPIO44), Grove/EXT pins, PWM | `cpos_core/gpio_svc.c` | `ir`, `gpio` |
| Preferences | `api.c`, NVS namespace per app, hashed keys | always |
| BLE | not yet (`ble` permission is reserved) | `ble` |

## ABI

`sdk/include/cpos/abi.h` is shared by the OS and the SDK. `cpos_api_t` holds system calls, then one sub-table per
service, then the libc/libm table generated from `sdk/libc_exports.def`. Rules:

* Functions are only ever appended; each sub-table ends with `reserved[]` slots so a minor release can add
  functions without moving anything.
* An app records the ABI major it was built for and the minimum minor it needs (`cpos_app_header_t`, in the
  `.cpos_app` section). The OS refuses other majors and newer minors, at install time and at launch.
* Apps cannot import anything that is not in the table: a missing function is a link error at build time.

## Store and updates

`cpos_ui/store.c`: `GET api.github.com/repos/<repo>/tags`, keep the highest `<name>-vX.Y.Z` per name, download
`releases/download/<tag>/<tag>.elf` to `/sdcard/apps`, show the app's permissions before installing, keep only the
newest version on the card. The pseudo-app `os` (`os-vX.Y.Z`, asset `.bin`) updates the firmware itself over the
air using the two OTA slots. The repository is configurable in System.

## Partition table (8 MB)

`nvs` 24 KB, `otadata`, `phy_init`, `ota_0` 3 MB, `ota_1` 3 MB, ~1.9 MB unused. The OS image is ~1.1 MB.

## What is next

1. BLE service (NimBLE) and a thread/mutex service, which unlock `shell-rc`, `ble-sentinel` and `mp3-player`.
2. XIP from a flash slot for large apps (map the app's code with `esp_mmu_map` at a fixed window).
3. Intents + back stack + saved state, using the reserved ABI slots.
