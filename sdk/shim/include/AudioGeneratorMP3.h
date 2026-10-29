// ESP8266Audio-compatible MP3 player. The decoding itself runs in the OS (cpos_api->media).
#pragma once
#include "AudioFileSource.h"
#include "AudioOutput.h"

class AudioGeneratorMP3 {
public:
    AudioGeneratorMP3() {}
    virtual ~AudioGeneratorMP3() { release(); }

    bool begin(AudioFileSource *src, AudioOutput *out) {
        release();
        if (!src || !out) return false;
        src_ = src;
        out_ = out;
        dec_ = cpos_api->media.mp3_open();
        in_ = (uint8_t *)malloc(IN_SIZE);
        pcm_ = (int16_t *)malloc(2304 * sizeof(int16_t));
        if (!dec_ || !in_ || !pcm_) { release(); return false; }
        in_ptr_ = in_;
        in_left_ = 0;
        eof_ = false;
        npcm_ = ipcm_ = 0;
        out_->SetBitsPerSample(16);
        if (!out_->begin()) { release(); return false; }
        running_ = true;
        return true;
    }

    // Feeds the output until it is full; false once the song is over.
    bool loop() {
        if (!running_) return false;
        for (;;) {
            while (ipcm_ < npcm_) {
                int16_t s[2];
                s[0] = pcm_[ipcm_];
                s[1] = chans_ == 2 ? pcm_[ipcm_ + 1] : s[0];
                if (!out_->ConsumeSample(s)) return true;
                ipcm_ += chans_;
            }
            int r = decode();
            if (r < 0) { running_ = false; return false; }
            if (r == 0) return true;
        }
    }

    bool stop() {
        running_ = false;
        if (out_) out_->stop();
        release();
        return true;
    }
    bool isRunning() { return running_; }

private:
    static const int IN_SIZE = 4096;

    void release() {
        if (dec_) cpos_api->media.mp3_close(dec_);
        free(in_);
        free(pcm_);
        dec_ = nullptr;
        in_ = nullptr;
        pcm_ = nullptr;
    }

    void fill() {
        if (in_left_ && in_ptr_ != in_) memmove(in_, in_ptr_, in_left_);
        in_ptr_ = in_;
        if (eof_) return;
        uint32_t n = src_->read(in_ + in_left_, IN_SIZE - in_left_);
        if (!n) eof_ = true;
        in_left_ += n;
    }

    // 1 = new samples, 0 = nothing yet, -1 = end of the song
    int decode() {
        for (int tries = 0; tries < 16; tries++) {
            if (in_left_ < IN_SIZE / 2 && !eof_) fill();
            if (in_left_ <= 0) return -1;
            int off = cpos_api->media.mp3_sync(in_ptr_, in_left_);
            if (off < 0) { /* no frame header here: keep the tail, it may be half of one */
                int keep = in_left_ > 3 ? 3 : in_left_;
                in_ptr_ += in_left_ - keep;
                in_left_ = keep;
                if (eof_) return -1;
                fill();
                continue;
            }
            in_ptr_ += off;
            in_left_ -= off;
            if (in_left_ < IN_SIZE / 2 && !eof_) fill();
            const uint8_t *p = in_ptr_;
            int left = in_left_;
            cpos_mp3_info_t info;
            int rc = cpos_api->media.mp3_decode(dec_, &p, &left, pcm_, &info);
            if (rc == 0) {
                in_ptr_ = (uint8_t *)p;
                in_left_ = left;
                if (info.rate != rate_) { rate_ = info.rate; out_->SetRate(rate_); }
                if (info.channels != chans_) { chans_ = info.channels; out_->SetChannels(chans_); }
                if (chans_ < 1) chans_ = 1;
                npcm_ = info.samples;
                ipcm_ = 0;
                return 1;
            }
            if (rc == -1) { /* frame cut short */
                if (eof_) return -1;
                fill();
            } else if (rc == -2) { /* first frames after a seek: decoder still needs earlier data */
                in_ptr_ = (uint8_t *)p;
                in_left_ = left;
            } else { /* damaged frame: step over its header */
                in_ptr_++;
                in_left_--;
            }
        }
        return 0;
    }

    AudioFileSource *src_ = nullptr;
    AudioOutput *out_ = nullptr;
    void *dec_ = nullptr;
    uint8_t *in_ = nullptr, *in_ptr_ = nullptr;
    int16_t *pcm_ = nullptr;
    int in_left_ = 0, npcm_ = 0, ipcm_ = 0, rate_ = 0, chans_ = 2;
    bool eof_ = false, running_ = false;
};
