/*
 * Copyright (C) 2025 Yunis <schnackus>,
 *                    Patrick Pedersen <ctx.xda@gmail.com>,
 *                    TuDo Makerspace
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 *
 */

/*
 * Firmware for the 2nd revision of the TuDo Makerspace Coinbox.
 *
 *                           +-----+
 *                           |  ?  |
 *                           +-----+
 *
 * This version runs on the much more powerful ESP32-S3 and supports
 * wireless configuration, debugging, and sample uploads via HTTP.
 * It also has significantly more memory for storing samples compared
 * to the previous Arduino-based version.
 * Lastly, this revision comes with a custom PCB for both the controller
 * board and the photodiode-based sensor board.
 *
 * For configuration, please refer to the config.h file!
 */

/*
 * HTTP Endpoints:
 * - /config                (GET)   Enter configuration mode, allowing sample uploads and OTA updates. Disables sound playback.
 * - /<sample_number>       (POST)  Upload a sample file (WAV, 8-bit Unsigned PCM, 16kHz, max 5s). Requires CONFIG mode!
 * - /reset                 (GET)   Reset samples to factory defaults
 * - /play<sample_number>   (GET)   Play a sample by number for debugging. Will sound worse due to WiFi interference.
 * - /measure               (GET)   Enter measurement mode, allowing sensor values to be polled via UDP. Used for debugging and calibration.
 * - /restart               (GET)   Restart the device, useful for exiting CONFIG mode.
 */

/* Example to upload a sample:
 *  1. Put device into CONFIG mode via /config:
 *      curl -X GET http://<STATIC_IP>/config
 *  2. Upload a sample (lower sample number has higher probability):
 *      curl -X POST -F "file=@/path/to/sample.wav" http://<STATIC_IP>/<sample_number>
 *  3. Play the sample to test it (note that this will sound choppy due to WiFi interference):
 *      curl -X GET http://<STATIC_IP>/play<sample_number>
 *  4. Exit CONFIG mode by restarting the device:
 *      curl -X GET http://<STATIC_IP>/restart
 */

/* Example to measure sensor via UDP:
 *  1. Put device into MEASURE mode via /measure:
 *      curl -X GET http://<STATIC_IP>/measure
 *  2. Use netcat to listen for sensor values:
 *      nc -u <STATIC_IP> 12345
 *  3. The device will send sensor values every 20ms (50Hz).
 *  4. To stop measuring, restart the device:
 *      curl -X GET http://<STATIC_IP>/restart
 */

#include <array>
#include <memory>

#include <Arduino.h>
#include <ArduinoOTA.h>
#include <LittleFS.h>
#include <XT_DAC_Audio.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>

#include <sounds.h>

#include "config.h"

/////////////////////////////////////////////////////////////////////////////////
// Logging Globals
/////////////////////////////////////////////////////////////////////////////////

std::vector<std::string> log_entries; // Stores recent log lines
std::vector<uint16_t> adc_values;     // Stores recent ADC values for debugging
std::vector<uint16_t> avg_adc_values; // Stores recent averaged ADC values for debugging

///////////////////////////////////////////////////////////////////////////////
// Configuration Globals
///////////////////////////////////////////////////////////////////////////////

static unsigned long config_timeout = 0; // Timestamp when config mode should time out

///////////////////////////////////////////////////////////////////////////////
// Coin Detection Globals
///////////////////////////////////////////////////////////////////////////////

enum CoinState { BLOCKING, IDLE, SPIKE_START, SPIKE_END };

static float     baseline        = 0;       // running average
static bool      baseline_init   = false;   // whether baseline has been initialized
static uint32_t  spike_start_ms  = 0;       // timestamp when spike started
static CoinState coin_state      = IDLE;    // current state of coin detection state machine

/////////////////////////////////////////////////////////////////////////////////
// Audio Globals
/////////////////////////////////////////////////////////////////////////////////

XT_DAC_Audio_Class DacAudio(DAC_PIN,0);             // DAC audio output class
std::array<uint32_t, N_SAMPLES> probabilities = {}; // Stores probabilities for each sample

std::array<File, N_SAMPLES> sample_files = {};           // Stores files for each sample
std::array<std::vector<uint8_t>, N_SAMPLES> sample_buffers;
std::array<std::unique_ptr<XT_Wav_Class>, N_SAMPLES> clips;
std::array<uint32_t, N_SAMPLES> sample_duration_ms{};
XT_Wav_Class* current_clip = nullptr;

/////////////////////////////////////////////////////////////////////////////////
// Web Server and UDP Globals
/////////////////////////////////////////////////////////////////////////////////

AsyncWebServer server(80);  // Web server on port 80 for sample uploads and configuration
WiFiUDP udp;                // UDP server to debug sensor data
IPAddress remote_ip;        // Store the IP of the last client that sent data
uint16_t remote_port = 0;   // Store the port of the last client
bool client = false;        // Whether we have an active client
uint32_t last_udp_send = 0; // Timestamp of last UDP send

/////////////////////////////////////////////////////////////////////////////////
// Device Mode Globals
/////////////////////////////////////////////////////////////////////////////////

enum device_mode {
    BOOT,
    MEASURE,
    CONFIG,
    NORMAL,
    RESTART
};

device_mode mode = BOOT;
unsigned long boot_done_tstamp;

/////////////////////////////////////////////////////////////////////////////////
// Logging Functions
/////////////////////////////////////////////////////////////////////////////////

void log(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    char buffer[LOG_ENTRY_LEN];
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    // Add to log lines
    if (log_entries.size() >= LOG_ENTRIES) {
        log_entries.erase(log_entries.begin());
    }

    // Truncate if too long
    if (strlen(buffer) >= LOG_ENTRY_LEN) {
        buffer[LOG_ENTRY_LEN - 1] = '\0'; // Ensure null termination
    }

    char log_entry[LOG_ENTRY_LEN + 16]; // Extra space for timestamp
    snprintf(log_entry, sizeof(log_entry), "[%lu] %s", millis(), buffer);

    log_entries.push_back(log_entry);

    // Print to Serial
    Serial.print(log_entry);
}

/////////////////////////////////////////////////////////////////////////////////
// Sample related functions
/////////////////////////////////////////////////////////////////////////////////

// Initialize probabilities for sample selection based on PROBABILITY_MAIN_SAMPLE
void init_prob()
{
    constexpr unsigned P = PROBABILITY_MAIN_SAMPLE;
    constexpr unsigned q = 100 - P;

    unsigned remain = 100;

    for (int i = 0; i < N_SAMPLES - 1; ++i) {
        probabilities[i] = (P * remain) / 100;
        remain          -= probabilities[i];
    }
    probabilities[N_SAMPLES - 1] = remain;

    log("Probabilities initialised:\n");
    for (int i = 0; i < N_SAMPLES; ++i) {
        log("\tSample %d: %u%%\n", i, probabilities[i]);
    }
}

void load_clip(int idx)
{
    // Check if sample file exists
    if (!sample_files[idx]) {
        log("No file for sample %d\n", idx);
        return;
    }

    // Get size of sample
    sample_files[idx].seek(0);
    size_t sz = sample_files[idx].size();

    // Load sample from file into buffer (RAM)
    sample_buffers[idx].resize(sz);
    sample_files[idx].readBytes(reinterpret_cast<char*>(sample_buffers[idx].data()), sz);

    // Create clip from buffer
    clips[idx] = std::unique_ptr<XT_Wav_Class>(
                     new XT_Wav_Class(sample_buffers[idx].data()));

    // Calculate sample duration in milliseconds

    // WAV header is a fixed 44 bytes for PCM files.
    const size_t payload_bytes = (sz > 44) ? sz - 44 : 0;

    // 1 byte per sample (8‑bit mono)
    // duration = samples / sampling rate (16kHz)
    sample_duration_ms[idx] = (payload_bytes * 1000UL) / 16000UL;

    // Trim to MAX_DURATION (failsafe if bad payload)
    if (sample_duration_ms[idx] > MAX_DURATION * 1000UL) {
        sample_duration_ms[idx] = MAX_DURATION * 1000UL;
    }

    log("Sample %d duration: %lu ms\n",
        idx, (unsigned long)sample_duration_ms[idx]);
}

// Initialize/Load samples from LittleFS or create default ones if they don't exist
void init_samples() {
    for (int i = 0; i < N_SAMPLES; ++i) {
        String filename = "/" + String(i) + ".wav";

        // Sample exists
        if (LittleFS.exists(filename)) {
            sample_files[i] = LittleFS.open(filename, "r");
            if (!sample_files[i]) {
                log("Failed to open %s\n", filename.c_str());
            } else {
                log("Loaded sample %d from %s\n", i, filename.c_str());
            }
        }

        // Sample does not exist, create with default sound
        else {
            log("Sample %d missing\n", i);

            // Load coin sound as default
            sample_files[i] = LittleFS.open(filename, "w");
            if (sample_files[i]) {
                switch(i) {
                case 1:
                    sample_files[i].write(powerup, sizeof(powerup));
                    break;
                case 2:
                    sample_files[i].write(oneup, sizeof(oneup));
                    break;
                default:
                    sample_files[i].write(coin, sizeof(coin)); // Default to coin sound
                    break;

                }
                sample_files[i].close();
                log("Using default coin sound for sample %d\n", i);
            } else {
                log("FATAL: Failed to create sample %d\n", i);
                while(true);
            }
        }

        // Load the sample into memory
        load_clip(i);
    }
}

// Handle file uploads for samples
void handle_upload(unsigned int nsample, AsyncWebServerRequest *request,
                   String filename, size_t index, uint8_t *data, size_t len, bool final) {

    config_timeout = millis() + CONFIG_TIMEOUT;

    if (nsample >= N_SAMPLES) {
        log("Sample %u: Rejecting upload, invalid sample number (max %d)\n", nsample, N_SAMPLES - 1);
    }

    // First chunk
    if (index == 0) {
        size_t left = LittleFS.totalBytes() - LittleFS.usedBytes();
        if (request->contentLength() > SAMPLE_SIZE ||
                request->contentLength() > left) {
            request->send(507, "text/plain", "Sample exceeds 5s\n");
            log("Sample %u: Rejected upload, too large (%u B)\n", nsample, request->contentLength());
            return;
        }

        log("Sample %u: Uploading %s (%u B)\n",
            nsample, filename.c_str(), request->contentLength());

        File file = LittleFS.open("/" + String(nsample) + ".wav", "w");
        request->_tempFile = file;
    }

    // Write chunk
    if (request->_tempFile) {
        request->_tempFile.write(data, len);
    }

    // Final chunk
    if (final && request->_tempFile) {
        request->_tempFile.close();
        log("Sample %u: Upload complete\n", nsample);

        sample_files[nsample] = LittleFS.open("/" + String(nsample) + ".wav", "r");
        if (!sample_files[nsample]) {
            log("Sample %u: Failed to open uploaded file\n", nsample);
        }

        load_clip(nsample);

        request->send(200, "text/plain", "Sample uploaded successfully\n");
    }
}

// Reset samples to factory defaults
void reset_samples() {
    log("Factory reset: resetting samples to defaults...\n");

    config_timeout = millis() + CONFIG_TIMEOUT;

    for (int i = 0; i < N_SAMPLES; ++i) {
        String fn = "/" + String(i) + ".wav";

        if (sample_files[i]) sample_files[i].close();
        LittleFS.remove(fn); // ensure truncate

        File f = LittleFS.open(fn, "w");
        if (!f) {
            log("Failed to open %s for writing\n", fn.c_str());
            continue;
        }

        switch(i) {
        case 1:
            f.write(powerup, sizeof(powerup));
            break;
        case 2:
            f.write(oneup, sizeof(oneup));
            break;
        default:
            f.write(coin, sizeof(coin)); // Default to coin sound
        }

        f.close();

        // Re-open read-only to use during playback
        sample_files[i] = LittleFS.open(fn, "r");
        if (!sample_files[i]) {
            log("Failed to reopen %s\n", fn.c_str());
        }

        // Load the sample into memory
        load_clip(i);
    }
}

// Play a sample by index
void play_sample(int idx)
{
    if (clips[idx]) {
        current_clip = clips[idx].get();
        DacAudio.Play(current_clip);
    }
}

// Pick a random sample based on probabilities
unsigned int pick_sample() {
    if (probabilities[0] == 0) {
        log("Probabilities not initialized!\n");
        init_prob();
    }

    unsigned int r = random(100);
    unsigned int cumulative = 0;
    for (unsigned int i = 0; i < N_SAMPLES; ++i) {
        cumulative += probabilities[i];
        if (r < cumulative) {
            log("Playing sample %u\n", i);
            return i;
        }
    }

    // Should not happen, but just in case
    log("WARNING: Failed to pick sample, falling back to first sample\n");
    return 0;
}

/////////////////////////////////////////////////////////////////////////////////
// Coin Detection Functions
/////////////////////////////////////////////////////////////////////////////////

unsigned int take_samples = ADC_SAMPLES; // Number of samples to take for baseline

// Poll the coin sensor and handle coin detection logic
bool poll_coin_sensor(bool update_baseline = true) {
    static uint32_t last_sample_us = 0;
    static unsigned long block_until = 0;
    static unsigned int read = 0;
    static uint16_t last_read = analogRead(SENSOR_PIN);
    static int16_t max_updiff = -1;

    bool coin_hit = false;

    uint32_t now_us = micros();
    if (now_us - last_sample_us < SAMPLE_PERIOD_US) {
        return false;
    }
    last_sample_us = now_us;

    if (take_samples > 0) {
        if (take_samples == ADC_SAMPLES) {
            read = 0;
        }
        uint16_t raw = analogRead(SENSOR_PIN);

        if (adc_values.size() >= LOG_ADC_VALUES) {
            adc_values.erase(adc_values.begin());
        }
        adc_values.push_back(raw);

        read += raw;
        take_samples--;
        return false;
    } else {
        read /= ADC_SAMPLES;

        if (avg_adc_values.size() >= LOG_ADC_AVG_VALUES) {
            avg_adc_values.erase(avg_adc_values.begin());
        }
        avg_adc_values.push_back(read);

        take_samples = ADC_SAMPLES;
    }

    if (!baseline_init) {
        baseline = read;
        baseline_init = true;
    }

    int16_t diff = (int16_t)read - (int16_t)baseline;

    switch (coin_state) {
    case BLOCKING:
        if (baseline < LOW_THRESHOLD || baseline > HIGH_THRESHOLD ||
                read < LOW_THRESHOLD || read > HIGH_THRESHOLD) {
            block_until = millis() + BLOCK_AFTER_LID_OPEN;
        }
        else if (millis() >= block_until) {
            coin_state = IDLE;
            log("Coin detection reactivated\n");
        }
        break;
    case IDLE:
        // If we're outside the thresholds, the lid is likely open
        if (baseline < LOW_THRESHOLD || baseline > HIGH_THRESHOLD ||
                read < LOW_THRESHOLD || read > HIGH_THRESHOLD) {
            log("Lid open detected (sensor exceeds threshold), blocking coin detection!\n");
            log("Detection data:\n\tThreshold High: %d\n\tThershold Low: %d\n\tBaseline: %.2f\n\tRead: %u\n\tDiff: %d\n",
                HIGH_THRESHOLD, LOW_THRESHOLD, baseline, read, (int)diff);
            coin_state = BLOCKING;
            block_until = millis() + BLOCK_AFTER_LID_OPEN;
            break;
        }

        // If the difference is above the threshold, start a spike
        if (diff < -SPIKE_THRESHOLD) {
            coin_state     = SPIKE_START;
            spike_start_ms = millis();
        }
        break;

    case SPIKE_START: {
        // Spike within time threshold
        int16_t updiff = (int16_t)read - (int16_t)last_read;

        if (updiff > max_updiff) {
            max_updiff = updiff;
        }

        if (updiff > SPIKE_THRESHOLD) {
            coin_state = SPIKE_END;
        }
        // Discard spikes that last too long
        else if (millis() - spike_start_ms > SPIKE_MAX_MS) {
            log("Lid open detected (spike too long), blocking coin detection!\n");
            coin_state = BLOCKING;
            block_until = millis() + BLOCK_AFTER_LID_OPEN;
        }
        break;
    }
    case SPIKE_END:
        coin_hit = true;
        coin_state = IDLE;
        break;
    }

    if ((coin_state == IDLE || coin_state == BLOCKING) && update_baseline) {
        baseline += BASELINE_ALPHA * ((float)read - baseline);
    }

    last_read = read;

    return coin_hit;
}

// Allows for remote measurement of sensor values via UDP
// Used for debugging and calibration
void measure_sensor() {
    static uint32_t last_sample_us = 0;
    uint32_t now_us = micros();

    // Sample at 500Hz (every 2000µs)
    if (now_us - last_sample_us < SAMPLE_PERIOD_US) return;
    last_sample_us = now_us;

    uint16_t raw = analogRead(SENSOR_PIN);
    Serial.println(raw);

    // Check for incoming UDP packets (handles keep-alive)
    int packetSize = udp.parsePacket();
    if (packetSize) {
        remote_ip = udp.remoteIP();
        remote_port = udp.remotePort();
        client = true;

        // Read and discard any data (we just care about the connection)
        char temp[64];
        udp.read(temp, sizeof(temp));
    }

    // Send data if we have an active client
    if (client) {
        uint32_t now = millis();
        if (now - last_udp_send >= UDP_SEND_INTERVAL) {
            char buffer[20];
            snprintf(buffer, sizeof(buffer), "%u\n", raw);
            udp.beginPacket(remote_ip, remote_port);
            udp.write((uint8_t*)buffer, strlen(buffer));
            udp.endPacket();
            last_udp_send = now;
        }
    }
}

/////////////////////////////////////////////////////////////////////////////////
// mDNS and Web Server Setup
/////////////////////////////////////////////////////////////////////////////////

// Expose mDNS services for device discovery and configuration
// NOTE: Currently not working
void expose_mDNS() {
    // Register mDNS host name as coinbox.local
    if (MDNS.begin("coinbox")) {
        log("mDNS host name coinbox.local registered\n");
    } else {
        log("Failed to register mDNS host name\n");
        return;
    }

    // Register HTTP service for device configuration
    if (MDNS.addService("http", "tcp", 80)) {
        log("mDNS service _http._tcp. registered on port 80\n");
    } else {
        log("Failed to register mDNS HTTP service\n");
        return;
    }
}

// Initialize web server routes for sample uploads and playback
void init_routes() {
    for (int i = 0; i < N_SAMPLES; ++i)
    {
        const int sample = i;

        server.on(("/" + String(sample)).c_str(), HTTP_POST,
                  [](AsyncWebServerRequest *request)
        {
        },
        [sample](AsyncWebServerRequest *request,
                 String filename, size_t index,
                 uint8_t *data, size_t len, bool final)
        {
            if (mode != CONFIG) {
                request->send(403, "text/plain", "Forbidden: Not in config mode\n");
                return;
            }
            handle_upload(sample, request,
                          filename, index, data, len, final);
        });

        server.on(("/play" + String(sample)).c_str(), HTTP_GET,
                  [sample](AsyncWebServerRequest *request)
        {
            if (sample_files[sample]) {
                play_sample(sample);
                request->send(200, "text/plain", "Playing sample " + String(sample) + "\n");
            } else {
                request->send(404, "text/plain", "Sample not found\n");
            }
        });
    }

    server.on("/ping", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send(200, "text/plain", "pong\n");
    });

    server.on("/measure", HTTP_GET, [](AsyncWebServerRequest *request) {
        log("Entering measurement mode...\n");
        request->send(200, "text/plain", "Entering measurement mode...\n");
        udp.begin(UDP_LISTEN_PORT);
        log("UDP server started on port %d\n", UDP_LISTEN_PORT);
        mode = MEASURE;
    });

    // Returns CSV with recent ADC values for debugging
    server.on("/dump", HTTP_GET, [](AsyncWebServerRequest *request) {
        String response = "ADC Values:\n";
        for (const auto& value : adc_values) {
            response += String(value) + ",";
        }
        response += "\nAveraged ADC Values:\n";
        for (const auto& value : avg_adc_values) {
            response += String(value) + ",";
        }
        request->send(200, "text/plain", response);
        log("Dumped ADC values to client\n");
    });

    server.on("/config", HTTP_GET, [](AsyncWebServerRequest *request) {
        log("Entering config mode...\n");
        request->send(200, "text/plain", "Entering Config mode...\n");
        ArduinoOTA.begin();
        mode = CONFIG;

        // Failsafe so device is not accidentally stuck in config mode forever
        config_timeout = millis() + CONFIG_TIMEOUT;
    });

    server.on("/restart", HTTP_GET, [](AsyncWebServerRequest *request) {
        log("Restarting device...\n");
        request->send(200, "text/plain", "Restarting...\n");
        ArduinoOTA.end();
        udp.stop();
        mode = RESTART; // Signal to restart
    });

    server.on("/reset", HTTP_GET, [](AsyncWebServerRequest *request) {
        log("Resetting samples to factory defaults...\n");
        request->send(200, "text/plain", "Resetting samples...\n");
        reset_samples();
    });

    server.on("/log", HTTP_GET, [](AsyncWebServerRequest *request) {
        String response;
        for (const auto& entry : log_entries) {
            response += String(entry.c_str());
        }
        request->send(200, "text/plain", response);
    });
}

/////////////////////////////////////////////////////////////////////////////////
// Main Routines
/////////////////////////////////////////////////////////////////////////////////

void setup() {
    Serial.begin(115200);

    pinMode(SENSOR_PIN, INPUT);
    analogReadResolution(12);
    analogSetAttenuation(ADC_11db);

    probabilities.fill(0); // Will be initialized later

    IPAddress gateway(192, 168, 0, 1);
    IPAddress subnet(255, 255, 255, 0);

    if (!WiFi.config(IPAddress(STATIC_IP), gateway, subnet)) {
        log("Failed to configure static IP\n");
    }

    WiFi.begin(SSID, PASSWORD);
    unsigned long tout_start = millis();
    bool fail = false;

    log("Connecting to WiFi...\n");

    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - tout_start >= WIFI_CONNECT_TIMEOUT) {
            fail = true;
            return;
        }
    }

    if (fail) {
        log("WiFi connection timeout, continuing without connection...\n");
    } else {
        log("Connected to WiFi\n");
        log(("IP Address: " + std::string(WiFi.localIP().toString().c_str()) + "\n").c_str());
    }

    if (!LittleFS.begin(true)) {
        log("FATAL: LittleFS mount failed\n");
        while(true);
    }

    init_samples();
    init_routes();
    init_prob();
    server.begin();
    expose_mDNS();

    boot_done_tstamp = millis() + BOOT_TIME * 1000;
    log(("Entering boot mode, ignoring sensor input for " + std::to_string(BOOT_TIME) + " seconds\n").c_str());
}

void loop() {
    switch(mode) {

    /* Boot Mode:
     * Waits for BOOT_TIME to elapse, providing a guaranteed time window
     * during which the device can be put into config mode.
     * This is a failsafe that prevents the device from immediately switching to normal mode after
     * boot, which could happen due to unexpected sensor behavior or misconfigured detection parameters.
     */
    case BOOT: {
        if (millis() >= boot_done_tstamp) {
            mode = NORMAL;
            log("Ready to detect coins!\n");
        }
        break;
    }

    /* Measure Mode:
     * Activated via a GET request to /measure.
     * Allows measurement of sensor values via serial (cable) and UDP (wirelessly).
     * Used for debugging and calibration.
     */
    case MEASURE: {
        measure_sensor();
        DacAudio.FillBuffer();
        break;
    }

    /* Config Mode:
     * Activated via a GET request to /config.
     * Enables safe uploading of new samples and OTA updates.
     * Sound playback is disabled in this mode.
     * The device remains in config mode until explicitly restarted,
     * e.g., by sending a GET request to /restart.
     */
    case CONFIG: {
        if (millis() >= config_timeout) {
            log("Config mode timed out, restarting...\n");
            ArduinoOTA.end();
            udp.stop();
            mode = RESTART; // Signal to restart
            return;
        }

        ArduinoOTA.handle();
        DacAudio.FillBuffer();
        break;
    }

    /* Normal Mode:
     * Normal operation mode, where the device waits for the first coin.
     * After the first coin is detected, it switches to normal mode.
     * In this mode, the device handles coin detection and plays sounds.
     * If a sound is already playing, it waits for COOLDOWN before processing new coins.
     */
    case NORMAL: {
        static unsigned long  last_coin_tstamp = 0;     // Last time a coin was detected
        static unsigned long  playing_until = 0;        // When the current sound playback ends
        static bool           wifi_active     = true;   // Whether WiFi is active
        static unsigned long  reactive_wifi_at = 0;     // When to reactivate WiFi after disabling it

        // Poll the coin sensor
        bool playing = (millis() < playing_until);
        if (poll_coin_sensor(!playing)) {

            reactive_wifi_at = millis() + REACTIVATE_WIFI_AFTER;

            if (millis() - last_coin_tstamp < COOLDOWN) {
                return; // Ignore if coin detected too soon
            }

            last_coin_tstamp = millis();

            unsigned int pick = pick_sample();

            // Shouldn't happen, but just to be sure
            if (pick >= N_SAMPLES) {
                pick = 0; // Fallback to first sample if out of range
                log("WARNING: Sample index out of range, falling back to sample 0\n");
            }

            playing_until = millis() + sample_duration_ms[pick];

            // WiFi interferes with audio playback, so disable it after the first coin
            if (wifi_active) {
                server.end();
                WiFi.disconnect(true);
                WiFi.mode(WIFI_OFF);
                mode = NORMAL;
                wifi_active = false;
                reactive_wifi_at = millis() + REACTIVATE_WIFI_AFTER;
                log("Disabling WiFi to prevent sound interference\n");
            }

            play_sample(pick);

        }
#if REACTIVATE_WIFI_AFTER > 0
        else if (!wifi_active && millis() >= reactive_wifi_at) {
            // Reactivate WiFi after REACTIVATE_WIFI_AFTER ms
            log("Reactivating WiFi after %d ms\n", REACTIVATE_WIFI_AFTER);
            WiFi.mode(WIFI_STA);
            WiFi.begin(SSID, PASSWORD);
            wifi_active = true;
            server.begin();
        }
#endif

        DacAudio.FillBuffer();
        break;
    }
    // Restart Signaled! Give time to finish any ongoing tasks
    // and then restart the device.
    case RESTART: {
        static unsigned long restart_in = millis() + 500;
        if (millis() >= restart_in) {
            ESP.restart();
        }
        break;
    }
    }
}
