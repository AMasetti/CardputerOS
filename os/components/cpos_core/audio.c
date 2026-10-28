/*
 * Speaker and microphone through the ES8311 codec on the Cardputer ADV.
 * The codec sits on the internal I2C bus (0x18) and takes its clock from the I2S bit clock,
 * so the ESP is the I2S master. Speaker and microphone share BCLK/WS, therefore only one of
 * them is open at a time (the same restriction the original firmware had).
 */
#include "app_ctx.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal.h"
#include <math.h>
#include <string.h>

#define PIN_I2S_BCLK 41
#define PIN_I2S_WS 43
#define PIN_I2S_DOUT 42
#define PIN_I2S_DIN 46
#define SPK_RATE 44100
#define BLOCK 256
#define VOICES 8

static const char *TAG = "audio";

typedef struct {
    uint8_t on;
    float phase, inc;            /* tone: position inside one period, step per sample */
    int64_t remain;              /* samples left, <0 = until stopped */
    const uint8_t *wave;
    size_t wlen;
    const int16_t *raw;          /* sampled sound */
    size_t rawlen;
    float rpos, rstep;
    int repeat, stereo;
} voice_t;

static voice_t s_v[VOICES];
static i2s_chan_handle_t s_tx, s_rx;
static i2c_master_dev_handle_t s_codec;
static TaskHandle_t s_task;
static volatile int s_spk_open, s_tx_run;
static float s_gain = 0.25f;
static int16_t s_sine[256];
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t s_mic_pga = 0x10;

static void codec_write_list(const uint8_t *list) {
    for (; list[0]; list += 3) {
        uint8_t b[2] = {list[1], list[2]};
        i2c_master_transmit(s_codec, b, 2, 50);
    }
}

static int codec_open(void) {
    if (s_codec) return 0;
    i2c_device_config_t dc = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = 0x18, .scl_speed_hz = 400000};
    if (i2c_master_bus_add_device(hal_i2c(), &dc, &s_codec) != ESP_OK) { s_codec = NULL; return -1; }
    return 0;
}

static void tx_close(void) {
    if (!s_tx) return;
    if (s_tx_run) i2s_channel_disable(s_tx);
    i2s_del_channel(s_tx);
    s_tx = NULL;
    s_tx_run = 0;
}

static void rx_close(void) {
    if (!s_rx) return;
    i2s_channel_disable(s_rx);
    i2s_del_channel(s_rx);
    s_rx = NULL;
}

static int tx_open(void) {
    if (s_tx) return 0;
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    cc.dma_desc_num = 4;
    cc.dma_frame_num = BLOCK;
    cc.auto_clear = true;
    if (i2s_new_channel(&cc, &s_tx, NULL) != ESP_OK) { s_tx = NULL; return -1; }
    i2s_std_config_t sc = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SPK_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {.mclk = I2S_GPIO_UNUSED, .bclk = PIN_I2S_BCLK, .ws = PIN_I2S_WS, .dout = PIN_I2S_DOUT, .din = I2S_GPIO_UNUSED},
    };
    if (i2s_channel_init_std_mode(s_tx, &sc) != ESP_OK) { tx_close(); return -1; }
    return 0;
}

static void mix(int16_t *out, int frames) {
    static float acc[BLOCK];
    memset(acc, 0, sizeof(float) * frames);
    for (int k = 0; k < VOICES; k++) {
        voice_t *v = &s_v[k];
        if (!v->on) continue;
        for (int i = 0; i < frames; i++) {
            float s;
            if (v->raw) {
                size_t p = (size_t)v->rpos;
                if (p >= v->rawlen) {
                    if (v->repeat > 1) { v->repeat--; v->rpos = 0; p = 0; }
                    else { v->on = 0; break; }
                }
                s = v->raw[v->stereo ? p * 2 : p] / 32768.0f;
                v->rpos += v->rstep;
            } else {
                if (v->remain == 0) { v->on = 0; break; }
                if (v->wave && v->wlen) s = ((int)v->wave[(size_t)(v->phase * v->wlen) % v->wlen] - 128) / 128.0f;
                else s = s_sine[(int)(v->phase * 256) & 255] / 32768.0f;
                v->phase += v->inc;
                if (v->phase >= 1.0f) v->phase -= 1.0f;
                if (v->remain > 0) v->remain--;
            }
            acc[i] += s;
        }
    }
    for (int i = 0; i < frames; i++) {
        float x = acc[i] * s_gain * 32767.0f;
        if (x > 32767) x = 32767;
        if (x < -32768) x = -32768;
        out[2 * i] = out[2 * i + 1] = (int16_t)x;
    }
}

static int any_active(void) {
    for (int k = 0; k < VOICES; k++) if (s_v[k].on) return 1;
    return 0;
}

static void audio_task(void *arg) {
    static int16_t buf[BLOCK * 2];
    int idle = 0;
    for (;;) {
        if (!s_spk_open) { s_task = NULL; vTaskDelete(NULL); }
        if (!any_active()) {
            if (s_tx_run && ++idle > 40) { i2s_channel_disable(s_tx); s_tx_run = 0; }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        idle = 0;
        if (!s_tx_run && s_tx) { i2s_channel_enable(s_tx); s_tx_run = 1; }
        mix(buf, BLOCK);
        size_t w;
        i2s_channel_write(s_tx, buf, sizeof(buf), &w, 200);
    }
}

static int spk_open(void) {
    if (s_spk_open) return 1;
    for (int i = 0; i < 256; i++) s_sine[i] = (int16_t)(sinf(i * 6.2831853f / 256.0f) * 32767.0f);
    rx_close();
    if (codec_open() || tx_open()) return 0;
    static const uint8_t spk_regs[] = {
        2, 0x00, 0x80, 2, 0x01, 0xB5, 2, 0x02, 0x18, 2, 0x0D, 0x01, 2, 0x12, 0x00, 2, 0x13, 0x10,
        2, 0x32, 0xBF, 2, 0x37, 0x08, 0};
    /* the register list uses the 3-byte (len, reg, value) layout of the vendor tables */
    for (const uint8_t *p = spk_regs; p[0]; p += 3) {
        uint8_t b[2] = {p[1], p[2]};
        i2c_master_transmit(s_codec, b, 2, 50);
    }
    s_spk_open = 1;
    xTaskCreatePinnedToCore(audio_task, "audio", 3072, NULL, 6, &s_task, 1);
    return 1;
}

static int au_begin(void) { return cpos_has_perm(CPOS_PERM_AUDIO_OUT) ? spk_open() : 0; }

static void au_end(void) {
    s_spk_open = 0;
    taskENTER_CRITICAL(&s_mux);
    for (int k = 0; k < VOICES; k++) s_v[k].on = 0;
    taskEXIT_CRITICAL(&s_mux);
    for (int i = 0; i < 40 && s_task; i++) vTaskDelay(pdMS_TO_TICKS(10)); /* let the mixer leave i2s_channel_write */
    tx_close();
}

static void au_vol(uint8_t v) { s_gain = (v / 255.0f) * 0.7f; }

static voice_t *pick(int ch, int stop) {
    if (ch >= 0 && ch < VOICES) return &s_v[ch];
    for (int k = 0; k < VOICES; k++) if (!s_v[k].on) return &s_v[k];
    return &s_v[0];
}

static void au_tone(float f, uint32_t ms, int ch, int stop, const uint8_t *w, size_t n) {
    if (!cpos_has_perm(CPOS_PERM_AUDIO_OUT) || !au_begin() || f <= 0) return;
    voice_t *v = pick(ch, stop);
    if (v->on && !stop && ch >= 0) return;
    voice_t nv = {0};
    nv.inc = f / SPK_RATE;
    nv.remain = ms == 0xFFFFFFFFu ? -1 : (int64_t)ms * SPK_RATE / 1000;
    nv.wave = w;
    nv.wlen = w ? n : 0;
    nv.on = 1;
    taskENTER_CRITICAL(&s_mux);
    *v = nv;
    taskEXIT_CRITICAL(&s_mux);
}

static void au_raw(const int16_t *s, size_t n, uint32_t rate, int stereo, int repeat, int ch, int stop) {
    if (!cpos_has_perm(CPOS_PERM_AUDIO_OUT) || !au_begin() || !s || !n) return;
    voice_t *v = pick(ch, stop);
    voice_t nv = {0};
    nv.raw = s;
    nv.rawlen = stereo ? n / 2 : n;
    nv.rstep = (float)rate / SPK_RATE;
    nv.repeat = repeat > 0 ? repeat : 1;
    nv.stereo = stereo;
    nv.on = 1;
    taskENTER_CRITICAL(&s_mux);
    *v = nv;
    taskEXIT_CRITICAL(&s_mux);
}

static int au_playing(int ch) {
    if (ch >= 0 && ch < VOICES) return s_v[ch].on;
    return any_active();
}

static void au_stop(int ch) {
    taskENTER_CRITICAL(&s_mux);
    if (ch >= 0 && ch < VOICES) s_v[ch].on = 0;
    else for (int k = 0; k < VOICES; k++) s_v[k].on = 0;
    taskEXIT_CRITICAL(&s_mux);
}

/* ---- microphone -------------------------------------------------------------------- */
static uint32_t s_mic_rate;

static int au_mic_begin(uint32_t rate) {
    if (!cpos_has_perm(CPOS_PERM_AUDIO_IN)) return 0;
    if (s_rx && s_mic_rate == rate) return 1;
    au_end();
    rx_close();
    if (codec_open()) return 0;
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    cc.dma_desc_num = 4;
    cc.dma_frame_num = 256;
    if (i2s_new_channel(&cc, NULL, &s_rx) != ESP_OK) { s_rx = NULL; return 0; }
    i2s_std_config_t sc = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {.mclk = I2S_GPIO_UNUSED, .bclk = PIN_I2S_BCLK, .ws = PIN_I2S_WS, .dout = I2S_GPIO_UNUSED, .din = PIN_I2S_DIN},
    };
    if (i2s_channel_init_std_mode(s_rx, &sc) != ESP_OK) { rx_close(); return 0; }
    i2s_channel_enable(s_rx);
    static const uint8_t mic_regs[] = {
        2, 0x00, 0x80, 2, 0x01, 0xBA, 2, 0x02, 0x18, 2, 0x0D, 0x01, 2, 0x0E, 0x02, 2, 0x14, 0x10,
        2, 0x17, 0xBF, 2, 0x1C, 0x6A, 0};
    for (const uint8_t *p = mic_regs; p[0]; p += 3) {
        uint8_t b[2] = {p[1], p[2]};
        i2c_master_transmit(s_codec, b, 2, 50);
    }
    /* the capture path arms only with the clock running */
    for (int i = 0; i < 3; i++) {
        vTaskDelay(pdMS_TO_TICKS(30));
        uint8_t b[2] = {0x0D, 0x01};
        i2c_master_transmit(s_codec, b, 2, 50);
    }
    s_mic_rate = rate;
    return 1;
}

static int au_mic_record(int16_t *buf, size_t len, uint32_t rate) {
    if (!cpos_has_perm(CPOS_PERM_AUDIO_IN)) return 0;
    cpos_check_exit();
    if (!s_rx && !au_mic_begin(rate)) return 0;
    size_t got = 0, total = len * 2;
    uint8_t *p = (uint8_t *)buf;
    while (got < total) {
        size_t n = 0;
        if (i2s_channel_read(s_rx, p + got, total - got, &n, 500) != ESP_OK) return 0;
        got += n;
    }
    for (size_t i = 0; i < len; i++) { /* codec level is low: apply a fixed software gain */
        int32_t x = (int32_t)buf[i] * 8;
        buf[i] = x > 32767 ? 32767 : (x < -32768 ? -32768 : (int16_t)x);
    }
    return 1;
}

static void au_mic_end(void) { rx_close(); s_mic_rate = 0; }

/* ---- system sounds (launcher clicks): no app permission involved ------------------- */
void cpos_ui_beep(int hz, int ms) {
    if (!spk_open()) return;
    s_gain = 0.12f;
    voice_t nv = {0};
    nv.inc = (float)hz / SPK_RATE;
    nv.remain = (int64_t)ms * SPK_RATE / 1000;
    nv.on = 1;
    taskENTER_CRITICAL(&s_mux);
    s_v[0] = nv;
    taskEXIT_CRITICAL(&s_mux);
}

/* close the speaker before an app runs, so the app starts from the default state */
void cpos_ui_audio_off(void) {
    if (s_spk_open) au_end();
    s_gain = 0.25f;
}

void cpos_audio_fill(cpos_audio_api_t *a) {
    a->begin = au_begin; a->end = au_end; a->set_volume = au_vol; a->tone = au_tone; a->play_raw = au_raw;
    a->is_playing = au_playing; a->stop = au_stop; a->mic_begin = au_mic_begin; a->mic_record = au_mic_record; a->mic_end = au_mic_end;
}

void cpos_app_cleanup_audio(app_ctx_t *a) {
    au_end();
    rx_close();
    s_mic_rate = 0;
}
