# Splatoon Farmers

> [!WARNING]
> This is not a plug-and-play project. Before running the automated routine,
> you must first beat the game, manually farm enough materials and crystals, and
> use them to complete the required initial gear setup. The script assumes that setup
> has already been finished and will not perform it for you.

An unofficial ESP32-S3 wired controller and browser console for material
farming in [Splatoon Raiders](https://www.nintendo.com/us/store/products/splatoon-raiders-switch-2/).
It runs entirely on the microcontroller: open the page, pick a script, hit
start. The board holds the routine in flash and keeps looping even if the
browser or WiFi link drops.

![](./images/banner.png)

Check this video for tutorial: [Bilibili](https://www.bilibili.com/video/BV12P3J6hE4h/)

Required gears described in this video: [Bilibili](https://www.bilibili.com/video/BV1Hp3G6KEfs/)

## What it does

- Emulates a wired Nintendo Switch controller over the ESP32-S3 native USB port.
- Keeps ten board-resident routines in firmware Flash and cycles through them
  on demand. Three are hand-coded (legacy material / apricot / apricot-inkback),
  seven are compiled at build time from user-supplied 文字版代码 scripts.
- Runs every routine entirely on the microcontroller. The browser is only
  used to pick a script and start/stop. Timing is MCU-owned, so serial
  jitter or dropped connections cannot break a sequence halfway through.
- Two transport paths to the board: a direct Web Serial (CH340 / USB-UART)
  link, **or** a WiFi access point / station mode that exposes the same
  console over WebSocket. Either path works for any of the ten routines.
- Manual override: every digital button, D-pad direction, and both analog
  sticks support mouse, touch, and keyboard input. Manual input always wins
  over an in-flight routine — any non-neutral input stops the embedded
  macro and takes over the bus.

## Hardware

The recommended board is an `ESP32-S3-DevKitC-1` (N8 or N16R8 variant) with
separate native USB and USB-UART connectors. The N16R8 variant is what the
board profile in `platformio.ini` is tuned for.

| Link | Board connection | Purpose |
| --- | --- | --- |
| Native USB | GPIO19 D- / GPIO20 D+ | Wired controller to the Switch dock |
| USB-UART | UART0 through the onboard bridge | Serial / Web Serial control from the computer |

Both links can stay connected at the same time. The board can also reach
the browser over WiFi (AP mode `SplatoonFarmers-XXXX` with no credentials,
or STA mode after a one-time provisioning step). See
[ESP32-S3-DevKitC-1 user guide](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s3/esp32-s3-devkitc-1/user_guide_v1.0.html)
for connector placement.

If the board exposes only native USB, connect an external USB-UART adapter:

- GPIO43 / TX0 to adapter RX
- GPIO44 / RX0 to adapter TX
- GND to GND

Do not connect the adapter VCC when the board is already powered from the
Switch. For the strongest protection against host-side reset signals, use only
TX, RX, and GND.

## Build and flash

Install Python 3 and [PlatformIO Core](https://docs.platformio.org/en/latest/core/index.html):

```bash
python3 -m pip install platformio==6.1.19
```

Compile the firmware:

```bash
pio run
```

Flash the firmware and the data partition (the `web/` payload — index.html,
app.js, styles.css, etc. — gets baked into LittleFS and served from the
firmware at runtime):

```bash
pio run -t upload --upload-port /dev/cu.usbserial-XXXX
pio run -t uploadfs --upload-port /dev/cu.usbserial-XXXX
```

Use a port such as `COM5` on Windows or `/dev/ttyUSB0` on Linux. After flashing:

1. Connect native USB to the Nintendo Switch dock.
2. (Optional) Connect USB-UART to the computer for the Web Serial path.
3. Power the board — it boots into WiFi AP or STA mode (see below).

To serve the web UI from your computer while developing (instead of from
the firmware's LittleFS):

```bash
npm run serve
```

Open <http://localhost:4173> in desktop Chrome or Edge. Web Serial requires
a secure context, so opening `web/index.html` directly is not supported.

### WiFi modes

- **AP provisioning** (no NVS credentials, or 5 s BOOT-button hold): the
  board opens `SplatoonFarmers-XXXX` WiFi. The captive portal
  automatically redirects any browser to `http://192.168.4.1/provision`
  where you select a home WiFi and enter its password.
- **STA mode** (credentials saved): the board joins the saved network and
  advertises itself via mDNS as `splatoon.local`. The web console is
  available at `http://splatoon.local/`. Boot banner prints the resolved IP
  on the serial line for environments where mDNS is blocked.

The first connection after a fresh flash goes through AP provisioning.
After saving credentials, the board restarts into STA mode and the
web UI is reachable at `splatoon.local` from any device on the same
LAN.

## Use

1. Open the page. By default the board serves the UI from its own data
   partition at `http://splatoon.local/` (WiFi mode) or
   `http://localhost:4173/` (npm serve).
2. The picker shows every firmware-resident routine, with Chinese label,
   step count, and cycle duration. Pick one.
3. Hit **开始刷取** to start it, **停止** to halt with a neutral report.
4. Manual input (mouse / touch / keyboard) at any time overrides the
   running routine — sending a non-neutral frame stops the macro engine
   and takes over the bus. Losing focus or hiding the tab releases all
   browser-held inputs.

### BOOT button

- **1 to N taps within 3 s of boot** auto-starts the corresponding
  registry entry. With ten routines, taps beyond ten clamp to the last
  one. The LED flashes to confirm the tap count.
- **Hold for 5 s** wipes the saved WiFi credentials and bounces the board
  back into AP provisioning. The LED blinks at 4 Hz to confirm the
  threshold reached.

### Editor

The custom-script editor below the picker has three modes:

- **+ 添加步骤** — pick a button, dpad, or stick direction and insert a
  step. Each press auto-releases (you'll see the step get a `hold` and a
  trailing `release`). `等待延时` lets you pick any millisecond count
  via the inline number input.
- **📋 导入固件脚本** — fetch any firmware-resident routine back into
  the editor for modification. Steps are converted from the firmware's
  `hold`/`release` form into the editor's nested shape. The result
  lives in `localStorage` and runs as the **自定义** chip.
- **JSON 导入 / 导出** — exchange full scripts as files.

## Adding your own scripts

The seven user-compiled scripts in the default repository are sourced from
the user's [文字版代码](#scripts-macros--diretory-layout) `.txt`
collection. The compiler lowers a useful subset of that DSL
(`WAIT`, button presses for N ms, stick direction including
persist-via-DOWN/UP, `$var` assignments, parameter folding, IF/ELIF/ELSE
constant folding, FUNC/CALL inlining, FOR with constant count
unrolled at compile time) to a `MacroStep[]` header that matches the
existing `MaterialFarmMacro.h` layout.

To add a new script:

1. Drop its `.txt` source in `scripts/macros/`.
2. `python3 scripts/compile_macro.py scripts/macros/`
3. `python3 scripts/build_scripts_index.py`
4. `pio run` — the new chip is automatically registered in
   `firmware/include/scripts_index.inc` and the web picker picks it up
   from `/api/scripts` on the next page load.

No firmware C++ change is required. Audio-recognition scripts (which
need a microphone path) are skipped; the firmware has no audio capture.

The `compile_macro.py` skips anything it can't lower, with a printed
warning. The DSL subset it covers and the skip list are documented in
the script's top-of-file docstring.

## Serial protocol

The control link is `115200 baud`, ASCII, one command per line. The same
commands are also exposed over the WebSocket path; just prepend them
to a `ws://splatoon.local/ws` text frame.

| Command | Behavior |
| --- | --- |
| `HELLO` / `INFO` | Return firmware, routine metadata, and current state as JSON |
| `STATUS` | Return phase, step, cycle count, and timing |
| `PING` | Return `PONG` |
| `START` / `START_MATERIAL` / `START_DEFAULT` | Start `material-farm` (registry index 0) |
| `START_APRICOT` / `START2` | Start `apricot-den` (registry index 1) |
| `START_INKBACK` / `START3` | Start `apricot-den-inkback` (registry index 2) |
| `START_IDX <n>` | Start the registry entry at index `n` (0 .. kCompiledScriptCount-1) |
| `STOP` | Stop and send a fully neutral controller report |
| `SCRIPT` | Return the name of the currently-running routine |
| `SCRIPT_LIST` | Return the full registry as a `script_list` JSON frame |
| `STREAM` | Stop both macro engines, prepare to forward raw `R ...` frames |
| `STREAM_END` | Resume normal mode, emit a final neutral frame |
| `R buttons dpad lx ly rx ry` | Send one complete HID report (only when `STREAM` is active) |

The HTTP `GET /api/scripts` endpoint returns the same registry as JSON
(the page uses it on load). `GET /api/scripts/<key>` returns just the
`MacroStep[]` for one entry; the editor's import button uses this to
clone a firmware-resident script into the custom editor.

## Development

```bash
npm test
pio run
```

The test suite covers:

- Embedded step count, duration, action boundaries, and compact Flash size
- Loop-gap boundaries, stop neutralization, and `millis()` wraparound
- Status parsing and the simulated serial transport
- All 14 button bits, cardinal/diagonal D-pad input, keyboard mapping, and
  multi-source press/release behavior
- The 文字版代码 compiler: lexer, parser, IF/ELIF/ELSE/ENDIF constant
  folding, FUNC/CALL inlining, FOR constant-count unrolling, parameter
  folding, and the `scripts_index.inc` builder

Project layout:

- `firmware/include/MaterialFarmMacro.h` — board-resident hand-coded routine
- `firmware/include/ApricotDenMacro.h` — board-resident hand-coded routine
- `firmware/include/ApricotDenInkbackMacro.h` — board-resident hand-coded routine
- `firmware/include/Script_*.h` — auto-generated by `compile_macro.py` from
  `scripts/macros/*.txt`
- `firmware/include/scripts_index.inc` — auto-generated by
  `build_scripts_index.py`; the single registry `main.cpp` iterates
- `firmware/src/MacroEngine.cpp` — non-blocking loop engine
- `firmware/src/main.cpp` — USB HID, serial protocol, and device main loop
- `firmware/src/web_server.cpp` — captive portal, `/api/status`,
  `/api/scripts`, `/api/scripts/<key>`, `/api/wifi`, `/api/reset`
- `web/` — dependency-free browser console (Web Serial + WebSocket)
- `scripts/compile_macro.py` — `.txt` → `Script_<hash>.h` lowerer
- `scripts/build_scripts_index.py` — composes the registry
- `scripts/macros/` — `.txt` sources for the user-compiled scripts
- `tests/` — host-side firmware and browser-logic tests

## License and disclaimer

This project is released under the
[GNU General Public License v3.0](./LICENSE). Third-party attribution is in
[NOTICE.md](./NOTICE.md).

This is an unofficial fan project and is not affiliated with, endorsed by, or
sponsored by Nintendo. Splatoon, Splatoon Raiders, Nintendo Switch, and related
names and marks belong to their respective owners. Use automation responsibly;
the project is intended for offline, single-player material farming.

## Credits

Thanks to [我的茕茕孑立](https://space.bilibili.com/35615481) for the original game controller macro.
