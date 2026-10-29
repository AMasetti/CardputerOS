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
    while (!api->exit_requested()) loop();
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
