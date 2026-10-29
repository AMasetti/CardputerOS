// ESP8266Audio-compatible output base class.
#pragma once
#include "Arduino.h"

class AudioOutput {
public:
    AudioOutput() {}
    virtual ~AudioOutput() {}
    virtual bool SetRate(int hz) { hertz = hz; return true; }
    virtual bool SetBitsPerSample(int bits) { bps = bits; return true; }
    virtual bool SetChannels(int ch) { channels = ch; return true; }
    virtual bool SetGain(float f) { gainF2P6 = (uint8_t)(f * 64.0f); return true; }
    virtual bool begin() { return false; }
    virtual bool ConsumeSample(int16_t sample[2]) = 0;
    virtual uint16_t ConsumeSamples(int16_t *samples, uint16_t count) {
        for (uint16_t i = 0; i < count; i++)
            if (!ConsumeSample(samples + 2 * i)) return i;
        return count;
    }
    virtual bool stop() { return false; }
    virtual void flush() {}
    virtual bool loop() { return true; }

protected:
    uint16_t hertz = 44100;
    uint8_t bps = 16;
    uint8_t channels = 2;
    uint8_t gainF2P6 = 64;
};
