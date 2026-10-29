// Intents for apps (docs/intents.md). An app that declares intents in app.json implements
//     void on_intent(const Intent &in, IntentResult &out);
// The OS starts the app for one intent: setup()/loop() do not run, and the app exits when this returns.
#pragma once
#include <cpos/abi.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// "key=value;key=value" arguments
struct Intent {
    const char *name;
    const char *args;
    // copies the value of `key` into buf; false if absent
    bool get(const char *key, char *buf, size_t n) const {
        size_t kl = strlen(key);
        for (const char *p = args; *p;) {
            if (!strncmp(p, key, kl) && p[kl] == '=') {
                const char *v = p + kl + 1;
                size_t l = 0;
                while (v[l] && v[l] != ';') l++;
                if (l >= n) l = n - 1;
                memcpy(buf, v, l);
                buf[l] = 0;
                return true;
            }
            while (*p && *p != ';') p++;
            if (*p == ';') p++;
        }
        return false;
    }
    bool has(const char *key) const { char b[2]; return get(key, b, sizeof(b)); }
    int i(const char *key, int def = 0) const { char b[16]; return get(key, b, sizeof(b)) ? atoi(b) : def; }
    bool is(const char *n) const { return !strcmp(name, n); }
};

struct IntentResult {
    int status = CPOS_INTENT_FAILED;
    char value[CPOS_INTENT_VALUE_MAX] = {0};
    void ok() { status = CPOS_INTENT_OK; value[0] = 0; }
    void ok(const char *v) { status = CPOS_INTENT_OK; snprintf(value, sizeof(value), "%s", v); }
    void ok(int v) { status = CPOS_INTENT_OK; snprintf(value, sizeof(value), "%d", v); }
    void ok(bool v) { status = CPOS_INTENT_OK; snprintf(value, sizeof(value), "%s", v ? "true" : "false"); }
    // launch: reply ok and then keep running as a normal app (setup()/loop()); setup() can read startIntent()
    bool handoff = false;
    void launch() { status = CPOS_INTENT_OK; value[0] = 0; handoff = true; }
    void error(int code) { status = code; value[0] = 0; }
    // records and lists are built piece by piece: field("note","La"); item("name")
    void field(const char *k, const char *v) { append(k, "=", v, ";"); }
    void field(const char *k, int v) { char b[16]; snprintf(b, sizeof(b), "%d", v); append(k, "=", b, ";"); }
    void item(const char *v) { append("", "", v, "|"); }
    void done() { status = CPOS_INTENT_OK; size_t l = strlen(value); if (l && (value[l - 1] == ';' || value[l - 1] == '|')) value[l - 1] = 0; }
  private:
    void append(const char *a, const char *b, const char *c, const char *sep) {
        size_t l = strlen(value);
        snprintf(value + l, sizeof(value) - l, "%s%s%s%s", a, b, c, sep);
    }
};

void on_intent(const Intent &in, IntentResult &out) __attribute__((weak));

// Non-null in setup()/loop() when the OS started the app with a launch intent.
extern const Intent *cpos_start_intent;
inline const Intent *startIntent() { return cpos_start_intent; }
