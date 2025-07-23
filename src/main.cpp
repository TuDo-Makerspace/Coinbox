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

///////////////////////////////////////////////////////////////////////////////
// Coin Detection Globals
///////////////////////////////////////////////////////////////////////////////

enum CoinState { IDLE, SPIKE_START, SPIKE_END };

static float     baseline        = 0;       // running average
static bool      baseline_init   = false;   // whether baseline has been initialized
static uint32_t  spike_start_ms  = 0;       // timestamp when spike started
static CoinState coin_state      = IDLE;    // current state of coin detection state machine

/////////////////////////////////////////////////////////////////////////////////
// Audio Globals
/////////////////////////////////////////////////////////////////////////////////

XT_DAC_Audio_Class DacAudio(DAC_PIN,0);             // DAC audio output class
std::array<File, N_SAMPLES> samples = {};           // Stores files for each sample
std::vector<uint8_t> sample_buffer;                 // Buffer to hold data of currently playing sample
std::unique_ptr<XT_Wav_Class> current_clip;         // Plays the currently selected sample
std::array<uint32_t, N_SAMPLES> probabilities = {}; // Stores probabilities for each sample

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
    READY,
    MEASURE,
    CONFIG,
    NORMAL,
    RESTART
};

device_mode mode = BOOT;
unsigned long boot_done_tstamp;

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

    Serial.println("Probabilities initialised:");
    for (int i = 0; i < N_SAMPLES; ++i) {
        Serial.printf("\tSample %d: %u%%\n", i, probabilities[i]);
    }
}

// Initialize/Load samples from LittleFS or create default ones if they don't exist
void init_samples() {
    for (int i = 0; i < N_SAMPLES; ++i) {
        String filename = "/" + String(i) + ".wav";

        // Sample exists
        if (LittleFS.exists(filename)) {
            samples[i] = LittleFS.open(filename, "r");
            if (!samples[i]) {
                Serial.printf("Failed to open %s\n", filename.c_str());
            } else {
                Serial.printf("Loaded sample %d from %s\n", i, filename.c_str());
            }
        }

        // Sample does not exist, create with default sound
        else {
            Serial.printf("Sample %d missing\n", i);

            // Load coin sound as default
            samples[i] = LittleFS.open(filename, "w");
            if (samples[i]) {
                switch(i) {
                case 1:
                    samples[i].write(powerup, sizeof(powerup));
                    break;
                case 2:
                    samples[i].write(oneup, sizeof(oneup));
                    break;
                default:
                    samples[i].write(coin, sizeof(coin)); // Default to coin sound
                    break;

                }
                samples[i].close();
                Serial.printf("Using default coin sound for sample %d\n", i);
            } else {
                Serial.printf("FATAL: Failed to create sample %d\n", i);
                while(true);
            }
        }
    }
}

// Handle file uploads for samples
void handle_upload(unsigned int nsample, AsyncWebServerRequest *request,
                   String filename, size_t index, uint8_t *data, size_t len, bool final) {

    if (nsample >= N_SAMPLES) {
        Serial.printf("Sample %u: Rejecting upload, invalid sample number (max %d)\n", nsample, N_SAMPLES - 1);
    }

    // First chunk
    if (index == 0) {
        size_t left = LittleFS.totalBytes() - LittleFS.usedBytes();
        if (request->contentLength() > SAMPLE_SIZE ||
                request->contentLength() > left) {
            request->send(507, "text/plain", "Sample exceeds 5s\n");
            Serial.printf("Sample %u: Rejected upload, too large (%u B)\n", nsample, request->contentLength());
            return;
        }

        Serial.printf("Sample %u: Uploading %s (%u B)\n",
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
        Serial.printf("Sample %u: Upload complete\n", nsample);

        samples[nsample] = LittleFS.open("/" + String(nsample) + ".wav", "r");
        if (!samples[nsample]) {
            Serial.printf("Sample %u: Failed to open uploaded file\n", nsample);
        }

        request->send(200, "text/plain", "Sample uploaded successfully\n");
    }
}

// Reset samples to factory defaults
void reset_samples() {
    Serial.println("Factory reset: resetting samples to defaults...");

    for (int i = 0; i < N_SAMPLES; ++i) {
        String fn = "/" + String(i) + ".wav";

        if (samples[i]) samples[i].close();
        LittleFS.remove(fn); // ensure truncate

        File f = LittleFS.open(fn, "w");
        if (!f) {
            Serial.printf("Failed to open %s for writing\n", fn.c_str());
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
        samples[i] = LittleFS.open(fn, "r");
        if (!samples[i]) {
            Serial.printf("Failed to reopen %s\n", fn.c_str());
        }
    }
}

// Play a sample by index
void play_sample(int idx) {
    File &f = samples[idx];
    f.seek(0);
    size_t sz = f.size();

    sample_buffer.resize(sz);
    f.readBytes(reinterpret_cast<char*>(sample_buffer.data()), sz);

    current_clip.reset(new XT_Wav_Class(sample_buffer.data()));
    DacAudio.Play(current_clip.get());
}

// Pick a random sample based on probabilities
unsigned int pick_n_play() {
    if (probabilities[0] == 0) {
        Serial.println("Probabilities not initialized!");
        init_prob();
    }

    unsigned int r = random(100);
    unsigned int cumulative = 0;
    for (unsigned int i = 0; i < N_SAMPLES; ++i) {
        cumulative += probabilities[i];
        if (r < cumulative) {
            Serial.printf("Playing sample %u\n", i);
            return i;
        }
    }

    // Should not happen, but just in case
    Serial.println("Warning: Failed to pick sample, falling back to first sample");
    return 0;
}

/////////////////////////////////////////////////////////////////////////////////
// Coin Detection Functions
/////////////////////////////////////////////////////////////////////////////////

// Poll the coin sensor and handle coin detection logic
bool poll_coin_sensor() {
    static uint32_t last_sample_us = 0;
    bool coin_hit = false;

    uint32_t now_us = micros();
    if (now_us - last_sample_us < SAMPLE_PERIOD_US) {
        return false;
    }
    last_sample_us = now_us;

    uint16_t raw = analogRead(SENSOR_PIN);

    if (!baseline_init) {
        baseline = raw;
        baseline_init = true;
    }

    int16_t diff = (int16_t)raw - (int16_t)baseline;

    switch (coin_state) {
    case IDLE:
        if (abs(diff) > SPIKE_THRESHOLD) {
            coin_state     = SPIKE_START;
            spike_start_ms = millis();
        }
        break;

    case SPIKE_START:
        if (abs(diff) < SPIKE_THRESHOLD) {
            coin_state = SPIKE_END;
        } else if (millis() - spike_start_ms > SPIKE_MAX_MS) {
            coin_state = IDLE;
            baseline   = raw;
        }
        break;

    case SPIKE_END:
        coin_hit = true; 
        coin_state = IDLE;
        break;
    }

    if (coin_state == IDLE) {
        baseline += BASELINE_ALPHA * ((float)raw - baseline);
    }

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
        remote_ip = udp.remote_ip();
        remote_port = udp.remote_port();
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
        Serial.println("mDNS host name coinbox.local registered");
    } else {
        Serial.println("Failed to register mDNS host name");
        return;
    }

    // Register HTTP service for device configuration
    if (MDNS.addService("http", "tcp", 80)) {
        Serial.println("mDNS service _http._tcp. registered on port 80");
    } else {
        Serial.println("Failed to register mDNS HTTP service");
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
            if (samples[sample]) {
                play_sample(sample);
                request->send(200, "text/plain", "Playing sample " + String(sample) + "\n");
            } else {
                request->send(404, "text/plain", "Sample not found\n");
            }
        });
    }

    server.on("/measure", HTTP_GET, [](AsyncWebServerRequest *request) {
        Serial.println("Entering measurement mode...");
        request->send(200, "text/plain", "Entering measurement mode...\n");
        udp.begin(UDP_LISTEN_PORT);
        Serial.printf("UDP server started on port %d\n", UDP_LISTEN_PORT);
        mode = MEASURE;
    });

    server.on("/config", HTTP_GET, [](AsyncWebServerRequest *request) {
        Serial.println("Entering config mode...");
        request->send(200, "text/plain", "Entering Config mode...\n");
        ArduinoOTA.begin();
        mode = CONFIG;
    });

    server.on("/restart", HTTP_GET, [](AsyncWebServerRequest *request) {
        Serial.println("Restarting device...");
        request->send(200, "text/plain", "Restarting...\n");
        ArduinoOTA.end();
        udp.stop();
        mode = RESTART; // Signal to restart
    });

    server.on("/reset", HTTP_GET, [](AsyncWebServerRequest *request) {
        Serial.println("Resetting samples to factory defaults...");
        request->send(200, "text/plain", "Resetting samples...\n");
        reset_samples();
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

    if (!WiFi.config(STATIC_IP, gateway, subnet)) {
        Serial.println("Failed to configure static IP");
    }

    WiFi.begin(SSID, PASSWORD);
    unsigned long tout_start = millis();
    bool fail = false;

    Serial.println("Connecting to WiFi...");

    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - tout_start >= WIFI_CONNECT_TIMEOUT) {
            fail = true;
            return;
        }
    }

    if (fail) {
        Serial.println("WiFi connection timeout, continuing without connection...");
    } else {
        Serial.println("Connected to WiFi");
        Serial.println("IP Address: " + WiFi.localIP().toString());
    }

    if (!LittleFS.begin(true)) {
        Serial.println("FATAL: LittleFS mount failed");
        while(true);
    }

    init_samples();
    init_routes();
    init_prob();
    server.begin();
    expose_mDNS();

    boot_done_tstamp = millis() + BOOT_TIME * 1000;
    Serial.println("Entering boot mode, ignoring sensor input for " + String(BOOT_TIME) + " seconds");
}

void loop() {
    switch(mode) {
    
    /* Boot Mode:
     * Waits for BOOT_TIME to elapse, providing a guaranteed time window
     * during which the device can be put into config mode.
     * This is a failsafe that prevents the device from immediately switching to normal mode after
     * boot, which could happen due to unexpected sensor behavior or misconfigured detection parameters.
     */
    case BOOT:
        if (millis() >= boot_done_tstamp) {
            mode = READY;
            Serial.println("Entering ready mode, waiting for first coin...");
        }
        break;
    
    /* Measure Mode:
     * Activated via a GET request to /measure.
     * Allows measurement of sensor values via serial (cable) and UDP (wirelessly).
     * Used for debugging and calibration.
     */
    case MEASURE:
        measure_sensor();
        break;

    /* Config Mode:
     * Activated via a GET request to /config.
     * Enables safe uploading of new samples and OTA updates.
     * Sound playback is disabled in this mode.
     * The device remains in config mode until explicitly restarted,
     * e.g., by sending a GET request to /restart.
     */
    case CONFIG:
        ArduinoOTA.handle();
        DacAudio.FillBuffer();
        break;
    
    /* Ready/Normal Mode:
     * Normal operation mode, where the device waits for the first coin.
     * After the first coin is detected, it switches to normal mode.
     * In this mode, the device handles coin detection and plays sounds.
     * If a sound is already playing, it waits for COOLDOWN before processing new coins.
     */
    case READY:
    case NORMAL:
        static bool was_playing = false;

        // Block while playing a sound
        if (DacAudio.AlreadyPlaying(current_clip.get())) {
            was_playing = true;
        }
        
        // Cooldown after playback to feedback from speaker-induced electrical noise or vibration
        else if (was_playing) {
            delay(COOLDOWN);
            was_playing = false;
        }
        
        // Poll the coin sensor
        else if (poll_coin_sensor()) {
            play_sample(pick_n_play());

            // WiFi interferes with audio playback, so disable it after the first coin
            if (mode == READY) {
                server.end();
                WiFi.disconnect(true);
                WiFi.mode(WIFI_OFF);
                mode = NORMAL;
                Serial.println("First coin detected, disabling WiFi to prevent sound interference");
            }
        }

        DacAudio.FillBuffer();
        break;
    
    // Restart Signaled! Give time to finish any ongoing tasks
    // and then restart the device.
    case RESTART:
        static unsigned long restart_in = millis() + 500;
        if (millis() >= restart_in) {
            ESP.restart();
        }
        break;
    }
}
