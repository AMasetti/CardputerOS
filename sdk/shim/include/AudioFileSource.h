// ESP8266Audio-compatible input base class.
#pragma once
#include "Arduino.h"

class AudioFileSource {
public:
    AudioFileSource() {}
    virtual ~AudioFileSource() {}
    virtual bool open(const char *) { return false; }
    virtual uint32_t read(void *data, uint32_t len) = 0;
    virtual uint32_t readNonBlock(void *data, uint32_t len) { return read(data, len); }
    virtual bool seek(int32_t, int) { return false; }
    virtual bool close() { return false; }
    virtual bool isOpen() { return false; }
    virtual uint32_t getSize() { return 0; }
    virtual uint32_t getPos() { return 0; }
    virtual bool loop() { return true; }
};
