// Native test of generated glue: plays the canonical-ABI host's role, calling
// the export functions wit-bindgen declared and recording the signal imports.
#include "component.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

static std::map<int32_t, exports_demo_counter_counter_counter_t *> g_handles;
static int32_t g_next = 1;
static std::vector<std::string> g_events;

extern "C" {
exports_demo_counter_counter_own_counter_t exports_demo_counter_counter_counter_new(exports_demo_counter_counter_counter_t *rep) {
    g_handles[g_next] = rep;
    return {g_next++};
}
void demo_counter_counter_events_value_changed(uint32_t source, int32_t value) {
    g_events.push_back("value-changed " + std::to_string(source) + " " + std::to_string(value));
}
void demo_counter_counter_events_overflowed(uint32_t source, component_string_t *why, int64_t at) {
    g_events.push_back("overflowed " + std::to_string(source) + " " + std::string((char *)why->ptr, why->len) + " " + std::to_string(at));
}
}

static component_string_t str(const char *s) {
    component_string_t r;
    r.len = strlen(s);
    r.ptr = (uint8_t *)malloc(r.len);
    memcpy(r.ptr, s, r.len);
    return r;
}

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)

int main() {
    auto h = exports_demo_counter_counter_constructor_counter();
    auto *self = g_handles[h.__handle];
    CHECK(exports_demo_counter_counter_method_counter_id(self) == 1);
    CHECK(exports_demo_counter_counter_method_counter_value(self) == 0);
    exports_demo_counter_counter_method_counter_set_value(self, 5);
    exports_demo_counter_counter_method_counter_step(self);
    CHECK(exports_demo_counter_counter_method_counter_value(self) == 6);
    exports_demo_counter_counter_method_counter_set_mode(self, DEMO_COUNTER_TYPES_MODE_DOUBLING);
    exports_demo_counter_counter_method_counter_step(self);
    CHECK(exports_demo_counter_counter_method_counter_value(self) == 12);

    component_string_t prefix = str("n=");  // ownership passes to the export
    component_string_t out;
    exports_demo_counter_counter_method_counter_describe(self, &prefix, &out);
    CHECK(std::string((char *)out.ptr, out.len) == "n=12 (doubling)");
    free(out.ptr);

    component_list_f64_t hist;
    exports_demo_counter_counter_method_counter_history(self, 2, &hist);
    CHECK(hist.len == 2 && hist.ptr[0] == 6 && hist.ptr[1] == 12);
    free(hist.ptr);

    exports_demo_counter_counter_method_counter_set_value(self, 0x40000000);
    exports_demo_counter_counter_method_counter_step(self);  // 2^31 overflows int
    CHECK(exports_demo_counter_counter_method_counter_value(self) == 0x40000000);

    std::vector<std::string> want = {
        "value-changed 1 5", "value-changed 1 6", "value-changed 1 12",
        "value-changed 1 1073741824", "overflowed 1 past INT_MAX 2147483648",
    };
    for (auto &e : g_events) fprintf(stderr, "event: %s\n", e.c_str());
    CHECK(g_events == want);

    // QTimer::singleShot(0) queued by step() ran when those calls returned.
    exports_demo_counter_counter_counter_destructor(self);
    printf("native glue test: OK\n");
    return 0;
}
