#pragma once
#include <stdint.h>

// USB diagnostics follow Arduino IDE Tools > Debug port (Serial / Disabled).
// Optional override for either build system: uncomment and set 0 or 1.
// An explicit value takes precedence over the IDE menu. Leave commented for auto.
// #define TAMBOX_DEBUG_ENABLED 1

// NodeMCU 1.0 / ESP-12E or ESP-12F, 4 MB flash. These are GPIO numbers!
constexpr uint8_t TAMBOX_SDA = 4; // D2
constexpr uint8_t TAMBOX_SCL = 5; // D1
#ifndef TAMBOX_LCD_ADDRESS
#define TAMBOX_LCD_ADDRESS 0x27
#endif
#ifndef TAMBOX_KEYPAD_ADDRESS
#define TAMBOX_KEYPAD_ADDRESS 0x20
#endif
// PCF8574: P0..P3 = C1..C4, P4..P7 = R1..R4. Change these arrays to match
// the actual module. Values are PCF pin numbers, NOT NodeMCU GPIO numbers.
constexpr uint8_t TAMBOX_KEYPAD_ROWS[4] = {4, 5, 6, 7};
constexpr uint8_t TAMBOX_KEYPAD_COLS[4] = {0, 1, 2, 3};
static_assert(TAMBOX_LCD_ADDRESS != TAMBOX_KEYPAD_ADDRESS, "LCD and keypad need different I2C addresses");
constexpr char TAMBOX_KEYS[] = "123A456B789C*0#D";
constexpr char TAMBOX_FIRMWARE_VERSION[] = "0.7.7";
constexpr char TAMBOX_MODEL[] = "NodeMCU ESP8266 PCF8574";
