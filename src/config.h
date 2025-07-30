#pragma once

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

// Configuration for the TuDo Makerspace Coinbox Firmware

///////////////////////////////////////////////////////////////////////////////
// WiFi
///////////////////////////////////////////////////////////////////////////////

#define SSID "TUDOMakerspace"
#define PASSWORD "SECRET"
#define STATIC_IP 192, 168, 0, 31
#define WIFI_CONNECT_TIMEOUT 5000 // ms
#define BOOT_TIME 2
#define REACTIVATE_WIFI_AFTER 10000 // ms

///////////////////////////////////////////////////////////////////////////////
// Configuration
///////////////////////////////////////////////////////////////////////////////

#define CONFIG_TIMEOUT 1800000 // ms (30 minutes)

///////////////////////////////////////////////////////////////////////////////
// Audio
///////////////////////////////////////////////////////////////////////////////

#define DAC_PIN 25                                  // Pin used for audio output
#define SAMPLE_RATE 16000                           // Sample rate (only used for sample size calculation)
#define MAX_DURATION 5                              // Maximum duration of a sample in seconds
#define SAMPLE_SIZE (SAMPLE_RATE * MAX_DURATION)    // Maximum sample size in bytes (16000 samples * 2 bytes/sample = 32000 bytes)
#define N_SAMPLES 3                                 // Number of samples (probability decreases with higher index)
#define PROBABILITY_MAIN_SAMPLE 70                  // Probability of the main sample (sample 0). Remaining probability is distributed among the other samples.
#define COOLDOWN 10                                 // Wait time after playback ends to prevent feedback loop
#define ADC_IGNORE_ABOVE
#define BLOCK_AFTER_LID_OPEN 2000 // ms

// DO NOT EDIT: Sanity check for PROBABILITY_MAIN_SAMPLE
#if PROBABILITY_MAIN_SAMPLE > 100 || PROBABILITY_MAIN_SAMPLE < 50
#error "PROBABILITY_MAIN_SAMPLE must be between 50 and 100"
#endif

///////////////////////////////////////////////////////////////////////////////
// Sensor and Coin Detection
///////////////////////////////////////////////////////////////////////////////

#define SENSOR_PIN          34      // ADC pin for sensor
#define SPIKE_THRESHOLD     100     // Minimum ADC deviation from baseline to register a spike
#define SPIKE_MAX_MS        90      // Spike must return to baseline within this time to count as a coin
#define SAMPLE_PERIOD_US    2000    // Sensor sampling interval (500 Hz)
#define LOW_THRESHOLD       7
#define HIGH_THRESHOLD      750
#define ADC_SAMPLES         4

const float BASELINE_ALPHA = 0.02f;   // Baseline smoothing factor (0–1); lower = slower adaptation

///////////////////////////////////////////////////////////////////////////////
// Debugging
///////////////////////////////////////////////////////////////////////////////

#define UDP_LISTEN_PORT     12345   // Port to listen on
#define UDP_SEND_INTERVAL   20      // Send every 20ms (50Hz)

#define LOG_ENTRIES 150         // how many recent lines to keep
#define LOG_ENTRY_LEN 128       // max chars per line (longer lines are truncated)
#define LOG_ADC_VALUES 2000     // how many recent ADC values to keep
#define LOG_ADC_AVG_VALUES 2000 // how many recent averaged ADC values to keep