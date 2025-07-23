#include <array>

#include <Arduino.h>
#include <ArduinoOTA.h>
#include <LittleFS.h>
#include <XT_DAC_Audio.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>

#include <sounds.h>
#include <memory>

#define DAC_PIN 25
#define SAMPLE_RATE 16000
#define MAX_DURATION 5 // seconds
#define SAMPLE_SIZE (SAMPLE_RATE * MAX_DURATION)
#define N_SAMPLES 3
#define BOOT_TIME 5 // seconds - failsafe if device tries to immidiately switch to normal mode

#define PROBABILITY_MAIN_SAMPLE 70 // %
#if PROBABILITY_MAIN_SAMPLE > 100 || PROBABILITY_MAIN_SAMPLE < 50
#error "PROBABILITY_MAIN_SAMPLE must be between 50 and 100"
#endif

#define SENSOR_PIN          34      // GPIO34 = ADC1_CH6 (any ADC pin works)
#define BASELINE_ALPHA      0.02f   // 0–1, lower = slower baseline
#define SPIKE_THRESHOLD     200     // ADC counts above or below baseline
#define SPIKE_MAX_MS        100     // must fall back within this time
#define SAMPLE_PERIOD_US    2000    // 500 Hz sampling
#define WAIT_AFTER_COIN_MS  100     // cooldown after a detection

#define WIFI_CONNECT_TIMEOUT 5000 // ms
#define UDP_LISTEN_PORT     12345            // Port to listen on
#define UDP_SEND_INTERVAL   20               // Send every 20ms (50Hz)

const std::string ssid = "TUDOMakerspace";
const std::string password = "SECRET";
const IPAddress static_ip(192, 168, 0, 31);

enum CoinState { IDLE, SPIKE_START, SPIKE_END };

static float     baseline        = 0;      // running average
static bool      baseline_init   = false;
static uint32_t  last_coin_ms    = 0;
static uint32_t  spike_start_ms  = 0;
static CoinState coin_state      = IDLE;

XT_DAC_Audio_Class DacAudio(DAC_PIN,0);
AsyncWebServer server(80);

std::array<File, N_SAMPLES> samples = {};           // Stores files for each sample
std::vector<uint8_t> sample_buffer;                 // Buffer to hold data of currently playing sample
std::unique_ptr<XT_Wav_Class> current_clip;         // Plays the currently selected sample
std::array<uint32_t, N_SAMPLES> probabilities = {}; // Stores probabilities for each sample

WiFiUDP udp;
IPAddress remoteIP;      // Store the IP of the last client that sent data
uint16_t remotePort = 0; // Store the port of the last client
bool udpClientConnected = false;
uint32_t last_udp_send = 0;

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

void handle_upload(unsigned int nsample, AsyncWebServerRequest *request,
                   String filename, size_t index, uint8_t *data, size_t len, bool final) {

    if (nsample >= N_SAMPLES) {
        Serial.printf("Sample %u: Rejecting upload, invalid sample number\n", nsample);
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

void play_sample(int idx)
{
    File &f = samples[idx];
    f.seek(0);
    size_t sz = f.size();

    sample_buffer.resize(sz);
    f.readBytes(reinterpret_cast<char*>(sample_buffer.data()), sz);

    current_clip.reset(new XT_Wav_Class(sample_buffer.data()));
    DacAudio.Play(current_clip.get());
}

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

void reset_samples()
{
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

void poll_coin_sensor()
{
    static uint32_t last_sample_us = 0;

    uint32_t now_us = micros();
    if (now_us - last_sample_us < SAMPLE_PERIOD_US) return;
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
        if (mode == READY) {
            server.end();
            WiFi.disconnect(true);
            WiFi.mode(WIFI_OFF);
            mode = NORMAL;
            Serial.println("First coin detected, disabling WiFi to prevent sound interference");
        }
        play_sample(pick_n_play());
        coin_state = IDLE;
        break;
    }

    if (coin_state == IDLE) {
        baseline += BASELINE_ALPHA * ((float)raw - baseline);
    }
}

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
        remoteIP = udp.remoteIP();
        remotePort = udp.remotePort();
        udpClientConnected = true;

        // Read and discard any data (we just care about the connection)
        char temp[64];
        udp.read(temp, sizeof(temp));
    }

    // Send data if we have an active client
    if (udpClientConnected) {
        uint32_t now = millis();
        if (now - last_udp_send >= UDP_SEND_INTERVAL) {
            char buffer[20];
            snprintf(buffer, sizeof(buffer), "%u\n", raw);
            udp.beginPacket(remoteIP, remotePort);
            udp.write((uint8_t*)buffer, strlen(buffer));
            udp.endPacket();
            last_udp_send = now;
        }
    }
}

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

void init_routes()
{
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

void setup() {
    Serial.begin(115200);

    pinMode(SENSOR_PIN, INPUT);
    analogReadResolution(12);              // 0‑4095
    analogSetAttenuation(ADC_11db);        // wider input range (≈0‑2.6 V)

    probabilities.fill(0); // Will be initialized later

    IPAddress gateway(192, 168, 0, 1); // Adjust as needed
    IPAddress subnet(255, 255, 255, 0); // Adjust as needed

    if (!WiFi.config(static_ip, gateway, subnet)) {
        Serial.println("Failed to configure static IP");
    }

    WiFi.begin(ssid.c_str(), password.c_str());
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
    case BOOT:
        if (millis() >= boot_done_tstamp) {
            mode = READY;
            Serial.println("Entering ready mode, waiting for first coin...");
        }
        break;
    case MEASURE:
        // Measurement mode, handle sensor readings
        measure_sensor();
        break;
    case CONFIG:
        // Config mode (for safe uploads and OTA updates)
        ArduinoOTA.handle();
        DacAudio.FillBuffer();
        break;
    case READY:
    case NORMAL:
        static bool was_playing = false;
        // Normal operation, handle coin sensor and play sounds
        if (DacAudio.AlreadyPlaying(current_clip.get())) {
            was_playing = true;
        } else if (was_playing) {
            delay(WAIT_AFTER_COIN_MS); // Wait a bit after playback ends to prevent feedback loop
            was_playing = false;
        } else {
            poll_coin_sensor();
        }
        DacAudio.FillBuffer();
        break;
    case RESTART:
        static unsigned long restart_in = millis() + 1000;
        if (millis() >= restart_in) {
            ESP.restart();
        }
        break;
    }
}
