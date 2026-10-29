# Intents

An **intent** is a small named job an app offers to the rest of the system: `light.color`,
`compass.tilt`, `tv.off_all`. The OS can start the app, hand it the arguments, and get a value back.
Intents are the building block for shortcuts (chains of intents, conditions, triggers). Chaining and
activation are not built yet; this document covers declaring, running and trying single intents.

* An app declares its intents in `app.json`; the build embeds the declaration in the ELF.
* The OS starts the app **without its UI** (`setup()`/`loop()` do not run), calls the app's
  `on_intent()`, and gets a status and a value.
* One app runs at a time, so intents run one after another.

## Declaring intents

```json
{
  "name": "wiz-rc",
  "perms": ["wifi"],
  "intents": {
    "light.color": {
      "title": "Color RGB",
      "args": { "r": "int:0-255", "g": "int:0-255", "b": "int:0-255", "ip": "text:15" },
      "returns": "ok",
      "timeout_ms": 15000
    },
    "light.find": { "title": "Buscar lámparas", "returns": "list:12", "timeout_ms": 15000 }
  }
}
```

| Field | Meaning |
|---|---|
| name (the key) | `group.action`, lower case letters, digits and `_` (`light.color`) |
| `title` | text shown when picking an intent (required) |
| `args` | argument names and types; every argument is optional on the wire, the handler decides which ones it needs |
| `returns` | what the intent answers with (default `ok`) |
| `timeout_ms` | the OS stops the app and reports `timeout` after this long (default 2000) |
| `launch` | `true` for launch intents (see below) |
| `needs` | informational: permissions the intent relies on |

The build tool (`sdk/tools/build_app.py`) validates the block and embeds it as compact JSON in the
`.cpos_intents` section (max 3000 bytes); an app with intents needs ABI minor 4.

### Types

| Type | Written as | Notes |
|---|---|---|
| bool | `bool` | `true` / `false` |
| integer | `int` or `int:min-max` | `int:0-255`, `int:-50-50`. The range is documentation and a hint for the UI; handlers must check it |
| text | `text` or `text:N` | at most N characters; no `;` in the value |
| list | `list:N` | at most N items, each short text; items never contain `\|` |
| record | `{"field": "type", ...}` (only for `returns`) | several named values, e.g. `{"note": "text:4", "cents": "int:-50-50"}` |
| ok | `ok` | no value, only the status |

## Wire format

* **Arguments**: one text, `key=value;key=value` (at most 96 bytes). Values may contain `=` but not `;`.
* **Value** (up to 160 bytes): a scalar as plain text (`42`, `true`, `La`), a record as `note=La;octave=4;cents=-3`,
  a list as `192.168.1.5|192.168.1.7`.
* **Status**: `0 ok`, `1 no_app`, `2 no_perm`, `3 timeout`, `4 bad_arg`, `5 failed`.
  Typical uses: `bad_arg` for a missing or out-of-range argument, `failed` when the job could not be done
  (no WiFi, robot not found, nothing heard), `no_app` for an intent the app does not know.

## Writing a handler

Include `<cpos_intent.h>` and implement `on_intent`:

```cpp
#include <cpos_intent.h>

void on_intent(const Intent &in, IntentResult &out)
{
    if (in.is("light.color")) {
        int r = in.i("r", -1), g = in.i("g", -1), b = in.i("b", -1);
        if (r < 0 || g < 0 || b < 0 || r > 255 || g > 255 || b > 255) { out.error(CPOS_INTENT_BAD_ARG); return; }
        setColor(r, g, b);
        out.ok();
    } else if (in.is("light.find")) {
        for (auto &l : discover()) out.item(l.ip.toString().c_str());
        out.done();
    } else {
        out.error(CPOS_INTENT_NO_APP);
    }
}
```

| `Intent` | |
|---|---|
| `in.is("name")` | is this the intent being asked for |
| `in.i("key", def)` | integer argument, `def` when absent |
| `in.get("key", buf, n)` | copy a text argument; false when absent |
| `in.has("key")` | is the argument present |

| `IntentResult` | |
|---|---|
| `out.ok()` / `ok(int)` / `ok(bool)` / `ok("text")` | success, with an optional scalar value |
| `out.field("k", v)` … `out.done()` | build a record |
| `out.item("x")` … `out.done()` | build a list |
| `out.error(CPOS_INTENT_*)` | failure |
| `out.launch()` | launch intent (below) |

Rules for handlers:

* The app starts **without** `setup()`: initialise only what the job needs (`M5Cardputer.begin(cfg, false)`,
  read your `Preferences`, bring up WiFi or BLE if needed). Do not draw anything.
* Finish within the declared `timeout_ms`; the OS stops the app at its next call into the OS when time is up.
* **Anything that moves hardware must stop on its own and have a short upper limit** (the robot and car
  intents cap `ms` at 5000). An intent never leaves a device running.
* Disconnect cleanly (BLE robots refuse the next connection otherwise).

### Launch intents

A job that needs the app to *stay* running (music playing) is a **launch intent**: declare `"launch": true`,
call `out.launch()`, and the app replies and then carries on as a normal app. `setup()` runs and can read
the request with `startIntent()` to start the job. The intent timeout is cancelled once the app replied.
Example: `music.play_playlist` opens the player and starts the playlist.

## Where it lives

| Piece | File |
|---|---|
| ABI table `intent` (minor 4): `request()` / `reply()` | `sdk/include/cpos/abi.h` |
| OS runtime: `cpos_run_intent()` | `os/components/cpos_core/intent.c` |
| Declarations read from the ELF: `cpos_read_intents()` | `os/components/cpos_core/loader.c` |
| App side, header only | `sdk/shim/include/cpos_intent.h`, entry in `sdk/shim/src/crt.cpp` |
| Declaration check and embedding | `sdk/tools/build_app.py` |
| Manual runner and the `os.*` intents | `os/components/cpos_ui/intents_ui.c` |

## Trying them

* On the device: **System > Intents** lists the `os.*` intents and every intent of the installed apps.
  Pick one, edit the arguments (`r=;g=;b=` is prefilled from the declaration) and press Enter: the result
  (`ok: 75`, `bad_arg`, `failed`, …) is shown. Launch intents keep the app open until you leave it with Fn + `.
* Host test of the app-side helpers: `g++ -std=c++17 -Isdk/include -Isdk/shim/include sdk/tests/intent_test.cpp -o /tmp/t && /tmp/t`
* Emulator: the self test runs a set of intents and logs `INTENT name(args) -> status value`.

## System intents (answered by the OS)

| Intent | Arguments | Returns |
|---|---|---|
| `os.battery` | — | int 0-100 |
| `os.wifi` | — | bool, connected |
| `os.time` | — | text `HH:MM` (local time; synced over WiFi) |
| `os.notify` | `text` | ok, shows a message for a moment |
| `os.open` | `app` | ok (not wired yet) |

## Catalog

This section is generated from the `app.json` files by `tools/gen_intents_doc.py`; do not edit it by hand.

<!-- catalog:begin -->

### ble-sentinel

| Intent | What it does | Arguments | Returns |
|---|---|---|---|
| `sentinel.scan` | Escanear y contar por categoría | `seconds`: int:2-30 | record {drone: int, glasses: int, tracker: int, headset: int, other: int, unknown: int} |
| `sentinel.count` | Contar una categoría (drone, glasses, tracker, headset, other, unknown, any) | `category`: text:7, `seconds`: int:2-30 | int |

### compass

| Intent | What it does | Arguments | Returns |
|---|---|---|---|
| `compass.tilt` | Inclinación (pitch y roll) | — | record {pitch: int:-90-90, roll: int:-180-180} |
| `compass.flat` | ¿Está apoyado plano? | — | bool |

### ekoparty-badge

| Intent | What it does | Arguments | Returns |
|---|---|---|---|
| `badge.get_url` | URL del QR | — | text:90 |
| `badge.set_url` | Cambiar la URL del QR | `url`: text:90 | ok |

### karting

| Intent | What it does | Arguments | Returns |
|---|---|---|---|
| `kart.last_session` | Última sesión guardada | — | record {session: int, laps: int, best_ms: int, last_ms: int} |
| `kart.best_ever` | Mejor vuelta de todas las sesiones (ms) | — | int |

### led-rc

| Intent | What it does | Arguments | Returns |
|---|---|---|---|
| `strip.on` | Encender la tira | — | ok |
| `strip.off` | Apagar la tira | — | ok |
| `strip.color` | Color RGB (el más parecido del mando) | `r`: int:0-255, `g`: int:0-255, `b`: int:0-255 | text:20 |
| `strip.brightness` | Brillo | `dir`: text:4, `steps`: int:1-10 | ok |
| `strip.mode` | Modo (flash, strobe, fade, smooth) | `name`: text:6 | ok |

### mp3-player

| Intent | What it does | Arguments | Returns |
|---|---|---|---|
| `music.play_playlist` (launch) | Abrir y reproducir una playlist | `name`: text:40 | ok |
| `music.shuffle_all` (launch) | Abrir y reproducir todo en aleatorio | — | ok |
| `music.playlists` | Listar playlists | — | list:16 |
| `music.volume` | Volumen (desde la próxima vez) | `level`: int:0-100 | ok |
| `music.shuffle` | Aleatorio on/off (desde la próxima vez) | `state`: text:3 | ok |

### robot-control

| Intent | What it does | Arguments | Returns |
|---|---|---|---|
| `robot.scan` | Buscar robots BLE | `seconds`: int:2-15 | list:16 |
| `robot.color` | Color de un Sphero | `name`: text:12, `r`: int:0-255, `g`: int:0-255, `b`: int:0-255 | ok |
| `robot.drive` | Mover un Sphero un rato (fwd, right, back, left) | `name`: text:12, `dir`: text:5, `ms`: int:100-5000, `speed`: int:10-100 | ok |
| `robot.sound` | Sonido del R2-D2 / R2-Q5 | `name`: text:12 | ok |

### shell-rc

| Intent | What it does | Arguments | Returns |
|---|---|---|---|
| `car.drive` | Manejar un rato (fwd, back, left, right) | `dir`: text:5, `ms`: int:100-5000, `speed`: int:1-4, `lights`: text:3 | ok |
| `car.turbo` | Turbo hacia adelante (gen. 2025) | `ms`: int:100-5000, `lights`: text:3 | ok |
| `car.donut` | Donut (gen. 2025) | `ms`: int:100-5000, `lights`: text:3 | ok |
| `car.battery` | Batería del auto (gen. 2025) | — | int:0-100 |

### snake

| Intent | What it does | Arguments | Returns |
|---|---|---|---|
| `snake.high_score` | Récord | — | int |
| `snake.sound` | Sonido (on/off) | `state`: text:3 | ok |

### tetrix

| Intent | What it does | Arguments | Returns |
|---|---|---|---|
| `tetrix.high_score` | Récord | — | int |
| `tetrix.sound` | Sonido (on/off) | `state`: text:3 | ok |

### tuner

| Intent | What it does | Arguments | Returns |
|---|---|---|---|
| `tuner.listen` | Escuchar una nota | `seconds`: int:1-10 | record {note: text:4, octave: int:0-9, cents: int:-50-50} |
| `tuner.set` | Elegir afinación y La4 | `tuning`: int:0-9, `a4`: int:430-450 | ok |

### tv-b-gone

| Intent | What it does | Arguments | Returns |
|---|---|---|---|
| `tv.off_all` | Apagar televisores (todos los códigos) | `region`: text:4 | int |
| `tv.region` | Elegir región (na, eu, both) | `region`: text:4 | ok |

### wiz-rc

| Intent | What it does | Arguments | Returns |
|---|---|---|---|
| `light.on` | Encender las luces | `ip`: text:15 | ok |
| `light.off` | Apagar las luces | `ip`: text:15 | ok |
| `light.brightness` | Brillo | `level`: int:10-100, `ip`: text:15 | ok |
| `light.color` | Color RGB | `r`: int:0-255, `g`: int:0-255, `b`: int:0-255, `ip`: text:15 | ok |
| `light.temp` | Blanco (kelvin) | `kelvin`: int:2200-6500, `ip`: text:15 | ok |
| `light.scene` | Escena | `name`: text:24, `id`: int:1-32, `ip`: text:15 | ok |
| `light.find` | Buscar lámparas | — | list:12 |

<!-- catalog:end -->

### What these intents cannot do (yet)

* **Control something that is already running.** `music.pause`, `music.next`, `kart.start_session` or
  `robot.takeoff` need an app that keeps running between intents, and the OS runs one app at a time. Today an
  intent is a short job, or a launch intent that opens the app. Background apps would change this.
* **Fly the drones.** Flight needs a continuous control stream, so there are no `robot.takeoff`/`robot.land`
  intents; the Jumping Sumo is also left out because using it means leaving the saved WiFi network.
* **Compass heading.** The Cardputer has no magnetometer, so `compass` offers tilt and flatness, not a heading.
* **Read live state of other apps** (kart session in progress, now playing).

## Open questions (next steps)

* **Chaining:** how a shortcut is written and stored (a file on the SD, edited on the device or on a computer),
  how a result feeds the next intent's arguments, and how conditions look (`if os.battery < 20 …`).
* **Activation:** how a shortcut starts (from the launcher, a key combination, a timer, an event such as
  "WiFi connected" or "BLE device seen").
* **Permissions:** the first time a shortcut touches an app it should ask, as the store does at install time.
* **Duplicate names:** two apps declaring the same intent (two light apps): the shortcut picks one.
