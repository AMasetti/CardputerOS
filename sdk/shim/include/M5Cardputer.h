// M5Cardputer / M5GFX source-compatible layer. Every call goes to the OS through cpos_api.
#pragma once
#include <string>
#include <vector>
#include "Arduino.h"

#define GFXAPI (cpos_api->gfx)

enum textdatum_t : uint8_t {
    top_left = 0, top_center = 1, top_right = 2,
    middle_left = 4, middle_center = 5, middle_right = 6,
    bottom_left = 8, bottom_center = 9, bottom_right = 10,
    baseline_left = 16, baseline_center = 17, baseline_right = 18,
};

namespace lgfx {
struct IFont { int id; };
inline uint16_t color565(uint8_t r, uint8_t g, uint8_t b) { return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)); }
struct rgb565_t { uint16_t raw; };
struct swap565_t { uint16_t raw; };
inline uint32_t color888(uint8_t r, uint8_t g, uint8_t b) { return 0x1000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b; }
}  // namespace lgfx

namespace fonts {
inline constexpr lgfx::IFont Font0{CPOS_FONT0};
inline constexpr lgfx::IFont Font2{CPOS_FONT2};
inline constexpr lgfx::IFont Font4{CPOS_FONT4};
inline constexpr lgfx::IFont Font7{CPOS_FONT7};
inline constexpr lgfx::IFont DejaVu18{CPOS_FONT_DEJAVU18};
inline constexpr lgfx::IFont DejaVu24{CPOS_FONT_DEJAVU24};
inline constexpr lgfx::IFont DejaVu40{CPOS_FONT_DEJAVU40};
inline constexpr lgfx::IFont DejaVu56{CPOS_FONT_DEJAVU56};
inline constexpr lgfx::IFont DejaVu72{CPOS_FONT_DEJAVU72};
inline constexpr lgfx::IFont lgfxJapanGothicP_12{CPOS_FONT_JP12};
inline constexpr lgfx::IFont lgfxJapanGothicP_16{CPOS_FONT_JP16};
}  // namespace fonts
namespace lgfx { namespace fonts = ::fonts; }
using lgfx::color565;

#define TFT_BLACK 0x0000
#define TFT_NAVY 0x000F
#define TFT_DARKGREEN 0x03E0
#define TFT_DARKCYAN 0x03EF
#define TFT_MAROON 0x7800
#define TFT_PURPLE 0x780F
#define TFT_OLIVE 0x7BE0
#define TFT_LIGHTGREY 0xD69A
#define TFT_DARKGREY 0x7BEF
#define TFT_BLUE 0x001F
#define TFT_GREEN 0x07E0
#define TFT_CYAN 0x07FF
#define TFT_RED 0xF800
#define TFT_MAGENTA 0xF81F
#define TFT_YELLOW 0xFFE0
#define TFT_WHITE 0xFFFF
#define TFT_ORANGE 0xFDA0
#define TFT_GREENYELLOW 0xB7E0
#define TFT_PINK 0xFE19
#define TFT_BROWN 0x9A60
#define TFT_GOLD 0xFEA0
#define TFT_SILVER 0xC618
#define TFT_SKYBLUE 0x867D
#define TFT_VIOLET 0x915C

class LGFXBase {
public:
    virtual ~LGFXBase() {}
    virtual cpos_canvas_t *target() = 0;
    virtual bool isDisplay() { return false; }
    int width() { auto c = target(); return c ? GFXAPI.width(c) : 0; }
    int height() { auto c = target(); return c ? GFXAPI.height(c) : 0; }
    static uint16_t color565(uint8_t r, uint8_t g, uint8_t b) { return lgfx::color565(r, g, b); }
    static uint32_t color888(uint8_t r, uint8_t g, uint8_t b) { return lgfx::color888(r, g, b); }
    static uint32_t c32(uint32_t c) { return (c & 0x1000000u) ? (c & 0xFFFFFFu) | 0x1000000u : c; }

    void fillScreen(uint32_t c) { GFXAPI.fill(target(), c32c(c)); }
    void fillSprite(uint32_t c) { GFXAPI.fill(target(), c32c(c)); }
    void clear(uint32_t c = 0) { GFXAPI.fill(target(), c32c(c)); }
    void drawPixel(int x, int y, uint32_t c) { GFXAPI.pixel(target(), x, y, c32c(c)); }
    void drawLine(int x0, int y0, int x1, int y1, uint32_t c) { GFXAPI.line(target(), x0, y0, x1, y1, c32c(c)); }
    void drawFastHLine(int x, int y, int w, uint32_t c) { GFXAPI.hline(target(), x, y, w, c32c(c)); }
    void drawFastVLine(int x, int y, int h, uint32_t c) { GFXAPI.vline(target(), x, y, h, c32c(c)); }
    void drawRect(int x, int y, int w, int h, uint32_t c) { GFXAPI.rect(target(), x, y, w, h, c32c(c), 0); }
    void fillRect(int x, int y, int w, int h, uint32_t c) { GFXAPI.rect(target(), x, y, w, h, c32c(c), 1); }
    void drawRoundRect(int x, int y, int w, int h, int r, uint32_t c) { GFXAPI.round_rect(target(), x, y, w, h, r, c32c(c), 0); }
    void fillRoundRect(int x, int y, int w, int h, int r, uint32_t c) { GFXAPI.round_rect(target(), x, y, w, h, r, c32c(c), 1); }
    void drawCircle(int x, int y, int r, uint32_t c) { GFXAPI.circle(target(), x, y, r, c32c(c), 0); }
    void fillCircle(int x, int y, int r, uint32_t c) { GFXAPI.circle(target(), x, y, r, c32c(c), 1); }
    void drawEllipse(int x, int y, int rx, int ry, uint32_t c) { GFXAPI.ellipse(target(), x, y, rx, ry, c32c(c), 0); }
    void fillEllipse(int x, int y, int rx, int ry, uint32_t c) { GFXAPI.ellipse(target(), x, y, rx, ry, c32c(c), 1); }
    void drawTriangle(int x0, int y0, int x1, int y1, int x2, int y2, uint32_t c) { GFXAPI.triangle(target(), x0, y0, x1, y1, x2, y2, c32c(c), 0); }
    void fillTriangle(int x0, int y0, int x1, int y1, int x2, int y2, uint32_t c) { GFXAPI.triangle(target(), x0, y0, x1, y1, x2, y2, c32c(c), 1); }
    void drawArc(int x, int y, int r0, int r1, float a0, float a1, uint32_t c) { GFXAPI.arc(target(), x, y, r0, r1, a0, a1, c32c(c), 0); }
    void fillArc(int x, int y, int r0, int r1, float a0, float a1, uint32_t c) { GFXAPI.arc(target(), x, y, r0, r1, a0, a1, c32c(c), 1); }

    void setFont(const lgfx::IFont *f) { GFXAPI.set_font(target(), f ? f->id : 0); }
    void setTextSize(float sx, float sy = 0) { GFXAPI.set_text_size(target(), sx, sy); }
    void setTextColor(uint32_t fg) { GFXAPI.set_text_color(target(), c32c(fg), 0, 0); }
    void setTextColor(uint32_t fg, uint32_t bg) { GFXAPI.set_text_color(target(), c32c(fg), c32c(bg), 1); }
    void setTextDatum(textdatum_t d) { GFXAPI.set_text_datum(target(), d); }
    void setTextWrap(bool w, bool = false) { GFXAPI.set_text_wrap(target(), w); }
    int drawString(const char *s, int x, int y) { return GFXAPI.draw_string(target(), s, x, y); }
    int drawString(const String &s, int x, int y) { return GFXAPI.draw_string(target(), s.c_str(), x, y); }
    int drawString(const char *s, int x, int y, const lgfx::IFont *f) { setFont(f); return drawString(s, x, y); }
    int drawString(const String &s, int x, int y, const lgfx::IFont *f) { setFont(f); return drawString(s.c_str(), x, y); }
    int drawCenterString(const char *s, int x, int y) { auto d = datum_; setTextDatum(top_center); int r = drawString(s, x, y); setTextDatum(d); return r; }
    int drawNumber(long v, int x, int y) { char b[24]; snprintf(b, sizeof(b), "%ld", v); return drawString(b, x, y); }
    int drawFloat(float v, int dec, int x, int y) { char b[32]; snprintf(b, sizeof(b), "%.*f", dec, v); return drawString(b, x, y); }
    int textWidth(const char *s) { return GFXAPI.text_width(target(), s); }
    int textWidth(const String &s) { return GFXAPI.text_width(target(), s.c_str()); }
    int textWidth(const char *s, const lgfx::IFont *f) { setFont(f); return textWidth(s); }
    int fontHeight() { return GFXAPI.font_height(target()); }
    int fontHeight(const lgfx::IFont *f) { setFont(f); return fontHeight(); }
    void setClipRect(int x, int y, int w, int h) { GFXAPI.set_clip(target(), x, y, w, h); }
    void clearClipRect() { GFXAPI.clear_clip(target()); }
    void qrcode(const char *text, int x, int y, int w, int = 0) { GFXAPI.qrcode(target(), text, x, y, w); }
    void qrcode(const String &text, int x, int y, int w, int v = 0) { qrcode(text.c_str(), x, y, w, v); }
    void pushImage(int x, int y, int w, int h, const lgfx::rgb565_t *d) { GFXAPI.blit(target(), x, y, w, h, (const uint16_t *)d, 0, -1); }
    void pushImage(int x, int y, int w, int h, const lgfx::swap565_t *d) { GFXAPI.blit(target(), x, y, w, h, (const uint16_t *)d, 1, -1); }
    void pushImage(int x, int y, int w, int h, const uint16_t *d) { GFXAPI.blit(target(), x, y, w, h, d, 1, -1); }
    void pushImage(int x, int y, int w, int h, const uint16_t *d, uint16_t transparent) { GFXAPI.blit(target(), x, y, w, h, d, 1, transparent); }
    // image decoders are not part of the OS yet
    bool drawPng(const uint8_t *, uint32_t, int = 0, int = 0, int = 0, int = 0, int = 0, int = 0, float = 1.0f, float = 0.0f) { return false; }
    bool drawJpg(const uint8_t *, uint32_t, int = 0, int = 0, int = 0, int = 0, int = 0, int = 0, float = 1.0f, float = 0.0f) { return false; }
    // cursor based text output
    void setCursor(int x, int y) { cx_ = x; cy_ = y; }
    size_t print(const char *s) { int w = drawStringTL(s, cx_, cy_); cx_ += w; return strlen(s); }
    size_t print(const String &s) { return print(s.c_str()); }
    size_t println(const char *s) { print(s); cx_ = 0; cy_ += fontHeight(); return strlen(s) + 1; }
    size_t println(const String &s) { return println(s.c_str()); }
    size_t printf(const char *f, ...) {
        char b[160];
        va_list ap; va_start(ap, f);
        vsnprintf(b, sizeof(b), f, ap);
        va_end(ap);
        return print(b);
    }

protected:
    textdatum_t datum_ = top_left;
    int cx_ = 0, cy_ = 0;
    static uint32_t c32c(uint32_t c) { return (c & 0x1000000u) ? (c & 0xFFFFFFu) : c; }
    int drawStringTL(const char *s, int x, int y) {
        GFXAPI.set_text_datum(target(), top_left);
        int w = GFXAPI.draw_string(target(), s, x, y);
        GFXAPI.set_text_datum(target(), datum_);
        return w;
    }
};

class M5GFX : public LGFXBase {
public:
    cpos_canvas_t *target() override { return GFXAPI.screen(); }
    bool isDisplay() override { return true; }
    void setRotation(int) {}
    void setBrightness(uint8_t b) { GFXAPI.set_brightness(b); }
    void setColorDepth(int) {}
    void init() {}
    void startWrite() {}
    void endWrite() {}
    void display() {}
};

class LGFX_Sprite : public LGFXBase {
public:
    LGFX_Sprite(LGFXBase *parent = nullptr) : parent_(parent) {}
    ~LGFX_Sprite() override { deleteSprite(); }
    cpos_canvas_t *target() override { return cv_; }
    void setColorDepth(int d) { depth_ = d; }
    void *createSprite(int w, int h) {
        deleteSprite();
        cv_ = GFXAPI.create(w, h, depth_);
        return cv_;
    }
    void deleteSprite() { if (cv_) { GFXAPI.destroy(cv_); cv_ = nullptr; } }
    void *getBuffer() { return cv_ ? GFXAPI.buffer(cv_) : nullptr; }
    void pushSprite(int x, int y) { GFXAPI.push(cv_, nullptr, x, y, -1); }
    void pushSprite(int x, int y, uint32_t transparent) { GFXAPI.push(cv_, nullptr, x, y, (int)transparent); }
    void pushSprite(LGFXBase *dst, int x, int y) { push_to(dst, x, y, -1); }
    void pushSprite(LGFXBase *dst, int x, int y, uint32_t transparent) { push_to(dst, x, y, (int)transparent); }

private:
    void push_to(LGFXBase *dst, int x, int y, int t) {
        cpos_canvas_t *d = (dst && !dst->isDisplay()) ? dst->target() : nullptr;
        GFXAPI.push(cv_, d, x, y, t);
    }
    LGFXBase *parent_;
    cpos_canvas_t *cv_ = nullptr;
    int depth_ = 16;
};
typedef LGFX_Sprite M5Canvas;

// ---- keyboard --------------------------------------------------------------------------
class Keyboard_Class {
public:
    struct KeysState {
        bool tab = false, fn = false, shift = false, ctrl = false, opt = false, alt = false, del = false, enter = false, space = false;
        uint8_t modifiers = 0;
        std::vector<char> word;
    };
    void begin() {}
    void update() {
        cpos_kbd_t k;
        cpos_api->kbd_state(&k);
        changed_ = k.seq != seq_;
        seq_ = k.seq;
        st_ = KeysState();
        st_.tab = k.tab; st_.fn = k.fn; st_.shift = k.shift; st_.ctrl = k.ctrl; st_.opt = k.opt; st_.alt = k.alt;
        st_.del = k.del; st_.enter = k.enter; st_.space = k.space; st_.modifiers = k.modifiers;
        for (int i = 0; i < k.count; i++) st_.word.push_back(k.keys[i]);
        pressed_ = k.count + k.tab + k.fn + k.shift + k.ctrl + k.opt + k.alt + k.del + k.enter;
    }
    bool isChange() { return changed_; }
    uint8_t isPressed() { return pressed_; }
    KeysState keysState() { return st_; }
    bool isKeyPressed(char c) {
        for (char w : st_.word) if (w == c) return true;
        return false;
    }

private:
    uint32_t seq_ = 0;
    bool changed_ = false;
    uint8_t pressed_ = 0;
    KeysState st_;
};

// ---- buttons, power, sensors, audio -------------------------------------------------
class Button_Class {
public:
    void update() {
        bool now = cpos_api->power.button_a();
        uint32_t t = millis();
        was_pressed_ = now && !down_;
        was_released_ = !now && down_;
        if (now != down_) { last_change_ = t; }
        down_ = now;
    }
    bool isPressed() const { return down_; }
    bool isReleased() const { return !down_; }
    bool wasPressed() const { return was_pressed_; }
    bool wasReleased() const { return was_released_; }
    bool pressedFor(uint32_t ms) const { return down_ && millis() - last_change_ >= ms; }
    uint32_t lastChange() const { return last_change_; }

private:
    bool down_ = false, was_pressed_ = false, was_released_ = false;
    uint32_t last_change_ = 0;
};

class Power_Class {
public:
    int32_t getBatteryLevel() { return cpos_api->power.battery_level(); }
    int16_t getBatteryVoltage() { return cpos_api->power.battery_mv(); }
};

class Imu_Class {
public:
    bool update() { return cpos_api->imu.update() != 0; }
    bool isEnabled() { return cpos_api->imu.enabled() != 0; }
    void getAccel(float *x, float *y, float *z) { cpos_api->imu.accel(x, y, z); }
    void getGyro(float *x, float *y, float *z) { cpos_api->imu.gyro(x, y, z); }
};

class Speaker_Class {
public:
    bool begin() { return cpos_api->audio.begin() != 0; }
    void end() { cpos_api->audio.end(); }
    bool isEnabled() { return true; }
    void setVolume(uint8_t v) { cpos_api->audio.set_volume(v); }
    bool tone(float freq, uint32_t ms = 0xFFFFFFFF, int ch = -1, bool stop_current = true) {
        cpos_api->audio.tone(freq, ms, ch, stop_current, nullptr, 0);
        return true;
    }
    bool tone(float freq, uint32_t ms, int ch, bool stop_current, const uint8_t *wave, size_t len, bool = false) {
        cpos_api->audio.tone(freq, ms, ch, stop_current, wave, len);
        return true;
    }
    bool playRaw(const int16_t *s, size_t n, uint32_t rate, bool stereo = false, uint32_t repeat = 1, int ch = -1, bool stop = false) {
        cpos_api->audio.play_raw(s, n, rate, stereo, repeat, ch, stop);
        return true;
    }
    void stop(int ch = -1) { cpos_api->audio.stop(ch); }
    size_t isPlaying(int ch = -1) { return cpos_api->audio.is_playing(ch); }
};

struct mic_config_t {
    uint32_t sample_rate = 16000;
    uint8_t noise_filter_level = 0;
    uint8_t magnification = 16;
    uint16_t dma_buf_len = 256;
    uint8_t dma_buf_count = 3;
    bool stereo = false;
    bool over_sampling = 0;
};

class Mic_Class {
public:
    mic_config_t config() { return cfg_; }
    void config(const mic_config_t &c) { cfg_ = c; }
    bool begin() { return cpos_api->audio.mic_begin(cfg_.sample_rate) != 0; }
    void end() { cpos_api->audio.mic_end(); }
    bool isEnabled() { return true; }
    bool record(int16_t *buf, size_t len, uint32_t rate) { return cpos_api->audio.mic_record(buf, len, rate) != 0; }
    bool isRecording() { return false; }

private:
    mic_config_t cfg_;
};

namespace m5 {
struct M5Unified {
    struct config_t {
        uint32_t serial_baudrate = 115200;
        bool clear_display = true, output_power = true, internal_imu = true, internal_rtc = true, internal_spk = true,
             internal_mic = true, external_imu = false, external_rtc = false, led_brightness = false, fallback_board = false;
    };
};
}  // namespace m5

class M5CardputerClass {
public:
    M5GFX Display;
    Keyboard_Class Keyboard;
    Button_Class BtnA;
    Power_Class Power;
    Imu_Class Imu;
    Speaker_Class Speaker;
    Mic_Class Mic;
    m5::M5Unified::config_t config() { return {}; }
    void begin() {}
    void begin(const m5::M5Unified::config_t &, bool = true) {}
    void update() { Keyboard.update(); BtnA.update(); }
};
extern M5CardputerClass M5Cardputer;
extern M5CardputerClass &M5;
