# OrangeBus I-Bus SPP Protocol Specification

> **Purpose:** This document specifies the exact Bluetooth SPP communication protocol used by the OrangeBus Android app to control I-Bus features on the ESP32. It is intended for cross-verification with the ESP32 firmware implementation to ensure both sides agree on command formats, response formats, and connection behavior.

---

## 1. Bluetooth SPP Connection

### 1.1 Pairing

| Property | Value |
|---|---|
| Device name | `BMW-BlueBus` (or `BMW_BlueBus_SPP`) |
| Pairing PIN | `1234` or `0000` (standard, may not be prompted) |
| Protocol | RFCOMM / SPP (Serial Port Profile) |

### 1.2 Socket Connection

| Property | Value |
|---|---|
| SPP UUID | `00001101-0000-1000-8000-00805F9B34FB` (standard SPP) |
| Thread requirement | Must connect on a background thread (not UI/main) |
| Coexistence | SPP shares the same ACL link with A2DP/HFP/AVRCP — music and calls continue while SPP commands are sent |

> **Note for ESP32 verification:** The Android app uses the standard SPP UUID. Ensure the ESP32 SPP server is registered under this UUID. The integration guide contains a typo (`00001101-0000-10000-8000-...` with 5 zeros in the third group) — this is not a valid UUID and the app does **not** use it.

### 1.3 Connection Lifecycle

```
Android                                          ESP32
  |                                                 |
  |--- connect() via RFCOMM socket ---------------->|
  |<-- connection established ----------------------|
  |--- (no handshake required) -------------------->|
  |                                                 |
  |  [app sends IBUS? to query initial state]       |
  |--- "IBUS?\r\n" -------------------------------->|
  |<-- "IBUS:DEBUG=...,UI=...,...\r\n" ------------|
  |                                                 |
  |  [normal operation: commands & responses]       |
  |                                                 |
  |--- disconnect() / socket.close() ------------->|
```

- **No handshake is required** after socket connect — the app immediately sends `IBUS?` to query initial state.
- The ESP32 should accept commands as soon as the SPP socket is open.

---

## 2. Protocol Format

### 2.1 Framing

| Property | Value |
|---|---|
| Encoding | ASCII text |
| Line terminator | `\r\n` (CRLF) |
| Direction | Full-duplex: app sends commands, ESP32 sends responses |
| Message boundary | Each complete line (up to `\r\n`) is one message |

### 2.2 Command Format

```
<PREFIX>:<SUBCOMMAND>[:<PARAM>[:<PARAM>...]]\r\n
```

All I-Bus commands use the prefix `IBUS:`. EQ commands use the prefix `EQ:`.

### 2.3 Response Format

```
<STATUS>[:<DETAIL>]\r\n
```

- Success responses start with `OK:`
- Error responses start with `ERR`
- Status query response starts with `IBUS:DEBUG=`

### 2.4 Buffering

The ESP32 may fragment SPP data across multiple reads. The Android app buffers incoming bytes and splits on `\r\n` boundaries. The ESP32 must ensure every response ends with `\r\n` and must not send partial lines without terminators.

---

## 3. I-Bus Commands — Complete Reference

### 3.1 Debug Mode

Controls whether the ESP32 logs I-Bus traffic without transmitting. When debug is ON, the ESP32 receives and logs I-Bus messages but does **not** transmit on the I-Bus.

| Command | Response | Description |
|---|---|---|
| `IBUS:DEBUG:ON\r\n` | `OK:DEBUG_ON\r\n` | Enable debug mode (log only, no TX) |
| `IBUS:DEBUG:OFF\r\n` | `OK:DEBUG_OFF\r\n` | Disable debug mode (normal TX resumes) |

### 3.2 Telephone Emulation

Controls the BMW telephone module emulation. The ESP32 pretends to be a phone connected to the car's I-Bus telephone interface.

| Command | Response | Description |
|---|---|---|
| `IBUS:TEL:CONNECT\r\n` | `OK:TEL_CONN\r\n` | Mark TEL as connected on the I-Bus |
| `IBUS:TEL:DISCONNECT\r\n` | `OK:TEL_DISC\r\n` | Mark TEL as disconnected on the I-Bus |
| `IBUS:TEL:CALL:ACTIVE\r\n` | `OK:CALL_ACTIVE\r\n` | Signal active call to the radio/MID |
| `IBUS:TEL:CALL:END\r\n` | `OK:CALL_END\r\n` | Signal call ended to the radio/MID |
| `IBUS:TEL:CALL:INCOMING:<number>\r\n` | `OK:CALL_INCOMING\r\n` | Incoming call with caller ID number |
| `IBUS:TEL:CALLER:<number>\r\n` | `OK:CALLER_ID\r\n` | Set/update caller ID display on radio/MID |

> **Verification point:** The `<number>` parameter in `IBUS:TEL:CALL:INCOMING` and `IBUS:TEL:CALLER` is a free-form string (phone number or label). The ESP32 should pass it to the I-Bus telephone display. The Android app does not format it — it sends the raw number string.

### 3.3 CD Changer Emulation

Controls the CD changer emulation. When "playing", the ESP32 tells the radio a CDC is active, allowing audio from the A2DP source to play through the car speakers.

| Command | Response | Description |
|---|---|---|
| `IBUS:CDC:PLAY\r\n` | `OK:CDC_PLAY\r\n` | Start CDC playback emulation |
| `IBUS:CDC:STOP\r\n` | `OK:CDC_STOP\r\n` | Stop CDC playback emulation |

### 3.4 Configuration — UI Mode

Selects which BMW radio/display type the ESP32 should format I-Bus messages for.

| Command | Response | Description |
|---|---|---|
| `IBUS:CONFIG:UI:<mode>\r\n` | `OK:UI=<name>\r\n` | Set UI mode |

**UI Mode Codes:**

| Code | Name | Description |
|---|---|---|
| `1` | `CD53` | Business CD radio (text via CD changer display) |
| `2` | `BMBT` | On-board monitor (full text display) |
| `3` | `MID` | Multi-Information Display |
| `5` | `MIR` | Mirror interface |

Example:
```
→ IBUS:CONFIG:UI:2\r\n
← OK:UI=BMBT\r\n
```

> **Verification point:** The response includes the mode **name** (string), not the numeric code. The Android app maps the name back to its enum.

### 3.5 Configuration — Autoplay

When enabled, the ESP32 automatically starts CDC playback when the phone connects via A2DP.

| Command | Response | Description |
|---|---|---|
| `IBUS:CONFIG:AUTOPLAY:0\r\n` | `OK:AUTOPLAY\r\n` | Disable autoplay |
| `IBUS:CONFIG:AUTOPLAY:1\r\n` | `OK:AUTOPLAY\r\n` | Enable autoplay |

### 3.6 Configuration — Metadata Mode

Controls how metadata (artist/title) is displayed on the radio.

| Command | Response | Description |
|---|---|---|
| `IBUS:CONFIG:META:<mode>\r\n` | `OK:META\r\n` | Set metadata display mode |

The Android app sends an integer `0`–`3`. The meaning of each mode is defined by the ESP32 firmware.

### 3.7 Configuration — Comfort Features

All comfort settings are persisted in NVS and survive power cycles.

| Command | Response | Description |
|---|---|---|
| `IBUS:CONFIG:COMFORT:BLINK:0\r\n` | `OK:BLINK\r\n` | Disable blink confirmation |
| `IBUS:CONFIG:COMFORT:BLINK:1\r\n` | `OK:BLINK\r\n` | Enable blink confirmation (lights flash on lock/unlock) |
| `IBUS:CONFIG:COMFORT:LOCKS:0\r\n` | `OK:LOCKS\r\n` | Disable auto-lock |
| `IBUS:CONFIG:COMFORT:LOCKS:1\r\n` | `OK:LOCKS\r\n` | Enable auto-lock (lock at >15 km/h, unlock on key removal) |
| `IBUS:CONFIG:COMFORT:MIRRORS:0\r\n` | `OK:MIRRORS\r\n` | Disable mirror fold |
| `IBUS:CONFIG:COMFORT:MIRRORS:1\r\n` | `OK:MIRRORS\r\n` | Enable mirror fold on lock (requires motorized mirrors) |

### 3.8 Raw Packet

Sends a raw I-Bus packet directly onto the bus. For advanced users and debugging only.

| Command | Response | Description |
|---|---|---|
| `IBUS:SEND:<hex bytes>\r\n` | `OK:SENT\r\n` | Send raw I-Bus packet |

**Hex byte format:**
- Space-separated hex byte pairs
- Minimum 4 bytes (8 hex characters): source, length, destination, command
- Total byte count must be even (each byte is 2 hex chars)
- Uppercase hex preferred but not required

Example:
```
→ IBUS:SEND:68 05 18 39 00 02 50\r\n
← OK:SENT\r\n
```

> **Verification point:** The Android app normalizes all hex input before sending: it strips all whitespace, validates length (minimum 8 chars, even count), then re-inserts spaces between each byte pair. So whether the user types `68051839000250` or `68 05 18 39 00 02 50`, the app always sends `IBUS:SEND:68 05 18 39 00 02 50`. The ESP32 must accept space-separated hex bytes after the `IBUS:SEND:` prefix.

### 3.9 Status Query

Requests the current state of all I-Bus configuration from the ESP32.

| Command | Response |
|---|---|
| `IBUS?\r\n` | `IBUS:DEBUG=<ON\|OFF>,UI=<mode>,AUTOPLAY=<0\|1>,META=<mode>,BLINK=<0\|1>,LOCKS=<0\|1>,MIRRORS=<0\|1>\r\n` |

Example:
```
→ IBUS?\r\n
← IBUS:DEBUG=OFF,UI=CD53,AUTOPLAY=1,META=0,BLINK=1,LOCKS=0,MIRRORS=1\r\n
```

**Response field mapping:**

| Field | Values | Description |
|---|---|---|
| `DEBUG` | `ON` / `OFF` | Debug mode state |
| `UI` | `CD53` / `BMBT` / `MID` / `MIR` | Current UI mode (by name, not code) |
| `AUTOPLAY` | `0` / `1` | Autoplay on A2DP connect |
| `META` | Integer (e.g. `0`, `1`, `2`, `3`) | Metadata display mode |
| `BLINK` | `0` / `1` | Blink confirmation on lock/unlock |
| `LOCKS` | `0` / `1` | Auto-lock at speed |
| `MIRRORS` | `0` / `1` | Mirror fold on lock |

> **Verification point:** The `UI` field in the status response uses the **name** (e.g. `CD53`), not the numeric code (e.g. `1`). The Android app maps names back to its `UiMode` enum. The ESP32 must send the name, not the code.

---

## 4. Error Responses

The ESP32 should send these error responses when applicable:

| Response | Meaning |
|---|---|
| `ERR\r\n` | General error |
| `ERR:UNKNOWN\r\n` | Unknown command prefix (not `IBUS:` or `EQ:`) |
| `ERR:UNKNOWN_IBUS\r\n` | Unknown IBUS sub-command |
| `ERR:FORMAT\r\n` | Malformed `IBUS:SEND` packet (bad hex, wrong length, etc.) |

The Android app handles any response starting with `ERR` as an error and displays it in the status message.

---

## 5. EQ Commands (Shared SPP Connection)

EQ commands share the same SPP socket. These are included here for completeness — the ESP32 must handle both `IBUS:` and `EQ:` prefixed commands on the same connection.

| Command | Response | Description |
|---|---|---|
| `EQ:ON\r\n` | `OK\r\n` | Enable parametric EQ |
| `EQ:OFF\r\n` | `OK\r\n` | Disable parametric EQ |
| `EQ:<i>:<freq>:<q>:<gain>\r\n` | `OK\r\n` | Set band i (0–4) parameters |
| `EQ:SAVE:<name>\r\n` | `OK\r\n` | Save preset to NVS |
| `EQ:LOAD:<name>\r\n` | `OK\r\n` | Load preset from NVS |
| `EQ?\r\n` | Band data (see below) | Query all bands |

**EQ band query response format:**
```
<idx>:<freq>:<q>:<gain>;<idx>:<freq>:<q>:<gain>;... (5 bands, semicolon-separated)\r\n
```

Example:
```
→ EQ?\r\n
← 0:60:1.0:+3.5;1:250:1.0:0.0;2:1000:1.0:-2.0;3:4000:1.0:0.0;4:12000:1.0:+1.5\r\n
```

> **Important:** The Android app's `EqController` ignores responses starting with `IBUS:`, `OK:DEBUG`, `OK:TEL`, `OK:CALL`, `OK:CDC`, `OK:UI=`, `OK:AUTOPLAY`, `OK:META`, `OK:BLINK`, `OK:LOCKS`, `OK:MIRRORS`, and `OK:SENT`. Similarly, the `IbusController` only processes responses starting with `IBUS:` or `OK:`. There is no central response router — each controller independently filters by prefix. This means **every response must be processable by prefix matching alone**.

---

## 6. Android App Architecture — Signal Flow

### 6.1 SPP Manager (Single Socket, Two Consumers)

```
┌─────────────┐
│  SppManager  │  ← owns the single BluetoothSocket
│  (one instance) │
└──────┬───────┘
       │
       ├── legacyListener ──────── EqController (EQ commands)
       │
       └── listeners[0] ────────── IbusController (IBUS commands)
```

- **One `SppManager` instance** owns the Bluetooth socket, created by `EqController`.
- `EqController` uses `setListener()` (legacy single listener).
- `IbusController` uses `addListener()` (multi-listener).
- Both controllers receive **all** responses and filter by prefix.

### 6.2 IbusController Attachment

The `IbusController` does **not** own a `SppManager`. It attaches to the `EqController`'s instance:

```
EqController.sppManager ──→ ibusController.attachToSppManager(sppManager)
```

**Attachment timing:**
1. When the user navigates to the Device Control screen, `IbusController.attachToSppManager()` is called.
2. If the SPP socket is already connected, `IbusController` immediately sends `IBUS?` to query current state.
3. The listener stays attached for the entire lifetime of the Device Control screen (not just the I-Bus Config screen).
4. The listener is detached only when the user navigates back from Device Control (via the back button).

### 6.3 Command Sending Path

```
User taps toggle
  → IbusController.setComfortBlink(true)
    → sendCommand("IBUS:CONFIG:COMFORT:BLINK:1")
      → checks _sppManager != null and isConnected()
      → viewModelScope.launch(Dispatchers.IO)
        → SppManager.sendCommand("IBUS:CONFIG:COMFORT:BLINK:1")
          → checks outputStream != null
          → scope.launch (Dispatchers.IO)
            → outputStream.write("IBUS:CONFIG:COMFORT:BLINK:1\r\n".toByteArray())
            → outputStream.flush()
```

**Key properties:**
- Commands are sent on IO threads (never UI thread).
- `outputStream` is `@Volatile` — safe cross-thread read.
- If `SppManager` is not attached or not connected, the command is dropped and an error is logged.
- If `outputStream` is null (socket closed), the command is dropped and `onError` is notified.

### 6.4 Response Handling Path

```
SppManager reader (IO coroutine)
  → inputStream.read(buffer)
  → buffer accumulated until \r\n found
  → notifyResponse(line)
    → EqController.listener.onResponse(line)
      → if starts with IBUS: or OK:DEBUG/TEL/CALL/CDC/UI/AUTOPLAY/META/BLINK/LOCKS/MIRRORS/SENT → skip
      → else → handleResponse() (EQ processing)
    → IbusController.listener.onResponse(line)
      → handleResponse() (IBUS processing)
```

### 6.5 State Management

The `IbusController` uses **optimistic local state + ESP32 confirmation**:

1. When user toggles a setting, the local `StateFlow` is updated immediately (optimistic).
2. When the ESP32 responds with `OK:...`, the response is parsed and state is updated from the confirmed value.
3. When `IBUS?` response arrives, all state flows are updated from the ESP32's authoritative values.

**Example — comfort blink toggle:**

```
User toggles blink ON
  → _blink.value = true               (optimistic)
  → sendCommand("IBUS:CONFIG:COMFORT:BLINK:1")
  ← OK:BLINK\r\n                       (ESP32 confirms — but no value in response)
     → _blink stays true               (no contradictory data)
```

**Example — status query:**

```
→ IBUS?\r\n
← IBUS:DEBUG=OFF,UI=CD53,AUTOPLAY=1,META=0,BLINK=1,LOCKS=0,MIRRORS=1\r\n
  → _debugOn = false
  → _uiMode = CD53
  → _autoplay = true
  → _metaMode = 0
  → _blink = true
  → _locks = false
  → _mirrors = false
```

---

## 7. Verification Checklist for ESP32 Firmware

Use this checklist to verify the ESP32 firmware matches the Android app's expectations:

### 7.1 Connection

- [ ] SPP server registered under UUID `00001101-0000-1000-8000-00805F9B34FB`
- [ ] Device advertises as `BMW-BlueBus` or `BMW_BlueBus_SPP`
- [ ] SPP socket accepts connection without requiring a handshake
- [ ] Multiple listeners can receive responses simultaneously (Android sends commands from two controllers on the same socket)

### 7.2 Command Parsing

- [ ] All commands are terminated with `\r\n` (CRLF)
- [ ] `IBUS:` prefix is case-sensitive (uppercase)
- [ ] Sub-commands after `IBUS:` are case-sensitive (uppercase)
- [ ] Numeric parameters (UI mode code, autoplay 0/1, meta mode, comfort 0/1) are sent as integers without padding
- [ ] Phone number in `IBUS:TEL:CALL:INCOMING:<number>` and `IBUS:TEL:CALLER:<number>` is a free-form string (may contain `+`, digits, spaces — the Android app sends it as-is)

### 7.3 Response Format

- [ ] All responses are terminated with `\r\n` (CRLF)
- [ ] Success responses start with `OK:`
- [ ] Error responses start with `ERR`
- [ ] Status query response format: `IBUS:DEBUG=<ON|OFF>,UI=<name>,AUTOPLAY=<0|1>,META=<mode>,BLINK=<0|1>,LOCKS=<0|1>,MIRRORS=<0|1>\r\n`
- [ ] `UI` field in status response uses the **name** (`CD53`, `BMBT`, `MID`, `MIR`), not the numeric code
- [ ] `OK:UI=<name>\r\n` response uses the name, not the code
- [ ] Raw packet success response is `OK:SENT\r\n`

### 7.4 Raw Packet Handling

- [ ] `IBUS:SEND:` prefix followed by space-separated hex byte pairs
- [ ] Example: `IBUS:SEND:68 05 18 39 00 02 50\r\n`
- [ ] Minimum 4 bytes (8 hex characters after removing spaces)
- [ ] Invalid format → `ERR:FORMAT\r\n`

### 7.5 Comfort Feature Persistence

- [ ] All comfort settings (BLINK, LOCKS, MIRRORS) are persisted in NVS
- [ ] Settings survive power cycles
- [ ] `IBUS?` query returns the persisted values

### 7.6 Response Completeness

- [ ] Every command produces exactly one response
- [ ] No command should produce zero responses (the Android app waits for confirmation)
- [ ] Commands that succeed should respond with the specific `OK:...` format listed above, not a bare `OK`

### 7.7 Command-Response Mapping Summary

| Android Sends | ESP32 Must Respond |
|---|---|
| `IBUS:DEBUG:ON\r\n` | `OK:DEBUG_ON\r\n` |
| `IBUS:DEBUG:OFF\r\n` | `OK:DEBUG_OFF\r\n` |
| `IBUS:TEL:CONNECT\r\n` | `OK:TEL_CONN\r\n` |
| `IBUS:TEL:DISCONNECT\r\n` | `OK:TEL_DISC\r\n` |
| `IBUS:TEL:CALL:ACTIVE\r\n` | `OK:CALL_ACTIVE\r\n` |
| `IBUS:TEL:CALL:END\r\n` | `OK:CALL_END\r\n` |
| `IBUS:TEL:CALL:INCOMING:<num>\r\n` | `OK:CALL_INCOMING\r\n` |
| `IBUS:TEL:CALLER:<num>\r\n` | `OK:CALLER_ID\r\n` |
| `IBUS:CDC:PLAY\r\n` | `OK:CDC_PLAY\r\n` |
| `IBUS:CDC:STOP\r\n` | `OK:CDC_STOP\r\n` |
| `IBUS:CONFIG:UI:1\r\n` | `OK:UI=CD53\r\n` |
| `IBUS:CONFIG:UI:2\r\n` | `OK:UI=BMBT\r\n` |
| `IBUS:CONFIG:UI:3\r\n` | `OK:UI=MID\r\n` |
| `IBUS:CONFIG:UI:5\r\n` | `OK:UI=MIR\r\n` |
| `IBUS:CONFIG:AUTOPLAY:0\r\n` | `OK:AUTOPLAY\r\n` |
| `IBUS:CONFIG:AUTOPLAY:1\r\n` | `OK:AUTOPLAY\r\n` |
| `IBUS:CONFIG:META:<n>\r\n` | `OK:META\r\n` |
| `IBUS:CONFIG:COMFORT:BLINK:0\r\n` | `OK:BLINK\r\n` |
| `IBUS:CONFIG:COMFORT:BLINK:1\r\n` | `OK:BLINK\r\n` |
| `IBUS:CONFIG:COMFORT:LOCKS:0\r\n` | `OK:LOCKS\r\n` |
| `IBUS:CONFIG:COMFORT:LOCKS:1\r\n` | `OK:LOCKS\r\n` |
| `IBUS:CONFIG:COMFORT:MIRRORS:0\r\n` | `OK:MIRRORS\r\n` |
| `IBUS:CONFIG:COMFORT:MIRRORS:1\r\n` | `OK:MIRRORS\r\n` |
| `IBUS:SEND:<hex>\r\n` | `OK:SENT\r\n` or `ERR:FORMAT\r\n` |
| `IBUS?\r\n` | `IBUS:DEBUG=...,UI=...,AUTOPLAY=...,META=...,BLINK=...,LOCKS=...,MIRRORS=...\r\n` |
| (unknown `IBUS:...`) | `ERR:UNKNOWN_IBUS\r\n` |
| (unknown prefix) | `ERR:UNKNOWN\r\n` |

---

## 8. Known Discrepancies / Open Questions

1. **UUID typo in integration guide:** `ANDROID_INTEGRATION_GUIDE.md` lists `00001101-0000-10000-8000-00805F9B34FB` (5 zeros in third group). This is not a valid UUID. The app uses the correct standard SPP UUID `00001101-0000-1000-8000-00805F9B34FB`. **The ESP32 must use the standard UUID.**

2. **OK response value inclusion:** The Android app parses `OK:AUTOPLAY:`, `OK:META:`, `OK:BLINK:`, `OK:LOCKS:`, `OK:MIRRORS:` with a trailing value (e.g. `OK:BLINK:1`). If the ESP32 only sends `OK:BLINK` without the value, the app will not update state from the confirmation (it falls back to the optimistic local value). **Recommendation: ESP32 should echo the value in OK responses** (e.g. `OK:BLINK:1\r\n` instead of `OK:BLINK\r\n`) for more robust state synchronization.

3. **Raw packet hex format in guide:** `ANDROID_INTEGRATION_GUIDE.md` example code uses `joinToString("")` (no spaces) for `IBUS:SEND:`, while the command table specifies space-separated bytes. The Android app normalizes to space-separated. **The ESP32 must accept space-separated hex bytes.**

4. **Autoplay response:** The Android app expects `OK:AUTOPLAY\r\n` (no value) per the guide, but also parses `OK:AUTOPLAY:<0|1>` if present. The ESP32 can send either format. Sending the value is preferred for state verification.

5. **Metadata mode range:** The Android app sends integer values `0`–`3` via a slider. The ESP32 defines what each mode means. If the ESP32 supports a different range, the app slider may send out-of-range values. **The ESP32 should clamp or reject invalid modes with `ERR`.**
