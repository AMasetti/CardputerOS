/* Audio and IMU services. The drivers land after the core; until then they report "not available". */
#include "app_ctx.h"

static int au_begin(void) { return 0; }
static void au_end(void) {}
static void au_vol(uint8_t v) {}
static void au_tone(float f, uint32_t ms, int ch, int stop, const uint8_t *w, size_t n) {}
static void au_raw(const int16_t *s, size_t n, uint32_t r, int st, int rep, int ch, int stop) {}
static int au_playing(int ch) { return 0; }
static void au_stop(int ch) {}
static int au_mic_begin(uint32_t r) { return 0; }
static int au_mic_rec(int16_t *b, size_t n, uint32_t r) { return 0; }
static void au_mic_end(void) {}

void cpos_audio_fill(cpos_audio_api_t *a) {
    a->begin = au_begin; a->end = au_end; a->set_volume = au_vol; a->tone = au_tone; a->play_raw = au_raw;
    a->is_playing = au_playing; a->stop = au_stop; a->mic_begin = au_mic_begin; a->mic_record = au_mic_rec; a->mic_end = au_mic_end;
}
void cpos_app_cleanup_audio(app_ctx_t *a) {}

static int im_update(void) { return 0; }
static int im_enabled(void) { return 0; }
static void im_vec(float *x, float *y, float *z) { *x = *y = *z = 0; }
void cpos_imu_fill(cpos_imu_api_t *i) { i->update = im_update; i->enabled = im_enabled; i->accel = im_vec; i->gyro = im_vec; }
