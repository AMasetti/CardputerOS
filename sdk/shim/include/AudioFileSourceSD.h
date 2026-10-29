// A file on the SD card as an audio source.
#pragma once
#include "AudioFileSource.h"
#include "SD.h"

class AudioFileSourceSD : public AudioFileSource {
public:
    AudioFileSourceSD() {}
    explicit AudioFileSourceSD(const char *path) { open(path); }
    ~AudioFileSourceSD() override { close(); }
    bool open(const char *path) override { f_ = SD.open(path); return (bool)f_; }
    uint32_t read(void *data, uint32_t len) override { return f_ ? f_.read((uint8_t *)data, len) : 0; }
    bool seek(int32_t pos, int dir) override { return f_ && f_.seek(pos, dir); }
    bool close() override { f_.close(); return true; }
    bool isOpen() override { return (bool)f_; }
    uint32_t getSize() override { return f_ ? f_.size() : 0; }
    uint32_t getPos() override { return f_ ? f_.position() : 0; }

private:
    File f_;
};
