# Splatoon-Farmers 项目接手手册

> 下一个 agent 请从头读这份文件。它是会话之间的事实/上下文交接，比 git log 更全。

## 1. 项目是什么

- **官方仓库**: <https://github.com/ShikyC/Splatoon-Farmers>（GPL-3.0，第三方项目，与 Nintendo 无关）
- **目的**: 在 ESP32-S3 上模拟有线 Switch 手柄，自动跑 Splatoon Raiders 的材料农场宏
- **核心能力**:
  - ESP32-S3 原生 USB（**GPIO19 D- / GPIO20 D+**）→ Switch 底座手柄口
  - USB-UART（CH340）→ 电脑，运行 WebUI 控制台
  - 固件里 48 步、**63.595 秒**完整循环，由 MCU 自主计时（serial jitter 不会中断）
  - 串口协议 `HELLO` / `INFO` / `STATUS` / `START` / `STOP` / `PING` / `R buttons dpad lx ly rx ry`（115200 baud，ASCII，LF 终止）
  - 浏览器 WebUI：Web Serial API，无依赖

## 2. 硬件现状（2026-08-07 会话结束时确认）

- **板子**: ESP32-S3-N16R8（双 Type-C口，16MB Flash，8MB Octal PSRAM）
- **板子接法**:
  - **CH340 USB-UART**（您已接电脑）→ 烧录 + 串口控制
  - **原生 USB Type-C**（另一口）→ **当前未接**，需要插 Switch 底座手柄口
- **串口端口**（macOS）: `/dev/cu.wchusbserial5B910032911`
- **Switch 状态**: 用户已确认 Splatoon Raiders **已通关 + 初始装备已完成**（这是项目前提条件）
- **摄像头**: 当前 `esp32` 标签指向 FaceTime（自拍），**没有任何摄像头对着开发板**——不要尝试用 read-camera skill 拍板子，会拍到用户的脸

## 3. 当前已完成的步骤

1. ✅ 工具链已就绪（PlatformIO 6.1.19、Node v24.12.0、Python 3.10.1、pyserial 3.5）
2. ✅ 仓库 clone 到 `/Users/zeroko/Documents/project/EmbeddedProject/Splatoon-Farmers/`
3. ✅ `platformio.ini` 已针对 N16R8 改过（**见第 4 节**）
4. ✅ 固件编译成功（7.02s，345KB，Flash 11.0%，RAM 9.6%）
5. ✅ 烧录成功（8.50s，ESP32-S3 v0.2，MAC `68:ee:8f:4f:f1:70`）
6. ✅ 串口握手成功（HELLO 收到 `SplatoonFarmers/1.0.0` JSON 状态，48 步 / 63595ms 周期 / idle）

**板子现在跑的就是这个固件，待命中（state: idle）。**

## 4. platformio.ini 的关键改动（**重要**）

原项目 `platformio.ini` 假定 DevKitC-1-N8（无 PSRAM）。**我们改了 3 行**适配 N16R8（Octal PSRAM + 16MB Flash）：

```ini
[env:esp32-s3-devkitc-1]
platform = espressif32@6.10.0      ; ← README 写的 6.1.19 是笔误
board = esp32-s3-devkitc-1          ; ← board ID 仍用这个，PlatformIO 兼容 N16R8
framework = arduino
monitor_speed = 115200
board_build.partitions = huge_app.csv        ; ← 新增：3MB app 分区
board_build.arduino.memory_type = qio_opi    ; ← 新增：Octal PSRAM
board_build.extra_flags =
  -DARDUINO_ESP32S3_DEV
  -DARDUINO_USB_MODE=0
  -DARDUINO_RUNNING_CORE=1
  -DARDUINO_EVENT_RUNNING_CORE=1
  -DBOARD_HAS_PSRAM                          ; ← 新增：启用 PSRAM
lib_deps =
  https://github.com/esp32beans/switch_ESP32.git#0adba99d9c2b32c86aed21cb74558cc35841530e
build_unflags =
  -std=gnu++11
build_flags =
  -std=gnu++17
  -DARDUINO_USB_CDC_ON_BOOT=0
  -DATT_CONTROL_SERIAL=Serial
```

**如需重做或 rollback**：3 行改动就够。其他都是项目原样。

## 5. 踩过的坑（必读）

### 5.1 PlatformIO Home Server 卡死

**症状**: `pio run` 进程 CPU 0%、state S（sleeping）、5+ 分钟不动、无子进程、stdout 0 字节。

**根因**: VSCode PlatformIO 扩展启动的 home server 进程（PID 28270，session 3f1b21f6…）**卡死 3 天**（CPU 时间 3:01 但不响应新请求），锁住 PlatformIO 的 IPC。

**修复**: `kill -9 28270`（以及同时间戳的孤儿 `94578 94560 72194`）。**杀 home server 是安全的**——VSCode 扩展会在下次需要时自动重启。**但每次 session 切换最好先 `ps -ef | grep platformio` 检查**。

### 5.2 串口调试的 USBHID 报错是**正常**的

`[E][USBHID.cpp:346] SendReport(): not ready` 在原生 USB 未插 Switch 时**会持续刷屏**。这是 TinyUSB 找不到 USB HOST 的正常反应，**不是 bug**。一旦原生 USB Type-C 接到 Switch，立刻消失。

### 5.3 路径陷阱

- **PATH 内联**：用户 zshrc **没有被改**（自动模式拒绝写持久化）。每次 `pio` 命令前要内联：
  ```bash
  PATH="$HOME/.platformio/penv/bin:$PATH" pio ...
  ```
- **不能把 `| tail` 套在 `pio run` 上**：tail 等 EOF，会让后台任务看起来一直"没动"。要么直接跑、要么加 `tee` 写文件。

### 5.4 README 版本号笔误

README 写 `python3 -m pip install platformio==6.1.19`，但 `platformio.ini` 实际用 `platform = espressif32@6.10.0`（平台版本，非 Core 版本）。Core 6.1.19 是真实装的版本。

## 6. 复现命令（任何一步可独立重跑）

```bash
# 编译
cd /Users/zeroko/Documents/project/EmbeddedProject/Splatoon-Farmers
PATH="$HOME/.platformio/penv/bin:$PATH" pio run

# 烧录
PATH="$HOME/.platformio/penv/bin:$PATH" pio run -t upload --upload-port /dev/cu.wchusbserial5B910032911

# 验证（不打开 WebUI，直接发串口命令）
python3 -c "
import serial, time
s = serial.Serial('/dev/cu.wchusbserial5B910032911', 115200, timeout=2)
time.sleep(0.3); s.reset_input_buffer()
s.write(b'HELLO\n'); time.sleep(0.5)
print(s.read(2048).decode('utf-8', errors='replace'))
s.close()
"

# 启动 WebUI
cd /Users/zeroko/Documents/project/EmbeddedProject/Splatoon-Farmers
npm run serve          # 然后浏览器打开 http://localhost:4173
```

## 7. 用户最后停留在的状态

- 板子已烧录，固件在跑，**等待用户把原生 USB Type-C 插到 Switch 底座手柄口**
- WebUI **尚未启动**（用户没让我跑 `npm run serve`）
- 用户**没问**接下来是否要改 firmware、是否要连 Switch、是否要加额外功能
- 用户**明确说**："将知识点整理给我,我给另一个 agent 接手" → 现在是 handoff 时机

## 8. 关键文件位置

| 文件 | 用途 |
|---|---|
| `/Users/zeroko/Documents/project/EmbeddedProject/Splatoon-Farmers/platformio.ini` | **已改**，见第 4 节 |
| `/Users/zeroko/Documents/project/EmbeddedProject/Splatoon-Farmers/firmware/src/main.cpp` | USB HID + 串口协议 + 主循环 |
| `/Users/zeroko/Documents/project/EmbeddedProject/Splatoon-Farmers/firmware/include/MaterialFarmMacro.h` | 48 步宏定义（要改步序/时长改这个） |
| `/Users/zeroko/Documents/project/EmbeddedProject/Splatoon-Farmers/firmware/src/MacroEngine.cpp` | 非阻塞 loop 引擎 |
| `/Users/zeroko/Documents/project/EmbeddedProject/Splatoon-Farmers/web/` | 浏览器控制台（无依赖，直接 `index.html` 配 Web Serial） |
| `/Users/zeroko/Documents/project/EmbeddedProject/Splatoon-Farmers/tests/` | 主机端固件测试（`npm test`） |
| `/Users/zeroko/Documents/project/EmbeddedProject/Splatoon-Farmers/.pio/build/esp32-s3-devkitc-1/firmware.bin` | 当前烧录进去的固件（345440 字节） |
| `/Users/zeroko/Documents/project/EmbeddedProject/Splatoon-Farmers/README.md` | 官方使用说明，必读 |
| `/Users/zeroko/Documents/project/EmbeddedProject/Splatoon-Farmers/LICENSE` | GPL-3.0 |

## 9. 用户偏好（从会话观察）

- **中文沟通**（用户全程用中文，包括"先检查是否符合条件"）
- **直接给结论**，不绕弯（用户在每轮都明确表态、给出关键信息）
- **会主动检查前置条件**（问 1-6 点来核对状态）
- **不接受"未明确同意"的持久化修改**（自动模式拒绝改 `~/.zshrc` 时用户没反驳——尊重这个边界）
- **摄像头辅助有效**（用户提"利用摄像头 skill 拍"，但板子没在镜头前——下次如果要让 agent 看硬件，先调整摄像头位置）
- **不在 IDE selection 里**（这是 CLI session，没有 IDE 选区）

## 10. 接手后建议的第一步

按可能性从高到低：

1. **问用户下一步要做什么**（最可能）：
   - 跑 `npm run serve` 启 WebUI？
   - 改 firmware（步序/时长）？
   - 加新功能？
   - debug 某个具体问题？
2. 如果用户要继续接 Switch：**提醒他插上原生 USB Type-C**，然后启 WebUI
3. 如果用户要做 firmware 改动：先读 `MaterialFarmMacro.h` 和 `MacroEngine.cpp`，改完 `pio run` + `pio run -t upload`

## 11. 关联的 memory 索引

- [[esp32s3-splatoon-farmers-board]] — N16R8 板子 + CH340 端口的事实
- [[platformio-home-server-stuck]] — PlatformIO home server 卡死现象 + 修复
- [[user-zh-cli-preferences]] — 用户的语言/边界/沟通偏好

## 12. 自定义脚本编辑器（Web 端 v1）

主控制卡右上角的 picker 第四档「自定义」展开一个编辑器卡片，只在该 chip 被选中时显示。

### 数据形态

每条步骤形如：

```json
{ "type": "hold",    "buttons": 4, "dpad": 15, "sticks": [128,128,128,128], "durationMs": 500 }
{ "type": "release", "buttons": 0, "dpad": 15, "sticks": [128,128,128,128], "durationMs": 50 }
{ "type": "delay",   "durationMs": 1000 }
```

`hold` 持续 `durationMs` 期间按住按钮/方向；`release` 期间发 neutral 帧；`delay` 仅消耗时间、不发帧。

### 协议路径（运行自定义脚本时）

1. WebUI 点「开始刷取」（custom chip）→ `ScriptRunner.play()` 异步发送 `STREAM`
2. Firmware 收到 `STREAM` 停所有 macro engine，进入 stream 模式（[main.cpp](firmware/src/main.cpp) `streamMode = true`）
3. 浏览器侧 `requestAnimationFrame` 节奏按步骤时长推进；每个 hold/release 帧通过 `R buttons dpad lx ly rx ry` 发给 firmware
4. firmware 在 stream 模式下直接转发 R 帧给 HID，不调 `stopAllMacros()`（避免 per-frame 浪费）
5. 脚本结束或用户点「停止」→ 浏览器发 `STREAM_END` + neutral，firmware 清 streamMode

录制时走的是同一条 R 帧通道，但由 `ManualInputState.onRecordEvent` 回调转成步骤。

### 录制方法

- **键盘**：IJKL=XYBA、方向键=方向键、Q/E=L/R、1/3=ZL/ZR、Z/X=L3/R3、C/H=Capture/Home
- **WebUI 按钮**：直接点 A/B/X/Y/ZR 等按钮同样触发
- **触发逻辑**：按下一个键 → 自动追加 `hold` 步骤；松开 → 自动追加 `release` 步骤；连续两次按下间隔 > 50ms → 中间夹 `delay` 步骤
- **副作用**：录制期间清空按钮、循环开关、导入/导出按钮全部禁用；REC 按钮红色脉冲

### 持久化

- **localStorage**：key = `splatoon-farmers.customScripts.v1`，任何步骤变化后 500ms debounce 自动保存
- **导出 JSON**：工具栏「导出 JSON」按钮 → 下载 `<filename>.json`（文件名从脚本名 sanitize 而来）
- **导入 JSON**：工具栏「导入 JSON」按钮 → 选文件 → 替换当前脚本（解析失败显示 error）

### 数据格式参考

完整 JSON 形态（serializeScript 输出）：

```json
{
  "name": "weird / name? with*chars",
  "repeat": false,
  "steps": [
    { "type": "hold",    "buttons": 8, "dpad": 0, "sticks": [128,128,128,128], "durationMs": 500 },
    { "type": "release", "buttons": 0, "dpad": 15, "sticks": [128,128,128,128], "durationMs": 50 }
  ]
}
```

### 关键文件

| 文件 | 用途 |
|---|---|
| [`web/editor.js`](web/editor.js) | Script 类 + ScriptRecorder + ScriptRunner + 持久化 helpers |
| [`tests/web/editor.test.mjs`](tests/web/editor.test.mjs) | 35 个 Node 单测（Script、Recorder、Runner、持久化） |
| [`web/manual-input.js`](web/manual-input.js) | `ManualInputState` ctor 增加 `onRecordEvent` 第二参数 |
| [`web/app.js`](web/app.js) | 集成编辑器卡、picker 第 4 chip、startButton/stopButton 分支 |
| [`web/index.html`](web/index.html) | 编辑器卡 DOM 骨架 |
| [`web/styles.css`](web/styles.css) | 编辑器 / picker / REC 动画样式 |

### v1 已知限制

- 摇杆固定 128 128 128 128（无摇杆输入源）
- 不支持拖拽排序（用 ↑↓ 按钮重排）
- 多脚本并存未实现（编辑器一次只编辑一个）
- 物理手柄录制未实现（Web Gamepad API 不在 v1 范围）
- 脚本烧录到 firmware 永久存储未实现（接口预留）
