# BlueBus ESP32 Implementation Plans

## Part 1: Parametric EQ + Android App (Kotlin)

### Overview
Add a 5-band parametric EQ to the A2DP audio pipeline, controlled by an Android app over BT SPP (Serial Port Profile). EQ presets are stored in NVS.

---

### Step 1: Enable SPP in sdkconfig.defaults

Add to `sdkconfig.defaults`:
```
CONFIG_BT_SPP_ENABLED=y
```

---

### Step 2: Add SPP Server to Firmware

In `main.c`, add includes:
```c
#include "esp_spp_api.h"
```

Add state variables:
```c
static bool s_spp_connected = false;
static uint32_t s_spp_handle = 0;
```

Add SPP callback:
```c
static void spp_cb(esp_spp_cb_event_t event, esp_spp_cb_param_t *param)
{
    switch (event) {
    case ESP_SPP_INIT_EVT:
        esp_spp_start_srv(ESP_SPP_SEC_NONE, ESP_SPP_ROLE_SLAVE, 0, "BMW-BlueBus-EQ");
        ESP_LOGI(TAG, "SPP server started");
        break;
    case ESP_SPP_SRV_OPEN_EVT:
        s_spp_connected = true;
        s_spp_handle = param->srv_open.handle;
        ESP_LOGI(TAG, "SPP client connected");
        break;
    case ESP_SPP_CLOSE_EVT:
        s_spp_connected = false;
        ESP_LOGI(TAG, "SPP disconnected");
        break;
    case ESP_SPP_DATA_IND_EVT:
        // Parse EQ commands here (see Step 5)
        parse_spp_cmd(param->data_ind.data, param->data_ind.len);
        break;
    default:
        break;
    }
}
```

In `app_main()`, after HFP init, add:
```c
esp_spp_register_callback(spp_cb);
esp_spp_init(ESP_SPP_MODE_CB);
```

---

### Step 3: Implement Biquad Parametric EQ

Add EQ data structures (5 bands):
```c
#define EQ_BANDS 5

typedef struct {
    float b0, b1, b2, a1, a2;
} eq_coeff_t;

typedef struct {
    float x1, x2, y1, y2;
} eq_state_t;

typedef struct {
    float freq;
    float q;
    float gain_db;
    eq_coeff_t coeff;
    eq_state_t state_l;
    eq_state_t state_r;
} eq_band_t;

static eq_band_t s_eq[EQ_BANDS];
static bool s_eq_enabled = true;
```

Add coefficient calculator (Cookbook formula):
```c
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static void eq_calc_coeffs(eq_band_t *band, uint32_t sample_rate)
{
    float w0 = 2.0f * M_PI * band->freq / sample_rate;
    float cos_w0 = cosf(w0);
    float sin_w0 = sinf(w0);
    float A = powf(10.0f, band->gain_db / 40.0f);
    float alpha = sin_w0 / (2.0f * band->q);

    float b0 = 1.0f + alpha * A;
    float b1 = -2.0f * cos_w0;
    float b2 = 1.0f - alpha * A;
    float a0 = 1.0f + alpha / A;
    float a1 = -2.0f * cos_w0;
    float a2 = 1.0f - alpha / A;

    band->coeff.b0 = b0 / a0;
    band->coeff.b1 = b1 / a0;
    band->coeff.b2 = b2 / a0;
    band->coeff.a1 = a1 / a0;
    band->coeff.a2 = a2 / a0;
}
```

Add per-sample filter:
```c
static float eq_process_sample(eq_band_t *band, float in, eq_state_t *st)
{
    float out = band->coeff.b0 * in
              + band->coeff.b1 * st->x1
              + band->coeff.b2 * st->x2
              - band->coeff.a1 * st->y1
              - band->coeff.a2 * st->y2;
    st->x2 = st->x1;
    st->x1 = in;
    st->y2 = st->y1;
    st->y1 = out;
    return out;
}
```

Initialize default bands:
```c
static void eq_init_defaults(void)
{
    float defaults[EQ_BANDS][3] = {
        { 60.0f,   1.0f, 0.0f },   // Sub bass
        { 250.0f,  1.0f, 0.0f },   // Bass
        { 1000.0f, 1.0f, 0.0f },   // Mid
        { 4000.0f, 1.0f, 0.0f },   // Presence
        { 12000.0f, 1.0f, 0.0f },  // Treble
    };
    for (int i = 0; i < EQ_BANDS; i++) {
        s_eq[i].freq = defaults[i][0];
        s_eq[i].q = defaults[i][1];
        s_eq[i].gain_db = defaults[i][2];
        eq_calc_coeffs(&s_eq[i], 44100);
        memset(&s_eq[i].state_l, 0, sizeof(eq_state_t));
        memset(&s_eq[i].state_r, 0, sizeof(eq_state_t));
    }
}
```

---

### Step 4: Integrate EQ into A2DP Audio Callback

Modify `a2dp_data_cb` to apply EQ before volume scaling:
```c
static void a2dp_data_cb(const uint8_t *data, uint32_t len)
{
    if (s_muted || !s_i2s_initialized || s_tx_handle == NULL) return;

    int16_t *samples = (int16_t *)data;
    uint32_t frame_count = len / 4;  // stereo = 2 samples per frame

    if (s_eq_enabled) {
        for (uint32_t i = 0; i < frame_count; i++) {
            float l = samples[i * 2] / 32768.0f;
            float r = samples[i * 2 + 1] / 32768.0f;
            for (int b = 0; b < EQ_BANDS; b++) {
                l = eq_process_sample(&s_eq[b], l, &s_eq[b].state_l);
                r = eq_process_sample(&s_eq[b], r, &s_eq[b].state_r);
            }
            // Clip to [-1.0, 1.0]
            if (l > 1.0f) l = 1.0f; else if (l < -1.0f) l = -1.0f;
            if (r > 1.0f) r = 1.0f; else if (r < -1.0f) r = -1.0f;
            samples[i * 2] = (int16_t)(l * 32767.0f);
            samples[i * 2 + 1] = (int16_t)(r * 32767.0f);
        }
    }

    if (s_volume < 100) {
        uint32_t sample_count = len / 2;
        float scale = s_volume / 100.0f;
        for (uint32_t i = 0; i < sample_count; i++) {
            samples[i] = (int16_t)(samples[i] * scale);
        }
    }
    i2s_channel_write(s_tx_handle, data, len, NULL, portMAX_DELAY);
}
```

Add `#include <math.h>` at the top of `main.c`.

Update `main/CMakeLists.txt` to add `m` library:
```cmake
idf_component_register(SRCS "main.c"
    PRIV_REQUIRES bt nvs_flash driver esp_driver_gpio esp_driver_i2s m
    INCLUDE_DIRS ".")
```

---

### Step 5: SPP Command Protocol

Define a simple text-based protocol (easier to debug):

| Command | Format | Example |
|---------|--------|---------|
| Set band | `EQ:<band>:<freq>:<q>:<gain>` | `EQ:0:60:1.0:+3.5` |
| Enable EQ | `EQ:ON` | |
| Disable EQ | `EQ:OFF` | |
| Get all bands | `EQ?` | Response: `EQ:0:60:1.0:0.0;1:250:1.0:-2.0;...` |
| Save preset | `EQ:SAVE:<name>` | `EQ:SAVE:rock` |
| Load preset | `EQ:LOAD:<name>` | `EQ:LOAD:rock` |

Parse function:
```c
static void parse_spp_cmd(const uint8_t *data, uint32_t len)
{
    char buf[128] = "";
    if (len > 127) len = 127;
    memcpy(buf, data, len);
    buf[len] = '\0';

    if (strncmp(buf, "EQ:ON", 5) == 0) {
        s_eq_enabled = true;
        spp_send("OK\r\n");
    } else if (strncmp(buf, "EQ:OFF", 6) == 0) {
        s_eq_enabled = false;
        spp_send("OK\r\n");
    } else if (strncmp(buf, "EQ:", 3) == 0 && buf[3] >= '0' && buf[3] <= '4') {
        int band = buf[3] - '0';
        float freq, q, gain;
        if (sscanf(buf + 4, ":%f:%f:%f", &freq, &q, &gain) == 3) {
            s_eq[band].freq = freq;
            s_eq[band].q = q;
            s_eq[band].gain_db = gain;
            eq_calc_coeffs(&s_eq[band], 44100);
            memset(&s_eq[band].state_l, 0, sizeof(eq_state_t));
            memset(&s_eq[band].state_r, 0, sizeof(eq_state_t));
            spp_send("OK\r\n");
        }
    } else if (strncmp(buf, "EQ?", 3) == 0) {
        char resp[128];
        int pos = 0;
        for (int i = 0; i < EQ_BANDS; i++) {
            pos += snprintf(resp + pos, sizeof(resp) - pos,
                "%s%d:%.0f:%.1f:%.1f",
                i > 0 ? ";" : "EQ:", i, s_eq[i].freq, s_eq[i].q, s_eq[i].gain_db);
        }
        snprintf(resp + pos, sizeof(resp) - pos, "\r\n");
        spp_send(resp);
    }
}

static void spp_send(const char *msg)
{
    if (s_spp_connected && s_spp_handle != 0) {
        esp_spp_write(s_spp_handle, strlen(msg), (uint8_t *)msg);
    }
}
```

---

### Step 6: NVS Preset Storage

Save/load EQ presets using NVS key-value pairs:
```c
#define EQ_NVS_NAMESPACE "eq_preset"

static void eq_save_preset(const char *name)
{
    nvs_handle_t h;
    if (nvs_open(EQ_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    char key[16];
    for (int i = 0; i < EQ_BANDS; i++) {
        snprintf(key, sizeof(key), "%s_b%d", name, i);
        float params[3] = { s_eq[i].freq, s_eq[i].q, s_eq[i].gain_db };
        nvs_set_blob(h, key, params, sizeof(params));
    }
    nvs_commit(h);
    nvs_close(h);
}

static bool eq_load_preset(const char *name)
{
    nvs_handle_t h;
    if (nvs_open(EQ_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;
    char key[16];
    for (int i = 0; i < EQ_BANDS; i++) {
        snprintf(key, sizeof(key), "%s_b%d", name, i);
        float params[3];
        size_t required = sizeof(params);
        if (nvs_get_blob(h, key, params, &required) != ESP_OK) {
            nvs_close(h);
            return false;
        }
        s_eq[i].freq = params[0];
        s_eq[i].q = params[1];
        s_eq[i].gain_db = params[2];
        eq_calc_coeffs(&s_eq[i], 44100);
        memset(&s_eq[i].state_l, 0, sizeof(eq_state_t));
        memset(&s_eq[i].state_r, 0, sizeof(eq_state_t));
    }
    nvs_close(h);
    return true;
}
```

---

### Step 7: Android Kotlin App

Create a new Android Studio project (Empty Activity, Kotlin).

#### 7a: AndroidManifest.xml permissions
```xml
<uses-permission android:name="android.permission.BLUETOOTH" />
<uses-permission android:name="android.permission.BLUETOOTH_CONNECT" />
<uses-permission android:name="android.permission.BLUETOOTH_ADMIN" />
<uses-permission android:name="android.permission.BLUETOOTH_SCAN" />
```

#### 7b: SPP Connection Manager
```kotlin
// SppManager.kt
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothSocket
import java.io.InputStream
import java.io.OutputStream
import java.util.UUID

object SppManager {
    private const val SPP_UUID = "00001101-0000-1000-8000-00805F9B34FB"
    private var socket: BluetoothSocket? = null
    private var outputStream: OutputStream? = null
    private var inputStream: InputStream? = null
    private var listener: ((String) -> Unit)? = null

    fun connect(deviceAddress: String, onResponse: (String) -> Unit): Boolean {
        val adapter = BluetoothAdapter.getDefaultAdapter() ?: return false
        val device = adapter.getRemoteDevice(deviceAddress) ?: return false
        listener = onResponse
        return try {
            socket = device.createRfcommSocketToServiceRecord(UUID.fromString(SPP_UUID))
            socket?.connect()
            outputStream = socket?.outputStream
            inputStream = socket?.inputStream
            Thread { readLoop() }.start()
            true
        } catch (e: Exception) {
            false
        }
    }

    fun send(command: String) {
        outputStream?.write("$command\n".toByteArray())
    }

    private fun readLoop() {
        val buf = StringBuilder()
        val buffer = ByteArray(256)
        while (true) {
            val bytes = inputStream?.read(buffer) ?: break
            if (bytes > 0) {
                buf.append(String(buffer, 0, bytes))
                while (buf.contains("\r\n")) {
                    val line = buf.substringBefore("\r\n")
                    buf.delete(0, line.length + 2)
                    listener?.invoke(line)
                }
            }
        }
    }

    fun disconnect() {
        try { socket?.close() } catch (_: Exception) {}
        socket = null
        outputStream = null
        inputStream = null
    }
}
```

#### 7c: EQ Band Data Model
```kotlin
// EqBand.kt
data class EqBand(
    val index: Int,
    var freq: Float,
    var q: Float,
    var gainDb: Float
)
```

#### 7d: EQ Control Functions
```kotlin
// EqController.kt
object EqController {
    val bands = listOf(
        EqBand(0, 60f, 1f, 0f),
        EqBand(1, 250f, 1f, 0f),
        EqBand(2, 1000f, 1f, 0f),
        EqBand(3, 4000f, 1f, 0f),
        EqBand(4, 12000f, 1f, 0f),
    )
    var enabled: Boolean = true

    fun sendBand(band: EqBand) {
        val sign = if (band.gainDb >= 0) "+" else ""
        SppManager.send("EQ:${band.index}:${band.freq}:${band.q}:${sign}${band.gainDb}")
    }

    fun sendEnable() {
        SppManager.send(if (enabled) "EQ:ON" else "EQ:OFF")
    }

    fun requestAll() {
        SppManager.send("EQ?")
    }

    fun savePreset(name: String) {
        SppManager.send("EQ:SAVE:$name")
    }

    fun loadPreset(name: String) {
        SppManager.send("EQ:LOAD:$name")
    }

    fun parseResponse(line: String) {
        if (!line.startsWith("EQ:")) return
        val parts = line.removePrefix("EQ:").split(";")
        for (part in parts) {
            val fields = part.split(":")
            if (fields.size != 4) continue
            val idx = fields[0].toIntOrNull() ?: continue
            if (idx in bands.indices) {
                bands[idx].freq = fields[1].toFloatOrNull() ?: continue
                bands[idx].q = fields[2].toFloatOrNull() ?: continue
                bands[idx].gainDb = fields[3].toFloatOrNull() ?: continue
            }
        }
    }
}
```

#### 7e: MainActivity Layout (activity_main.xml)
```xml
<?xml version="1.0" encoding="utf-8"?>
<LinearLayout xmlns:android="http://schemas.android.com/apk/res/android"
    android:layout_width="match_parent"
    android:layout_height="match_parent"
    android:orientation="vertical"
    android:padding="16dp">

    <Switch android:id="@+id/eqEnableSwitch"
        android:text="EQ Enabled"
        android:layout_width="wrap_content"
        android:layout_height="wrap_content" />

    <!-- Repeat for each band (0-4): -->
    <LinearLayout android:layout_width="match_parent"
        android:layout_height="wrap_content"
        android:orientation="horizontal">
        <TextView android:id="@+id/bandLabel0"
            android:text="60 Hz"
            android:layout_width="80dp"
            android:layout_height="wrap_content" />
        <SeekBar android:id="@+id/bandSeek0"
            android:max="200"
            android:progress="100"
            android:layout_width="0dp"
            android:layout_weight="1"
            android:layout_height="wrap_content" />
        <TextView android:id="@+id/bandValue0"
            android:text="0.0 dB"
            android:layout_width="60dp"
            android:layout_height="wrap_content" />
    </LinearLayout>
    <!-- Repeat for bands 1-4 with IDs bandSeek1..4, bandValue1..4, bandLabel1..4 -->

    <Button android:id="@+id/saveBtn"
        android:text="Save Preset"
        android:layout_width="wrap_content"
        android:layout_height="wrap_content" />
    <Button android:id="@+id/loadBtn"
        android:text="Load Preset"
        android:layout_width="wrap_content"
        android:layout_height="wrap_content" />
</LinearLayout>
```

#### 7f: MainActivity.kt
```kotlin
class MainActivity : AppCompatActivity() {
    private val bandFreqs = floatArrayOf(60f, 250f, 1000f, 4000f, 12000f)
    private val bandFreqLabels = arrayOf("60 Hz", "250 Hz", "1 kHz", "4 kHz", "12 kHz")

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        // Request BT permissions (Android 12+)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            requestPermissions(arrayOf(
                Manifest.permission.BLUETOOTH_CONNECT,
                Manifest.permission.BLUETOOTH_SCAN
            ), 1)
        }

        // Connect to paired device (find BMW-BlueBus in paired list)
        val adapter = BluetoothAdapter.getDefaultAdapter()
        val device = adapter?.bondedDevices?.find { it.name == "BMW-BlueBus" }
        if (device != null) {
            SppManager.connect(device.address) { line ->
                runOnUiThread { EqController.parseResponse(line) }
            }
        }

        // Wire up each band's SeekBar (progress 0-200 maps to -10dB..+10dB)
        for (i in 0..4) {
            val seekBar = findViewById<SeekBar>(
                resources.getIdentifier("bandSeek$i", "id", packageName))
            val valueLabel = findViewById<TextView>(
                resources.getIdentifier("bandValue$i", "id", packageName))
            val freqLabel = findViewById<TextView>(
                resources.getIdentifier("bandLabel$i", "id", packageName))
            freqLabel?.text = bandFreqLabels[i]

            seekBar?.setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
                override fun onProgressChanged(sb: SeekBar?, progress: Int, fromUser: Boolean) {
                    val gainDb = (progress - 100) / 10f  // -10.0 to +10.0
                    valueLabel?.text = String.format("%.1f dB", gainDb)
                    if (fromUser) {
                        EqController.bands[i].gainDb = gainDb
                        EqController.sendBand(EqController.bands[i])
                    }
                }
                override fun onStartTrackingTouch(sb: SeekBar?) {}
                override fun onStopTrackingTouch(sb: SeekBar?) {}
            })
        }

        // EQ enable switch
        findViewById<Switch>(R.id.eqEnableSwitch).setOnCheckedChangeListener { _, checked ->
            EqController.enabled = checked
            EqController.sendEnable()
        }
    }
}
```

---

### Firmware Implementation Order
1. Add `CONFIG_BT_SPP_ENABLED=y` to `sdkconfig.defaults`
2. Add `#include <math.h>` and `#include "esp_spp_api.h"` to `main.c`
3. Add EQ structures and functions (Step 3)
4. Modify `a2dp_data_cb` with EQ processing (Step 4)
5. Add SPP server (Step 2) and command parser (Step 5)
6. Add NVS preset storage (Step 6)
7. Call `eq_init_defaults()` in `app_main()` before I2S init
8. Build, flash, test

---

## Part 2: Microphone Input for HFP Calls (Future)

### The Problem
The PCM5102A is output-only (DAC, no ADC). For HFP calls, the phone needs to receive your microphone audio. Currently `hfp_audio_send_cb` sends silence.

### Recommended Microphone Boards

| Board | Interface | Sample Rate | Notes |
|-------|-----------|-------------|-------|
| **INMP441** | I2S (digital) | 8-48kHz | Best option. I2S digital MEMS mic, ~$2. Shares BCK/WS with PCM5102A. Just add DIN pin. |
| **MAX9814** | Analog + ADC needed | Variable | Analog mic with AGC. Needs separate ADC. Not recommended for this setup. |
| **SPH0645LM4H** | I2S (digital) | 8-96kHz | Higher quality I2S MEMS mic, ~$5. Same interface as INMP441. |
| **ICS-43434** | I2S (digital) | 8-52kHz | Low-noise I2S MEMS mic, ~$4. Good for car environment. |

### Recommended: INMP441
- ~$2 on AliExpress/Amazon
- I2S digital output — shares BCK and WS lines with your PCM5102A
- Only needs 1 additional GPIO for DATA (currently defined as `I2S_MIC_DATA 35`)
- 3.3V compatible, no ADC needed
- Perfect for 8kHz CVSD/mSBC HFP audio

### Wiring Addition (INMP441)
```
INMP441 Pin    -> ESP32 Pin
-----------      ----------
VDD           -> 3.3V
GND           -> GND
SCK (BCK)     -> GPIO 26  (shared with PCM5102A BCK)
WS            -> GPIO 25  (shared with PCM5102A WS)
SD (DATA)     -> GPIO 35  (new pin - ADC1 only, input-only)
L/R           -> GND     (left channel select)
```

### Implementation Steps

1. **Add I2S RX channel** — Modify `i2s_init()` to create both TX and RX channels on I2S_NUM_0. The ESP32 I2S peripheral supports full-duplex on the same BCK/WS bus.

2. **Modify `i2s_init()` signature** to `i2s_init(uint32_t rate, bool with_mic)`:
   ```c
   static i2s_chan_handle_t s_rx_handle = NULL;

   // In i2s_init(), change gpio_cfg.din:
   .din = with_mic ? I2S_MIC_DATA : I2S_GPIO_UNUSED,

   // Create both channels:
   i2s_new_channel(&chan_cfg, &s_tx_handle, with_mic ? &s_rx_handle : NULL);
   ```

3. **Init RX channel** with same clock but stereo slot config (I2S requires stereo even for mono mic):
   ```c
   if (with_mic && s_rx_handle) {
       i2s_std_slot_config_t rx_slot = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
           I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
       i2s_std_config_t rx_std = { .clk_cfg = clk_cfg, .slot_cfg = rx_slot, .gpio_cfg = gpio_cfg };
       i2s_channel_init_std_mode(s_rx_handle, &rx_std);
       i2s_channel_enable(s_rx_handle);
   }
   ```

4. **Modify `switch_to_sco()`** to enable mic:
   ```c
   static void switch_to_sco(void) {
       if (s_is_a2dp_mode || !s_i2s_initialized) {
           i2s_init(8000, true);  // true = with_mic
       }
   }
   ```

5. **Replace `hfp_audio_send_cb`** to read from I2S mic instead of sending silence:
   ```c
   static uint32_t hfp_audio_send_cb(uint8_t *data, uint32_t len) {
       if (data == NULL || len == 0) return 0;
       if (s_rx_handle == NULL) {
           memset(data, 0, len);
           return len;
       }
       // HFP expects mono 16-bit, I2S gives stereo 16-bit
       // Read 2x the stereo data, then downmix
       uint32_t mono_samples = len / 2;
       uint32_t stereo_bytes = mono_samples * 4;
       uint8_t *stereo_buf = malloc(stereo_bytes);
       if (stereo_buf == NULL) {
           memset(data, 0, len);
           return len;
       }
       size_t bytes_read = 0;
       i2s_channel_read(s_rx_handle, stereo_buf, stereo_bytes, &bytes_read, 0);
       // Extract left channel only (INMP441 on L/R=GND = left)
       int16_t *mono = (int16_t *)data;
       int16_t *stereo = (int16_t *)stereo_buf;
       uint32_t frames_read = bytes_read / 4;
       for (uint32_t i = 0; i < frames_read && i < mono_samples; i++) {
           mono[i] = stereo[i * 2];  // left channel
       }
       free(stereo_buf);
       return len;
   }
   ```

6. **Important: GPIO 35 limitation** — GPIO 35 is input-only and belongs to ADC1. This is fine for I2S data input, but note it has no internal pull-up/pull-down. The INMP441 will drive it actively.

7. **Clean up RX channel** when switching back to A2DP mode:
   ```c
   static void switch_to_a2dp(void) {
       if (!s_is_a2dp_mode || !s_i2s_initialized) {
           i2s_init(44100, false);  // false = no mic for A2DP
       }
   }
   ```

### Notes
- The PCM5102A and INMP441 share the same I2S bus (BCK, WS) — this is standard I2S full-duplex and works correctly
- A2DP mode does NOT need the mic, so `i2s_init(44100, false)` keeps it simple
- If you hear echo on calls, add a simple acoustic echo canceller or reduce speaker volume
- GPIO 35 is on ADC1 — do NOT use any ADC2 pins (GPIO 4, 0, 2, 15, 13, 12, 14, 27, 25, 26) as they conflict with Wi-Fi/BT
