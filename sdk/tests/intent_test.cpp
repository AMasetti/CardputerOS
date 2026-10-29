// Host test for the app-side intent helpers: g++ -std=c++17 -Isdk/include -Isdk/shim/include sdk/tests/intent_test.cpp
#include <cpos_intent.h>
#include <stdio.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

int main()
{
    Intent in{"light.color", "r=255;g=0;b=128;ip=192.168.1.20;name=Ocean view"};
    CHECK(in.i("r") == 255 && in.i("g", 9) == 0 && in.i("b") == 128);
    CHECK(in.i("missing", 7) == 7 && !in.has("x") && in.has("ip"));
    char ip[20], nm[24], small[4];
    CHECK(in.get("ip", ip, sizeof(ip)) && !strcmp(ip, "192.168.1.20"));
    CHECK(in.get("name", nm, sizeof(nm)) && !strcmp(nm, "Ocean view"));
    CHECK(in.get("name", small, sizeof(small)) && !strcmp(small, "Oce"));  // truncated, still terminated
    CHECK(!in.has("r2") && !in.has("g=") && in.is("light.color") && !in.is("light"));
    // a key that is a prefix of another must not match it
    Intent in2{"x.y", "rate=5;r=1"};
    CHECK(in2.i("r") == 1 && in2.i("rate") == 5);

    IntentResult r;
    CHECK(r.status == CPOS_INTENT_FAILED);  // not answered = failed
    r.ok(42);
    CHECK(r.status == 0 && !strcmp(r.value, "42"));
    r.ok(true);
    CHECK(!strcmp(r.value, "true"));
    r.ok();
    CHECK(r.value[0] == 0);
    IntentResult rec;
    rec.field("note", "La"); rec.field("cents", -3); rec.done();
    CHECK(rec.status == 0 && !strcmp(rec.value, "note=La;cents=-3"));
    IntentResult lst;
    lst.item("192.168.1.2"); lst.item("192.168.1.3"); lst.done();
    CHECK(!strcmp(lst.value, "192.168.1.2|192.168.1.3"));
    IntentResult l2;
    l2.launch();
    CHECK(l2.handoff && l2.status == 0);
    printf(fails ? "FAILED\n" : "PASS\n");
    return fails;
}
