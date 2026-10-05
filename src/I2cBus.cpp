/*
   I2cBus.cpp - Shared I2C bus (PCF8583 and OLED display)
*/
#include "I2cBus.h"

#include <Arduino.h>
#include <Wire.h>
#include "config.h"

static SemaphoreHandle_t s_lock = NULL;

/// Bus recovery: a reset in the middle of a read can leave a device holding
/// SDA low while it waits for the rest of its byte. Clock SCL until it lets
/// go, then send a STOP.
static void i2cBusClear() {
  pinMode(PIN_I2C_SDA, INPUT_PULLUP);
  pinMode(PIN_I2C_SCL, INPUT_PULLUP);
  delayMicroseconds(10);
  if (digitalRead(PIN_I2C_SDA) == HIGH) return;   // bus is free

  Serial.println("[I2C] SDA held low, clearing the bus");
  pinMode(PIN_I2C_SCL, OUTPUT_OPEN_DRAIN);
  for (int i = 0; i < 9 && digitalRead(PIN_I2C_SDA) == LOW; i++) {
    digitalWrite(PIN_I2C_SCL, LOW);
    delayMicroseconds(10);
    digitalWrite(PIN_I2C_SCL, HIGH);
    delayMicroseconds(10);
  }
  // STOP: SDA rises while SCL is high
  pinMode(PIN_I2C_SDA, OUTPUT_OPEN_DRAIN);
  digitalWrite(PIN_I2C_SDA, LOW);
  delayMicroseconds(10);
  digitalWrite(PIN_I2C_SCL, HIGH);
  delayMicroseconds(10);
  digitalWrite(PIN_I2C_SDA, HIGH);
  delayMicroseconds(10);
  pinMode(PIN_I2C_SDA, INPUT_PULLUP);
  pinMode(PIN_I2C_SCL, INPUT_PULLUP);
}

void i2cBegin() {
  if (s_lock != NULL) return;
  s_lock = xSemaphoreCreateRecursiveMutex();
  i2cBusClear();
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, I2C_CLOCK_HZ);

  // Log what is on the bus, to help with wiring problems
  Serial.printf("[I2C] SDA %s, SCL %s, devices:", digitalRead(PIN_I2C_SDA) ? "high" : "LOW",
                digitalRead(PIN_I2C_SCL) ? "high" : "LOW");
  int found = 0;
  for (uint8_t addr = 0x08; addr < 0x78; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.printf(" 0x%02X", addr);
      found++;
    }
  }
  Serial.println(found ? "" : " none");
}

void i2cLock() {
  xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
}

void i2cUnlock() {
  xSemaphoreGiveRecursive(s_lock);
}

bool i2cProbe(uint8_t addr) {
  i2cLock();
  Wire.beginTransmission(addr);
  bool found = Wire.endTransmission() == 0;
  i2cUnlock();
  return found;
}
