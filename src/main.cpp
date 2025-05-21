/*
 * main.cpp
 *
 * Author       : Yunis, Patrick Pedersen
 * Description  : Mario Coin Box
 *                A red-LED / photodiode pair monitors the coin slot and delivers
 *                an analog signal to the MCU.  The firmware continually tracks a
 *                running baseline via a low-pass filter, then looks for short-
 *                duration spikes above that baseline to identify passing coins.
 *                Each coin increments an internal counter and triggers the
 *                appropriate Mario sound effect (coin, power-up, 1-up, …)
 *                according to how many coins have been collected so far.
 *
 * 			  	  Measurements of the coin slot signal can be found in the measurements
 *				  directory of this repository.
 */

#include <Arduino.h>
#include <PCM.h>

#include "sounds.h"

// Pins
// Sound is on pin 11
#define SENSOR_PIN A7

// Number of coins to trigger sound
#define POWER_UP 4
#define ONE_UP 10

// Function
#define WAIT_AFTER_COIN 100 // ms

// Filter parameters
#define BASELINE_ALPHA 0.02f  	// 0 - 1  (smaller = slower baseline)
#define SPIKE_THRESHOLD 100   	// ADC counts above baseline
#define SPIKE_MAX_MS 100      	// must fall back within this time
#define SAMPLE_PERIOD_US 2000 	// ~500 Hz sampling
#define WAIT_AFTER_COIN 100 	// coolwdown time after coin detection

// State machine
enum State {
    IDLE,
    SPIKE_START,
    SPIKE_END,
};

void print_boot_msg() {
    Serial.println("@@@@/////////////////////////////////////////////////@@@@\n");
    Serial.println("////,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,@@@@\n");
    Serial.println("////,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,@@@@\n");
    Serial.println("////,,,@@@@,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,@@@@,,,@@@@\n");
    Serial.println("////,,,,,,,,,,,,,,/////////////////*,,,,,,,,,,,,,,,,,@@@@\n");
    Serial.println("////,,,,,,,,,,,,,,/////////////////*,,,,,,,,,,,,,,,,,@@@@\n");
    Serial.println("////,,,,,,,,,,///////@@@@@@@@@@@///////,,,,,,,,,,,,,,@@@@\n");
    Serial.println("////,,,,,,,,,,///////@@@@,,,,,,,///////@@@@,,,,,,,,,,@@@@\n");
    Serial.println("////,,,,,,,,,,///////@@@@,,,,,,,///////@@@@,,,,,,,,,,@@@@\n");
    Serial.println("////,,,,,,,,,,///////@@@@,,,,,,,///////@@@@,,,,,,,,,,@@@@\n");
    Serial.println("////,,,,,,,,,,,,,,@@@@@@@,,,///////////@@@@,,,,,,,,,,@@@@\n");
    Serial.println("////,,,,,,,,,,,,,,,,,,,,,///////@@@@@@@@@@@,,,,,,,,,,@@@@\n");
    Serial.println("////,,,,,,,,,,,,,,,,,,,,,///////@@@@@@@@@@@,,,,,,,,,,@@@@\n");
    Serial.println("////,,,,,,,,,,,,,,,,,,,,,///////@@@#,,,,,,,,,,,,,,,,,@@@@\n");
    Serial.println("////,,,,,,,,,,,,,,,,,,,,,,,,@@@@@@@#,,,,,,,,,,,,,,,,,@@@@\n");
    Serial.println("////,,,,,,,,,,,,,,,,,,,,,///////,,,,,,,,,,,,,,,,,,,,,@@@@\n");
    Serial.println("////,,,,,,,,,,,,,,,,,,,,,///////,,,,,,,,,,,,,,,,,,,,,@@@@\n");
    Serial.println("////,,,,,,,,,,,,,,,,,,,,,///////@@@#,,,,,,,,,,,,,,,,,@@@@\n");
    Serial.println("////,,,@@@@,,,,,,,,,,,,,,,,,@@@@@@@#,,,,,,,,,,@@@@,,,@@@@\n");
    Serial.println("////,,,@@@@,,,,,,,,,,,,,,,,,@@@@@@@#,,,,,,,,,,@@@@,,,@@@@\n");
    Serial.println("////,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,@@@@\n");
    Serial.println("@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@\n");
    Serial.println("\n\n");
    Serial.println("Mario Coin box by Yunis! ft. Patrick :) !!!!11!\n");
}

void setup() {
    pinMode(SENSOR_PIN, INPUT);
    Serial.begin(9600);
    print_boot_msg();
}

void loop() {
    // Runtime variables
    static float baseline = 0;         		// running baseline (float for filter)
    static bool baseline_init = false; 		// first-run flag
    static int money = 1;              		// counter for sounds
    static State     state         = IDLE;	// state machine
    static uint32_t  spike_start   = 0;		// time of spike start
    static uint32_t  last_coin_ms  = 0;		// time of last coin detection

    uint16_t raw = analogRead(SENSOR_PIN);

    if (!baseline_init) {
        baseline = raw;
        baseline_init = true;
    }

    // Deviation from baseline
    int16_t diff = (int16_t)raw - (int16_t)baseline;

    switch (state) {
    case IDLE:
        if (abs(diff) > SPIKE_THRESHOLD) {
            state       = SPIKE_START;
            spike_start = millis();
        }
        break;

    case SPIKE_START:
        if (abs(diff) < SPIKE_THRESHOLD) {
            state = SPIKE_END;                     // Spike ended, this is a coin!
        } else if ((millis() - spike_start) > SPIKE_MAX_MS) {
            state    = IDLE;                       // Probably just a change in light, not a coin...
            baseline = raw;
        }
        break;

    case SPIKE_END:
        if ((millis() - last_coin_ms) > WAIT_AFTER_COIN) { // cooldown
            Serial.println(F("Coin detected!"));
            if ((money % POWER_UP) == 0) {
                Serial.println(F("Power-up!"));
                startPlayback(powerup, sizeof(powerup));
            } else if ((money % ONE_UP) == 0) {
                Serial.println(F("One-up!"));
                startPlayback(oneup, sizeof(oneup));
            } else {
                Serial.println(F("Coin sound!"));
                startPlayback(coin, sizeof(coin));
            }
            money++;
            last_coin_ms = millis();
        }
        state = IDLE;
        break;
    }

    if (state == IDLE) {
        baseline += BASELINE_ALPHA * ((float)raw - baseline);
    }

    delayMicroseconds(SAMPLE_PERIOD_US);
}