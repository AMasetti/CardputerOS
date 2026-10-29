// App runtime: entry point, C++ support and the pieces of the C library that only need the OS table.
#include <cpos/abi.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <new>
#include <time.h>

const cpos_api_t *cpos_api;
void (*cpos_loop_hook)(void); /* set by shims that deliver events between loop() calls (BLE) */

void setup();
void loop();

extern "C" {
typedef void (*ctor_t)(void);
extern ctor_t __init_array_start[];
extern ctor_t __init_array_end[];
extern ctor_t __ctors_start[];
extern ctor_t __ctors_end[];

// The OS calls this with the ABI table. Arduino-style apps provide setup()/loop().
int cpos_app_entry(const cpos_api_t *api) {
    cpos_api = api;
    for (ctor_t *c = __init_array_start; c < __init_array_end; c++) (*c)();
    for (ctor_t *c = __ctors_end; c > __ctors_start;) (*--c)();
    setup();
    while (!api->exit_requested()) {
        loop();
        if (cpos_loop_hook) cpos_loop_hook();
    }
    return 0;
}

void *__dso_handle = nullptr;
int __cxa_atexit(void (*)(void *), void *, void *) { return 0; }
int atexit(void (*)(void)) { return 0; }
void exit(int code) { cpos_api->exit_app(code); for (;;) {} }
void _exit(int code) { cpos_api->exit_app(code); for (;;) {} }
void __cxa_pure_virtual() { cpos_api->exit_app(-2); }
int __cxa_guard_acquire(uint64_t *g) { return !*(char *)g; }
void __cxa_guard_release(uint64_t *g) { *(char *)g = 1; }
void __cxa_guard_abort(uint64_t *) {}

void *malloc(size_t n) { return cpos_api->malloc(n); }
void *calloc(size_t n, size_t s) { return cpos_api->calloc(n, s); }
void *realloc(void *p, size_t n) { return cpos_api->realloc(p, n); }
void free(void *p) { cpos_api->free(p); }

void __assert_func(const char *file, int line, const char *func, const char *expr) {
    char b[160];
    snprintf(b, sizeof(b), "assert failed: %s (%s:%d)\n", expr, file, line);
    cpos_api->log(b);
    cpos_api->exit_app(-3);
    for (;;) {}
}

// printf family on top of the v* entries of the table
int snprintf(char *b, size_t n, const char *f, ...) {
    va_list ap; va_start(ap, f);
    int r = cpos_api->libc.vsnprintf(b, n, f, ap);
    va_end(ap);
    return r;
}
int sprintf(char *b, const char *f, ...) {
    va_list ap; va_start(ap, f);
    int r = cpos_api->libc.vsprintf(b, f, ap);
    va_end(ap);
    return r;
}
int sscanf(const char *s, const char *f, ...) {
    va_list ap; va_start(ap, f);
    int r = cpos_api->libc.vsscanf(s, f, ap);
    va_end(ap);
    return r;
}
int vsnprintf(char *b, size_t n, const char *f, va_list ap) { return cpos_api->libc.vsnprintf(b, n, f, ap); }
int printf(const char *f, ...) {
    va_list ap; va_start(ap, f);
    int r = cpos_api->libc.vprintf(f, ap);
    va_end(ap);
    return r;
}
int puts(const char *s) { cpos_api->log(s); cpos_api->log("\n"); return 1; }
int putchar(int c) { char b[2] = {(char)c, 0}; cpos_api->log(b); return c; }
// fprintf(stderr/stdout, ...) goes to the log as well
int fprintf(FILE *, const char *f, ...) {
    va_list ap; va_start(ap, f);
    int r = cpos_api->libc.vprintf(f, ap);
    va_end(ap);
    return r;
}

// time(): seconds since boot is enough for the apps we have
time_t time(time_t *t) { time_t v = (time_t)(cpos_api->millis() / 1000); if (t) *t = v; return v; }
}

// ---- C++ allocation ------------------------------------------------------------------
void *operator new(size_t n) { return malloc(n); }
void *operator new[](size_t n) { return malloc(n); }
void *operator new(size_t n, const std::nothrow_t &) noexcept { return malloc(n); }
void *operator new[](size_t n, const std::nothrow_t &) noexcept { return malloc(n); }
void operator delete(void *p) noexcept { free(p); }
void operator delete[](void *p) noexcept { free(p); }
void operator delete(void *p, size_t) noexcept { free(p); }
void operator delete[](void *p, size_t) noexcept { free(p); }

// ---- exceptions are off: these are the hooks libstdc++ headers still reference -------
namespace std {
void __throw_bad_alloc() { cpos_api->exit_app(-4); for (;;) {} }
void __throw_bad_array_new_length() { cpos_api->exit_app(-4); for (;;) {} }
void __throw_length_error(const char *) { cpos_api->exit_app(-4); for (;;) {} }
void __throw_logic_error(const char *) { cpos_api->exit_app(-4); for (;;) {} }
void __throw_out_of_range(const char *) { cpos_api->exit_app(-4); for (;;) {} }
void __throw_out_of_range_fmt(const char *, ...) { cpos_api->exit_app(-4); for (;;) {} }
void __throw_invalid_argument(const char *) { cpos_api->exit_app(-4); for (;;) {} }
void __throw_bad_function_call() { cpos_api->exit_app(-4); for (;;) {} }
void __throw_system_error(int) { cpos_api->exit_app(-4); for (;;) {} }
void terminate() noexcept { cpos_api->exit_app(-4); for (;;) {} }
void __glibcxx_assert_fail(const char *, int, const char *, const char *) noexcept { cpos_api->exit_app(-6); for (;;) {} }
}

// ---- std::string: the library's prebuilt copy is not linked, so instantiate it here -----
#include <string>
template class std::__cxx11::basic_string<char>;

// ---- std::mutex: real OS mutexes (apps may run extra tasks). A pthread_mutex_t is one word
// holding the OS handle; it starts as 0xFFFFFFFF (static initializer) or 0 and is created on
// first use.
#include <stdint.h>
static void *mutex_of(void *m) {
    volatile uint32_t *w = (volatile uint32_t *)m;
    uint32_t v = *w;
    if (v && v != 0xFFFFFFFFu) return (void *)v;
    if (cpos_api->abi_minor < 2) return nullptr; /* older OS: single task, nothing to wait for */
    void *h = cpos_api->rtos.mutex_create(0);
    if (!h) return nullptr;
    if (!__atomic_compare_exchange_n(w, &v, (uint32_t)h, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        cpos_api->rtos.mutex_delete(h); /* another task created it first */
        return (void *)*w;
    }
    return h;
}
extern "C" {
int pthread_mutex_lock(void *m) { void *h = mutex_of(m); return h ? cpos_api->rtos.mutex_lock(h, 0xFFFFFFFFu) : 0; }
int pthread_mutex_unlock(void *m) { void *h = mutex_of(m); if (h) cpos_api->rtos.mutex_unlock(h); return 0; }
int pthread_mutex_trylock(void *m) { void *h = mutex_of(m); return !h || cpos_api->rtos.mutex_lock(h, 0) == 0 ? 0 : 16 /* EBUSY */; }
int pthread_mutex_init(void *m, const void *) { *(uint32_t *)m = 0; return 0; }
int pthread_mutex_destroy(void *m) {
    uint32_t v = *(uint32_t *)m;
    if (v && v != 0xFFFFFFFFu) cpos_api->rtos.mutex_delete((void *)v);
    *(uint32_t *)m = 0;
    return 0;
}
}

// ---- 64-bit string to integer (not in the OS table) --------------------------------------
extern "C" unsigned long long strtoull(const char *s, char **end, int base) {
    const char *p = s;
    while (*p == ' ' || *p == '\t' || *p == '\n') p++;
    bool neg = false;
    if (*p == '+' || *p == '-') neg = *p++ == '-';
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { p += 2; base = 16; }
    else if (base == 0) base = p[0] == '0' ? 8 : 10;
    unsigned long long v = 0;
    const char *start = p;
    for (;; p++) {
        int d = (*p >= '0' && *p <= '9') ? *p - '0' : (*p >= 'a' && *p <= 'z') ? *p - 'a' + 10 : (*p >= 'A' && *p <= 'Z') ? *p - 'A' + 10 : 99;
        if (d >= base) break;
        v = v * base + d;
    }
    if (end) *end = (char *)(p == start ? s : p);
    return neg ? -v : v;
}
extern "C" long long strtoll(const char *s, char **end, int base) { return (long long)strtoull(s, end, base); }
