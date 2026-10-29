// Reads another source in large blocks (SD cards are much faster that way).
#pragma once
#include "AudioFileSource.h"

class AudioFileSourceBuffer : public AudioFileSource {
public:
    AudioFileSourceBuffer(AudioFileSource *src, uint32_t size) : src_(src), size_(size) { buf_ = (uint8_t *)malloc(size); }
    ~AudioFileSourceBuffer() override { free(buf_); }
    uint32_t read(void *data, uint32_t len) override {
        uint8_t *d = (uint8_t *)data;
        uint32_t done = 0;
        while (done < len) {
            if (pos_ == len_) {
                if (!buf_ || len - done >= size_) { /* large reads skip the buffer */
                    uint32_t n = src_->read(d + done, len - done);
                    return done + n;
                }
                pos_ = 0;
                len_ = src_->read(buf_, size_);
                if (!len_) break;
            }
            uint32_t n = len_ - pos_;
            if (n > len - done) n = len - done;
            memcpy(d + done, buf_ + pos_, n);
            pos_ += n;
            done += n;
        }
        return done;
    }
    bool seek(int32_t pos, int dir) override {
        if (dir == SEEK_CUR) pos -= (int32_t)(len_ - pos_);
        pos_ = len_ = 0;
        return src_->seek(pos, dir);
    }
    bool close() override { return src_->close(); }
    bool isOpen() override { return src_->isOpen(); }
    uint32_t getSize() override { return src_->getSize(); }
    uint32_t getPos() override { return src_->getPos() - (len_ - pos_); }

private:
    AudioFileSource *src_;
    uint32_t size_;
    uint8_t *buf_;
    uint32_t pos_ = 0, len_ = 0;
};
