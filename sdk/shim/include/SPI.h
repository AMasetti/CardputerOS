#pragma once
#include "Arduino.h"
// The OS owns the SPI buses. Apps keep calling begin() for source compatibility.
class SPIClass {
public:
    void begin(int = -1, int = -1, int = -1, int = -1) {}
    void end() {}
    uint8_t transfer(uint8_t v) { return v; }
};
extern SPIClass SPI;
