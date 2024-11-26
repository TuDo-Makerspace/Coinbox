/*
 * main.cpp
 *
 * Author       : Yunis, Patrick Pedersen
 * Description  : Main code for the coin box
 *
 */

#include <Arduino.h>
#include <PCM.h>

#include "sounds.h"

//// Parameters

// Pins
// Sound is on pin 11
#define SENSOR_PIN A7

// Number of coins to trigger sound
#define POWER_UP 4
#define ONE_UP 10

// Function
#define WAIT_AFTER_COIN 100 // ms

/* From: https://docs.arduino.cc/language-reference/en/functions/digital-io/digitalread/:
 *   The analog input pins can be used as digital pins, referred to as A0, A1, etc.
 *   The exception is the Arduino Nano, Arduino Pro Mini, and Arduino Mini’s A6 and A7 pins,
 *   which can only be used as analog inputs. */
#if SENSOR_PIN == A6 || SENSOR_PIN == A7
    #define SENSOR_THRESHOLD 512
    #define SENSOR_READ() (analogRead(SENSOR_PIN) > SENSOR_THRESHOLD ? HIGH : LOW)
#else
    #define SENSOR_READ() digitalRead(SENSOR_PIN)
#endif

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

int money = 1;

void setup() {
  pinMode(SENSOR_PIN, INPUT);
  Serial.begin(9600);
  print_boot_msg();
}

void loop() {
  static uint8_t last_val = SENSOR_READ();
  uint8_t val = SENSOR_READ();

  if (last_val == LOW && val == HIGH) {

    Serial.print("Coin detected! Playing: ");

    if ((money % POWER_UP) == 0) {
      Serial.println("Power up!");
      startPlayback(powerup, sizeof(powerup));
    } else if ((money % ONE_UP) == 0) {
      Serial.println("One up!");
      startPlayback(oneup, sizeof(oneup));
    } else {
      Serial.println("Coin sound!");
      startPlayback(coin, sizeof(coin));
    }
    money++;

    delay(WAIT_AFTER_COIN);
  }

  last_val = val;
}
