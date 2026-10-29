// Arduino API subset for CardputerOS apps. The hardware calls go through the OS (permissions apply).
#pragma once
#include <algorithm>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cpos/abi.h>
#include "WString.h"

extern const cpos_api_t *cpos_api;

#define HIGH 1
#define LOW 0
#define INPUT 0
#define OUTPUT 1
#define INPUT_PULLUP 2
#define LSBFIRST 0
#define MSBFIRST 1
#define PI 3.1415926535897932384626433832795
#define HALF_PI 1.5707963267948966192313216916398
#define TWO_PI 6.283185307179586476925286766559
#define DEG_TO_RAD 0.017453292519943295769236907684886
#define RAD_TO_DEG 57.295779513082320876798154814105
#define EULER 2.718281828459045235360287471352
#define DEC 10
#define HEX 16
#define OCT 8
#define BIN 2

typedef uint8_t byte;
typedef bool boolean;
typedef unsigned int uint;

using std::max;
using std::min;
#ifndef abs
using std::abs;
#endif

#define constrain(amt, low, high) ((amt) < (low) ? (low) : ((amt) > (high) ? (high) : (amt)))
#define sq(x) ((x) * (x))
#define radians(deg) ((deg) * DEG_TO_RAD)
#define degrees(rad) ((rad) * RAD_TO_DEG)
#define bitRead(v, b) (((v) >> (b)) & 0x01)
#define bitSet(v, b) ((v) |= (1UL << (b)))
#define bitClear(v, b) ((v) &= ~(1UL << (b)))
#define lowByte(w) ((uint8_t)((w) & 0xff))
#define highByte(w) ((uint8_t)((w) >> 8))
#define IRAM_ATTR
#define DRAM_ATTR
#define PROGMEM
#define pgm_read_byte(a) (*(const uint8_t *)(a))
#define pgm_read_word(a) (*(const uint16_t *)(a))

inline uint32_t millis() { return cpos_api->millis(); }
inline uint32_t micros() { return cpos_api->micros(); }
inline void delay(uint32_t ms) { cpos_api->delay_ms(ms); }
inline void delayMicroseconds(uint32_t us) { cpos_api->delay_us(us); }
inline void yield() { cpos_api->delay_ms(0); }
inline long map(long x, long in_min, long in_max, long out_min, long out_max) {
    return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}
inline void randomSeed(unsigned long) {}
inline long random(long max) { return max > 0 ? (long)(cpos_api->random32() % (uint32_t)max) : 0; }
inline long random(long min, long max) { return min >= max ? min : min + random(max - min); }
inline uint32_t esp_random() { return cpos_api->random32(); }

// FreeRTOS subset: extra tasks run through the OS (1 tick = 1 ms)
typedef void *TaskHandle_t;
typedef uint32_t TickType_t;
typedef int BaseType_t;
typedef unsigned UBaseType_t;
#define pdPASS 1
#define pdFAIL 0
#define pdTRUE 1
#define pdFALSE 0
#define portMAX_DELAY 0xFFFFFFFFu
#define portTICK_PERIOD_MS 1
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define tskIDLE_PRIORITY 0
inline void vTaskDelay(TickType_t t) { cpos_api->delay_ms(t ? t : 1); }
inline TickType_t xTaskGetTickCount() { return cpos_api->millis(); }
inline BaseType_t xTaskCreatePinnedToCore(void (*fn)(void *), const char *name, uint32_t stack, void *arg, UBaseType_t prio,
                                          TaskHandle_t *h, BaseType_t = 0) {
    int id = cpos_api->rtos.task_create(fn, name, stack, arg, (int)prio);
    if (h) *h = id < 0 ? nullptr : (TaskHandle_t)(intptr_t)(id + 1);
    return id < 0 ? pdFAIL : pdPASS;
}
inline BaseType_t xTaskCreate(void (*fn)(void *), const char *name, uint32_t stack, void *arg, UBaseType_t prio, TaskHandle_t *h) {
    return xTaskCreatePinnedToCore(fn, name, stack, arg, prio, h);
}
inline void vTaskDelete(TaskHandle_t h) { cpos_api->rtos.task_delete(h ? (int)(intptr_t)h - 1 : -1); }

inline void pinMode(int pin, int mode) { cpos_api->gpio.pin_mode(pin, mode); }
inline void digitalWrite(int pin, int level) { cpos_api->gpio.write(pin, level); }
inline int digitalRead(int pin) { return cpos_api->gpio.read(pin); }
inline uint32_t ledcSetup(uint8_t ch, uint32_t freq, uint8_t bits) { return cpos_api->gpio.pwm_setup(ch, freq, bits); }
inline void ledcAttachPin(int pin, uint8_t ch) { cpos_api->gpio.pwm_attach(pin, ch); }
inline void ledcWrite(uint8_t ch, uint32_t duty) { cpos_api->gpio.pwm_write(ch, duty); }

// Serial goes to the OS log
class HardwareSerial {
public:
    void begin(unsigned long = 115200) {}
    void end() {}
    operator bool() const { return true; }
    void flush() {}
    size_t print(const char *s) { cpos_api->log(s); return strlen(s); }
    size_t print(const String &s) { return print(s.c_str()); }
    size_t print(char c) { char b[2] = {c, 0}; cpos_api->log(b); return 1; }
    size_t print(int v) { char b[16]; snprintf(b, sizeof(b), "%d", v); return print(b); }
    size_t print(unsigned v) { char b[16]; snprintf(b, sizeof(b), "%u", v); return print(b); }
    size_t print(long v) { char b[24]; snprintf(b, sizeof(b), "%ld", v); return print(b); }
    size_t print(unsigned long v) { char b[24]; snprintf(b, sizeof(b), "%lu", v); return print(b); }
    size_t print(double v, int d = 2) { char b[32]; snprintf(b, sizeof(b), "%.*f", d, v); return print(b); }
    template <class T> size_t println(T v) { size_t n = print(v); cpos_api->log("\n"); return n + 1; }
    size_t println() { cpos_api->log("\n"); return 1; }
    size_t printf(const char *f, ...) {
        char b[200];
        va_list ap; va_start(ap, f);
        vsnprintf(b, sizeof(b), f, ap);
        va_end(ap);
        return print(b);
    }
    int available() { return 0; }
    int read() { return -1; }
    size_t write(uint8_t c) { return print((char)c); }
};
extern HardwareSerial Serial;

class EspClass {
public:
    void restart() { cpos_api->exit_app(0); }
    uint32_t getFreeHeap() { return (uint32_t)cpos_api->free_heap(); }
    uint32_t getHeapSize() { return 300000; }
    uint32_t getMaxAllocHeap() { return (uint32_t)cpos_api->free_heap() / 2; }
};
extern EspClass ESP;
