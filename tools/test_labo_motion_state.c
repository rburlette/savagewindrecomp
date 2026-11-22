// Synthetic IPC requests exercise the actual AM handler; no game data or guest execution.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../runtime/services/am.c"

static bool test_pc_vr;
bool pc_vr_enabled(void) { return test_pc_vr; }

// These unrelated dependencies must not be reached by the tested commands.
KMutex g_klock;  // (single-threaded fixture: the kernel lock is a no-op)
void klock(void) {}
void kmutex_unlock(KMutex *m) { (void)m; }
const uint8_t g_user_id[16] = {0};
KEvent *kernel_create_event_pair(uint32_t *w, uint32_t *r) {
    (void)w; (void)r; abort();
}
void kernel_signal(KObject *o) { (void)o; abort(); }
Service *service_new(const char *name, ServiceHandler handler, void *state) {
    (void)name; (void)handler; (void)state; abort();
}
void ipc_return_object(IpcResp *resp, Service *service) { (void)resp; (void)service; abort(); }
const char *language_text_code(void) { abort(); }

static unsigned checks;
static void check(int ok, const char *what) {
    ++checks;
    if (!ok) { fprintf(stderr, "FAIL: %s\n", what); exit(1); }
}

static void set_profile(const char *value) {
#ifdef _WIN32
    check(_putenv_s("SAVAGEWIND_LABO_MOTION", value ? value : "") == 0, "set environment");
#else
    check(value ? setenv("SAVAGEWIND_LABO_MOTION", value, 1) == 0 :
                  unsetenv("SAVAGEWIND_LABO_MOTION") == 0, "set environment");
#endif
    am_reset();  // An in-place boot may re-read settings and must clear VR state.
}

static void query(uint32_t cmd, uint32_t value, uint32_t size) {
    Service service = {.name = "ICommonStateGetter", .handler = common_state_getter};
    Service original_service = service;
    IpcReq req = {.cmd = cmd};
    IpcReq original_req = req;
    struct { uint64_t before; IpcResp response; uint64_t after; } guarded = {0};
    guarded.before = 0x123456789abcdef0ull;
    guarded.after = 0xfedcba9876543210ull;
    service.handler(&service, &req, &guarded.response);
    IpcResp expected = {0};
    if (size == 1) expected.data[0] = (uint8_t)value;
    else if (size == 4) memcpy(expected.data, &value, sizeof value);
    expected.data_size = size;
    check(guarded.response.result == 0, "handler succeeds");
    check(guarded.response.data_size == size, "exact output width");
    check(memcmp(&guarded.response, &expected, sizeof expected) == 0,
          "exact payload with no handles or objects");
    check(memcmp(&req, &original_req, sizeof req) == 0, "request unchanged");
    check(memcmp(&service, &original_service, sizeof service) == 0, "service unchanged");
    check(guarded.before == 0x123456789abcdef0ull &&
          guarded.after == 0xfedcba9876543210ull, "response boundaries unchanged");
}

int main(void) {
    set_profile(NULL);
    check(!labo_motion_enabled() && !labo_motion_active(), "default disabled");
    check(!labo_motion_sample_acquire(), "default sample acquire fails without holding lock");
    query(5, 1, 1); query(6, 1, 4);
    query(50, 0, 0); query(53, 0, 0); query(50, 0, 0);
    check(!labo_motion_active(), "default begin has no effect");
    query(54, 0, 0);

    for (size_t i = 0; i < 3; ++i) {
        const char *invalid[] = {"true", "01", "2"};
        set_profile(invalid[i]);
        check(!labo_motion_enabled(), "invalid opt-in disabled");
        query(5, 1, 1); query(6, 1, 4);
        query(53, 0, 0);
        check(!labo_motion_active(), "invalid begin has no effect");
    }

    set_profile("1");
    check(labo_motion_enabled() && !labo_motion_active(), "strict opt-in enabled and inactive");
    check(!labo_motion_sample_acquire(), "inactive sample acquire fails without holding lock");
    query(5, 0, 1); query(6, 0, 4);
    query(50, 0, 4);
    query(53, 0, 0);
    check(labo_motion_active(), "begin activates state");
    check(labo_motion_sample_acquire(), "active sample acquire succeeds");
    labo_motion_sample_release();
    query(50, 1, 4);
    query(53, 0, 0); query(50, 1, 4);  // idempotent begin
    query(54, 0, 0);
    check(!labo_motion_active(), "end deactivates state");
    query(50, 0, 4);
    query(54, 0, 0); query(50, 0, 4);  // idempotent end
    query(53, 0, 0);
    am_reset();
    check(labo_motion_enabled() && !labo_motion_active(), "new boot resets active state");
    query(50, 0, 4);

    set_profile(NULL);
    query(5, 1, 1); query(6, 1, 4); query(50, 0, 0);
    check(!labo_motion_enabled() && !labo_motion_active(), "profile disabled after reset");
    test_pc_vr = true;
    set_profile("1"); // Production VR must override an inherited handheld diagnostic.
    check(!labo_motion_enabled(), "PC VR leaves handheld diagnostic disabled");
    query(5, 1, 1); query(6, 1, 4); // docked + boost quality retained
    query(50, 0, 4); query(53, 0, 0); query(50, 1, 4);
    check(labo_motion_sample_acquire(), "PC VR sample delivery enabled without handheld mode");
    labo_motion_sample_release();
    query(54, 0, 0); query(50, 0, 4);
    check(!labo_motion_sample_acquire(), "PC VR end prevents sample delivery");
    query(53, 0, 0);
    am_reset();
    query(50, 0, 4);
    query(5, 1, 1); query(6, 1, 4);
    check(!labo_motion_active(), "PC VR reset clears lifecycle while retaining quality");
    set_profile(NULL);
    check(!labo_motion_enabled(), "PC VR without the diagnostic stays docked");
    query(5, 1, 1); query(6, 1, 4);
    test_pc_vr = false;
    printf("PASS: %u AM Labo motion checks; actual handler, strict opt-in and reset\n", checks);
    return 0;
}
