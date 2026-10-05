/*
   I2cBus.h - Shared I2C bus (PCF8583 and OLED display)

   Wire is not thread-safe: every task that uses the bus takes the lock first.
   The lock is recursive, so a function holding it may call others that take it.
*/
#ifndef I2CBUS_H
#define I2CBUS_H

#include <stdint.h>

/// Start the bus (safe to call more than once)
void i2cBegin();

void i2cLock();
void i2cUnlock();

/// True when a device answers on this address
bool i2cProbe(uint8_t addr);

#endif
