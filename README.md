# Discord Bot for M5StickC PLUS2

## Overview

This project runs a Discord bot directly on an M5StickC PLUS2-class ESP32 device. The firmware connects to WiFi, authenticates to Discord, opens a WebSocket connection to the Discord Gateway, listens for message and interaction events, and performs moderation actions through Discord's REST API.

The device can be configured without recompiling. On first boot it starts a WiFi access point and configuration web server. After setup, it runs as a small Discord moderation appliance with local display feedback and button controls.

## Features

- First-boot WiFi configuration portal over SoftAP.
- Normal WiFi station mode after credentials are saved.
- Discord Gateway v10 WebSocket client.
- Discord REST client with bot-token authentication.
- Slash command registration for a configured guild.
- Slash command handling:
  - `/ping`
  - `/status`
  - `/uptime`
  - `/mod action:<warn|kick|ban|warnings|clearwarn> user:<user> reason:<text>`
- Auto-moderation for:
  - mention spam
  - link spam
  - comma-separated banned keywords
- Local warning counters stored on-device.
- Optional audit/log channel messages.
- ST7789 LCD status display.
- Button input for display/config controls.
- Buzzer feedback for moderation events.
- Optional custom CA certificate through SPIFFS.
- ESP32 PSRAM support and reduced TLS buffer settings.

## Hardware

Target hardware:

- M5StickC PLUS2 or similar ESP32 board with PSRAM.
- 135 x 240 ST7789-compatible LCD using SPI.
- Two buttons.
- Small buzzer.
- 8 MB flash layout.

Current pin map:

| Function | GPIO |
| --- | ---: |
| LCD SCLK | 13 |
| LCD MOSI | 15 |
| LCD DC | 14 |
| LCD CS | 5 |
| LCD RST | 12 |
| LCD backlight | 27 |
| Button A | 37 |
| Button B | 39 |
| Buzzer | 2 |

## Firmware Architecture

Main modules:

| Path | Purpose |
| --- | --- |
| `main/main.c` | Boot sequence, subsystem initialization, FreeRTOS task startup |
| `main/net/wifi.c` | SoftAP setup mode, station mode, credential testing |
| `main/net/ca_store.c` | Optional custom CA loading from SPIFFS |
| `main/config/nvs_config.c` | Saved token, WiFi, guild/channel IDs, thresholds, keywords |
| `main/config/web_server.c` | Browser-based configuration server |
| `main/discord/gateway.c` | Discord Gateway WebSocket, heartbeat, event parsing |
| `main/discord/rest.c` | Discord REST requests, rate-limit handling, interaction ACK path |
| `main/discord/events.c` | Routes Gateway events to moderation/command handlers |
| `main/commands/dispatcher.c` | Slash command registration and command implementations |
| `main/mod/filter.c` | Auto-moderation checks and delete/log actions |
| `main/mod/warnings.c` | Per-user warning counters |
| `main/hardware/display.c` | LCD drawing and hardware task |
| `main/hardware/buttons.c` | Button interrupts and debounce |
| `main/hardware/buzzer.c` | Buzzer output |

Runtime tasks:

- `gateway`: Discord Gateway WebSocket connection and event processing.
- `rest`: queued Discord REST work.
- `cmd_ack`: fast interaction ACK worker with a warmed HTTPS client.
- `commands`: slash command execution after ACK.
- `hardware`: display refresh and button handling.
- `slash_reg`: one-shot slash command registration after Gateway READY.

## Requirements

- ESP-IDF installed and available through ESP-IDF PowerShell or terminal.
- ESP32 target support.
- A Discord application with a bot user.
- A Discord server where you can invite the bot.
- Developer Mode enabled in Discord to copy server/channel IDs.

This repo has been used with ESP-IDF 6.x. The component manifest currently declares `idf >=4.1.0`, but the active SDK configuration and logs are from a newer ESP-IDF toolchain.

## Discord Bot Setup

1. Create an application in the Discord Developer Portal.
2. Add a bot user to the application.
3. Copy the bot token.
4. Enable the Message Content Intent for the bot if you want keyword/link/message-content moderation.
5. Invite the bot to your server with permissions appropriate for the features you use:
   - View Channels
   - Read Message History
   - Send Messages
   - Use Slash Commands
   - Manage Messages
   - Kick Members, only if using `/mod kick`
   - Ban Members, only if using `/mod ban`
6. Copy the following IDs from Discord:
   - Guild/server ID
   - Main moderation channel ID
   - Optional log/audit channel ID

Important: paste the raw Discord bot token into the device configuration page. Do not include the `Bot ` prefix.

## Build and Flash

Open ESP-IDF PowerShell, then run:

```powershell
idf.py set-target esp32
idf.py build
idf.py -p COM5 flash monitor
```

Replace `COM5` with your actual serial port.

To reset all saved configuration and flash clean:

```powershell
idf.py -p COM5 erase-flash
idf.py build
idf.py -p COM5 flash monitor
```

Useful monitor controls:

- `Ctrl+]` exits `idf.py monitor`.
- Press the device reset button to reboot while monitoring.

## First Boot Configuration

If no WiFi credentials are stored, the device starts in setup mode:

- SSID: `DiscordBot-Setup`
- Password: `discordbot`
- URL: `http://192.168.4.1`

Open that URL and enter:

- Raw bot token, without `Bot ` prefix.
- WiFi SSID and password.
- Guild/server ID.
- Main channel ID.
- Optional log channel ID.
- Banned keywords, comma-separated.
- Mention threshold.
- Link threshold.

The firmware tests WiFi credentials before saving. If the WiFi test passes, it saves settings to NVS and restarts into normal bot mode.

## Normal Operation

On boot after configuration:

1. The device connects to the saved WiFi network.
2. Time is synchronized with SNTP.
3. TLS CA data is loaded from SPIFFS if present, otherwise the ESP certificate bundle is used.
4. The Discord REST subsystem starts.
5. The Gateway task fetches `/gateway/bot`.
6. The device connects to `wss://gateway.discord.gg/?v=10&encoding=json`.
7. The bot identifies and waits for READY.
8. Slash commands are registered once per saved configuration.
9. Message and interaction events are processed.

## Button Controls (WIP)

| Control | Action |
| --- | --- |
| Button A short press | Cycle display page |
| Button A long press | Toggle displayed moderation state label |
| Button B short press | Set last-event label to `Status posted` |
| Button B long press | Toggle the configuration web server |

In normal WiFi station mode, the configuration server is available at the device's DHCP IP when toggled on. Watch the serial monitor or your router DHCP list to find the IP address.

## Display

The LCD shows:

- Gateway state.
- Gateway connection color.
- Last measured heartbeat latency.
- Last event text.
- Free heap.
- WiFi RSSI.

Latency is based on Discord Gateway heartbeat ACK timing. It starts at `0ms` after boot because no heartbeat ACK has been received yet. With Discord's heartbeat interval near 41 seconds, the display may show `0ms` for roughly 20 to 41 seconds before the first real latency value appears.

## Auto-Moderation Behavior (WIP)

For each Discord message event:

1. System messages are skipped.
2. The content is checked for:
   - mention count greater than or equal to the configured threshold
   - link count greater than or equal to the configured threshold
   - any configured banned keyword, case-insensitive
3. Violating messages are deleted through Discord REST.
4. The user's local warning count is incremented.
5. A log message is posted to the configured log channel, if one is set.
6. The buzzer beeps. At 3 or more warnings, it also plays a longer tone.

Warnings are stored locally on the device. Erasing flash clears them.

## Slash Commands

The firmware bulk-registers these guild commands:

| Command | Description |
| --- | --- |
| `/ping` | Replies with `Pong!` |
| `/status` | Shows Gateway, latency, heap, and WiFi RSSI |
| `/uptime` | Shows device uptime |
| `/mod warn` | Adds a warning for a user |
| `/mod kick` | Kicks a user |
| `/mod ban` | Bans a user and deletes recent messages |
| `/mod warnings` | Shows stored warning count |
| `/mod clearwarn` | Clears stored warnings |

Discord interactions must be acknowledged within a short time window. This firmware uses a dedicated `cmd_ack` task and a warmed HTTPS client to keep slash-command ACKs fast enough on the ESP32.

## Configuration Storage

Settings are stored in NVS under the `discord` namespace:

- `bot_token`
- `wifi_ssid`
- `wifi_pass`
- `guild_id`
- `channel_id`
- `log_chan_id`
- `keywords`
- `mention_thresh`
- `link_thresh`
- `cmds_reg`
- identify-rate bookkeeping

Saving configuration resets `cmds_reg` so slash commands are re-registered after changes.

## Flash Layout

The custom partition table uses:

| Partition | Offset | Size |
| --- | ---: | ---: |
| `nvs` | `0x9000` | `0x6000` |
| `phy_init` | `0xF000` | `0x1000` |
| `factory` | `0x10000` | `0x6F0000` |
| `spiffs` | `0x700000` | `0xF0000` |

The firmware is configured for 8 MB flash.

## TLS and CA Certificates

By default, the project uses the ESP certificate bundle for Discord TLS.

You can optionally add a custom CA file at:

```text
spiffs_image/ca.pem
```

It will be flashed into SPIFFS and loaded from:

```text
/spiffs/ca.pem
```

The custom CA file must be 8 KB or smaller.

## Troubleshooting

### `idf.py` is not recognized

Open ESP-IDF PowerShell from the Start menu, then run the build commands again.

### Device stays in setup mode

Check that WiFi credentials were saved successfully. If needed, erase flash and configure again:

```powershell
idf.py -p COM5 erase-flash
idf.py -p COM5 flash monitor
```

### Discord REST says `HTTP 401 Unauthorized`

The bot token is wrong or was pasted with the wrong prefix. Paste only the raw token, without `Bot `.

### Gateway reaches READY, but slash commands fail with `Unknown interaction`

The interaction ACK missed Discord's short ACK window. Check the serial log for:

```text
interaction HTTP warmup complete in ...
interaction ACK was slow: ...
interaction ACK failed in ...
```

If ACKs are still over 3 seconds, improve WiFi signal, avoid repeatedly toggling the config server, and test again after warmup has completed.

### REST queue full

Too many REST actions are being generated faster than Discord or the network can accept them. Reduce repeated slash command tests, improve WiFi, or increase queue depth carefully.

### `JSON parse failed (-1)`

This usually means the JSON token buffer is too small for a Discord payload. The project currently sets `JP_MAX_TOKENS` to `512`.

### Display latency shows `0ms`

This is expected until the first Discord heartbeat ACK. It is not the command latency. It is Gateway heartbeat round-trip time.

## Security Notes

- Treat the bot token like a password.
- Do not paste `Bot ` before the token in the config page.
- Revoke and regenerate the token if it appears in logs, screenshots, commits, or chat.
- The setup AP uses a fixed password in firmware. Change `AP_PASS` in `main/net/wifi.c` if the device will be used outside a trusted environment.
- NVS is not encrypted by default in this project.

## Development Notes

- Prefer ESP-IDF PowerShell on Windows so `idf.py` and toolchain paths are configured.
- This project uses ESP-IDF component manager for `esp_websocket_client`.
- SPIFFS contents are built from `spiffs_image/`.
- The REST client uses a warmed, unauthenticated interaction path for Discord interaction callbacks and normal bot-token auth for Discord bot REST calls.
- Main TLS buffers are reduced in `sdkconfig.defaults` to lower memory pressure.
