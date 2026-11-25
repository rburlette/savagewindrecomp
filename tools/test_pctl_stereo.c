// Public synthetic IPC requests exercise the actual service handler. No game data, profiles or OS controls.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../runtime/services/misc.c"

// Unrelated service dependencies must never be invoked by these no-I/O permission queries.
uint64_t g_program_id;
KEvent *kernel_create_event_pair(uint32_t *w, uint32_t *r) { (void)w; (void)r; abort(); }
Service *service_new(const char *name, ServiceHandler handler, void *state) {
    (void)name; (void)handler; (void)state; abort();
}
void ipc_return_object(IpcResp *resp, Service *service) { (void)resp; (void)service; abort(); }

static unsigned checks;
static void check(int ok, const char *what) {
    checks++;
    if (!ok) { fprintf(stderr, "FAIL: %s\n", what); exit(1); }
}

static void query(uint32_t cmd, uint8_t value, uint32_t size) {
    Service service = {0};
    service.name = "IParentalControlService";
    service.handler = pctl_service;
    Service service_before;
    memcpy(&service_before, &service, sizeof service);
    IpcReq req = {0};
    req.cmd = cmd;
    IpcReq req_before;
    memcpy(&req_before, &req, sizeof req);
    struct { uint64_t before; IpcResp response; uint64_t after; } guarded = {0};
    guarded.before = 0x123456789abcdef0ull;
    guarded.after = 0xfedcba9876543210ull;
    // ipc.c constructs IpcResp = {0}; use exactly that production precondition.
    service.handler(&service, &req, &guarded.response);
    check(guarded.response.result == 0, "query returns success");
    check(guarded.response.data_size == size, "query declares exact response payload size");
    check(guarded.response.data[0] == value, "query returns the no-profile permission value");
    IpcResp expected = {0};
    expected.data[0] = value;
    expected.data_size = size;
    check(!memcmp(&guarded.response, &expected, sizeof expected), "no other payload, handles or objects returned");
    check(!memcmp(&req, &req_before, sizeof req), "request unchanged");
    check(!memcmp(&service, &service_before, sizeof service), "service has no policy state mutation");
    check(guarded.before == 0x123456789abcdef0ull && guarded.after == 0xfedcba9876543210ull,
          "response boundaries unchanged");
}

int main(void) {
    // 1065 is a positive permission query; a generic zeroed-success reply incorrectly means forbidden.
    for (int i = 0; i < 8; i++) query(1065, 1, 1);
    // The already established no-profile model stays unchanged across permission queries.
    query(1031, 0, 1);  // IsRestrictionEnabled: false
    query(1032, 0, 4);  // existing safety-level response
    query(1018, 0, 1);  // existing restriction response
    query(1065, 1, 1);
    printf("PASS: %u synthetic pctl checks; actual handler, no profile or guest execution\n", checks);
    return 0;
}
