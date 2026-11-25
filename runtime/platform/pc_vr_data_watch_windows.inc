// Part of windows.c (D133): PC VR's data-watch audit: CPU data breakpoints.
// Included where it stands by windows.c, so it compiles in windows.c's translation unit as before.

// [pc-vr-audit] (debug) CPU data breakpoints on up to four 8-byte words, read or write, set on every thread the
// process has when armed (threads started later are not covered); runtime/dispatch.c rt_data_watch_hit reports them.
static _Atomic bool g_data_watch_armed;
static uint64_t g_data_watch_addr[4];

static DWORD WINAPI data_watch_arm(void *unused) {
    (void)unused;
    DWORD64 dr7 = 0;
    for (unsigned i = 0; i < 4; i++)
        if (g_data_watch_addr[i]) dr7 |= (1ull << (2 * i)) | (0xbull << (16 + 4 * i));  // local; read/write, 8 bytes
    atomic_store(&g_data_watch_armed, true);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    const DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
    unsigned armed = 0, failed = 0;
    THREADENTRY32 te = {.dwSize = sizeof te};
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
        HANDLE h = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
        if (!h) { ++failed; continue; }
        bool set = false;
        if (SuspendThread(h) != (DWORD)-1) {
            CONTEXT ctx = {.ContextFlags = CONTEXT_DEBUG_REGISTERS};
            if (GetThreadContext(h, &ctx)) {
                ctx.Dr0 = g_data_watch_addr[0];
                ctx.Dr1 = g_data_watch_addr[1];
                ctx.Dr2 = g_data_watch_addr[2];
                ctx.Dr3 = g_data_watch_addr[3];
                ctx.Dr6 = 0;
                ctx.Dr7 = dr7;
                set = SetThreadContext(h, &ctx) != 0;
            }
            ResumeThread(h);
        }
        set ? ++armed : ++failed;
        CloseHandle(h);
    }
    CloseHandle(snap);
    fprintf(stderr, "[pc-vr-audit] data watch armed on %u threads (%u failed): %llx %llx %llx %llx\n", armed, failed,
            (unsigned long long)g_data_watch_addr[0], (unsigned long long)g_data_watch_addr[1],
            (unsigned long long)g_data_watch_addr[2], (unsigned long long)g_data_watch_addr[3]);
    return 0;
}

bool plat_data_watch(const uint64_t addr[4]) {
    // (armed again with other addresses: the control endpoint's `watch` command, runtime/dispatch.c)
    for (unsigned i = 0; i < 4; i++) g_data_watch_addr[i] = addr[i] & ~7ull;
    HANDLE t = CreateThread(NULL, 0, data_watch_arm, NULL, 0, NULL);  // (a thread cannot set its own debug registers)
    if (!t) return false;
    CloseHandle(t);
    return true;
}
