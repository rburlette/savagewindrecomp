// PC VR experiment: what the game's threads are in while a frame's present is overdue (pc_vr_frame_trace.h). Upstream's
// profiler answers the same for every thread all the time, but a capture slows the command thread enough to hold the
// game's thread itself (2026-11-16: no stall in a check without it, 188 stalled frames with it). This watches one
// thing and costs nothing until it happens: once a frame's present is STALL_AFTER_NS overdue, the thread that runs the
// game's update and the one that presents are sampled every couple of milliseconds until the present comes.
//
// A sample is the thread's instruction address and the code addresses found on its stack, nearest first: a scan, not
// an unwind (no tables are read and nothing is locked while the thread is suspended). Addresses are written against
// their module (the runtime's are named afterwards from the build, scripts/summarize-pc-vr-stalls.py); the game's own
// code is written as "game" alone, never an address.
#include "pc_vr_frame_trace.h"

#include <stdatomic.h>
#include <string.h>

#include "platform.h"
#include "rt.h"

#ifdef _WIN32
#include <windows.h>
#define PSAPI_VERSION 2
#include <psapi.h>
#include <pthread.h>

enum {
    STALL_STACK_WORDS = 1536,  // stack words scanned above the stack pointer (12 KB)
    STALL_ADDRESSES = 14,      // code addresses kept from a scan
    STALL_SAMPLES = 40,        // samples of one overdue frame (a load's frames are overdue for seconds)
    STALL_LINES = 4000,        // sample lines in a run
    STALL_MODULES = 256
};
#define STALL_AFTER_NS 8000000ull  // overdue: the present has not come this long after the frame's start
#define STALL_EVERY_MS 2

typedef struct {
    HANDLE handle;
    DWORD id;
    uintptr_t stack_base;  // the highest address of its stack: the scan stops there
} StallThread;
// [0] the thread that runs the game's update, [1] the one that presents (the same thread in both when they are one).
static StallThread stall_threads[2];
static atomic_int stall_registered[2];

static struct { uintptr_t low, high; char name[40]; bool game, runtime; } stall_modules[STALL_MODULES];
static unsigned stall_module_count;

static _Atomic uint64_t stall_frame_start_ns;  // the frame being waited for; 0: none, or its present has come
static _Atomic uint64_t stall_after_present;
static atomic_uint stall_frame_samples;
static atomic_bool stall_watching;

static void stall_modules_list(void) {
    HMODULE modules[STALL_MODULES];
    DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), modules, sizeof modules, &needed)) return;
    const HMODULE runtime = GetModuleHandleA(NULL);
    for (DWORD index = 0; index < needed / sizeof *modules && stall_module_count < STALL_MODULES; ++index) {
        MODULEINFO info;
        if (!GetModuleInformation(GetCurrentProcess(), modules[index], &info, sizeof info)) continue;
        char name[MAX_PATH] = "";
        GetModuleBaseNameA(GetCurrentProcess(), modules[index], name, sizeof name);
        const unsigned at = stall_module_count++;
        stall_modules[at].low = (uintptr_t)info.lpBaseOfDll;
        stall_modules[at].high = (uintptr_t)info.lpBaseOfDll + info.SizeOfImage;
        stall_modules[at].runtime = modules[index] == runtime;
        stall_modules[at].game = !_strnicmp(name, "swgame", 6);
        snprintf(stall_modules[at].name, sizeof stall_modules[at].name, "%s", name);
    }
}

static int stall_module_of(uintptr_t address) {
    for (unsigned index = 0; index < stall_module_count; ++index)
        if (address >= stall_modules[index].low && address < stall_modules[index].high) return (int)index;
    return -1;
}

// One address as text: "exe+0x1a2b" for the runtime, "game" for the game's code, "<module>+0x..." for the rest.
static size_t stall_address_text(uintptr_t address, char *text, size_t size) {
    const int module = stall_module_of(address);
    if (module < 0) return (size_t)snprintf(text, size, " ?");
    if (stall_modules[module].game) return (size_t)snprintf(text, size, " game");
    return (size_t)snprintf(text, size, " %s+0x%llx", stall_modules[module].runtime ? "exe" : stall_modules[module].name,
                            (unsigned long long)(address - stall_modules[module].low));
}

// The thread stopped for the length of a context read and a copy of its stack; the scan and the line come after it runs
// again: a suspended thread may hold the log's lock.
static void stall_sample(const StallThread *thread, const char *role, uint64_t after_present, double overdue_ms) {
    static uintptr_t words[STALL_STACK_WORDS];  // (the watching thread alone)
    CONTEXT context;
    context.ContextFlags = CONTEXT_CONTROL;
    uintptr_t at = 0;
    unsigned copied = 0;
    if (SuspendThread(thread->handle) == (DWORD)-1) return;
    if (GetThreadContext(thread->handle, &context)) {
        at = (uintptr_t)context.Rip;
        const uintptr_t stack = (uintptr_t)context.Rsp;
        if (stack && stack < thread->stack_base && !(stack & 7)) {
            const uintptr_t available = (thread->stack_base - stack) / sizeof(uintptr_t);
            const SIZE_T wanted = (available < STALL_STACK_WORDS ? (SIZE_T)available : STALL_STACK_WORDS) * sizeof(uintptr_t);
            // (the system's copy, not memcpy: it stops at memory that cannot be read instead of faulting)
            SIZE_T read = 0;
            ReadProcessMemory(GetCurrentProcess(), (const void *)stack, words, wanted, &read);
            copied = (unsigned)(read / sizeof(uintptr_t));
        }
    }
    ResumeThread(thread->handle);
    if (!at) return;
    char text[1024];
    size_t length = (size_t)snprintf(text, sizeof text, "[pc-vr] stall sample: after present %llu, %.1f ms into the frame, "
                                     "%s thread %lu: at", (unsigned long long)after_present, overdue_ms, role,
                                     (unsigned long)thread->id);
    length += stall_address_text(at, text + length, sizeof text - length);
    length += (size_t)snprintf(text + length, sizeof text - length, "; stack");
    unsigned kept = 0, game_run = 0;
    for (unsigned index = 0; index < copied && kept < STALL_ADDRESSES && sizeof text - length > 96; ++index) {
        const int module = stall_module_of(words[index]);
        if (module < 0) continue;
        if (stall_modules[module].game) {  // (a run of the game's own code is written once)
            if (!game_run++) length += (size_t)snprintf(text + length, sizeof text - length, " game"), ++kept;
            continue;
        }
        game_run = 0;
        length += stall_address_text(words[index], text + length, sizeof text - length);
        ++kept;
    }
    LOG("%s\n", text);
}

static void *stall_watch(void *unused) {
    (void)unused;
    unsigned lines = 0;
    stall_modules_list();
    LOG("[pc-vr] stall sampler: watching for a present %.0f ms overdue (%u modules)\n", STALL_AFTER_NS / 1e6, stall_module_count);
    while (lines < STALL_LINES) {
        Sleep(STALL_EVERY_MS);
        const uint64_t start_ns = atomic_load(&stall_frame_start_ns);
        if (!start_ns) continue;
        const uint64_t now_ns = plat_now_ns();
        if (now_ns < start_ns + STALL_AFTER_NS || atomic_load(&stall_frame_samples) >= STALL_SAMPLES) continue;
        atomic_fetch_add(&stall_frame_samples, 1);
        const double overdue_ms = (double)(now_ns - start_ns) / 1e6;
        const uint64_t after_present = atomic_load(&stall_after_present);
        const bool update = atomic_load(&stall_registered[0]) == 2, present = atomic_load(&stall_registered[1]) == 2;
        const bool one_thread = update && present && stall_threads[0].id == stall_threads[1].id;
        if (update) stall_sample(&stall_threads[0], one_thread ? "update and present" : "update", after_present, overdue_ms), ++lines;
        if (present && !one_thread) stall_sample(&stall_threads[1], "present", after_present, overdue_ms), ++lines;
    }
    LOG("[pc-vr] stall sampler: %u lines written, no more in this run\n", lines);
    return NULL;
}

void pc_vr_stall_sampler_register(bool presenting) {
    if (!pc_vr_frame_trace_on()) return;
    const unsigned role = presenting ? 1 : 0;
    int expected = 0;
    if (!atomic_compare_exchange_strong(&stall_registered[role], &expected, 1)) return;  // (the first thread in the role)
    StallThread *thread = &stall_threads[role];
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &thread->handle, 0, FALSE,
                         DUPLICATE_SAME_ACCESS)) return;
    thread->id = GetCurrentThreadId();
    thread->stack_base = (uintptr_t)((NT_TIB *)NtCurrentTeb())->StackBase;
    atomic_store(&stall_registered[role], 2);
    LOG("[pc-vr] stall sampler: the game's %s runs on thread %lu\n", presenting ? "present" : "update", (unsigned long)thread->id);
}

void pc_vr_stall_sampler_frame(uint64_t start_ns, uint64_t after_present_id) {
    if (!pc_vr_frame_trace_on()) return;
    atomic_store(&stall_after_present, after_present_id);
    atomic_store(&stall_frame_samples, 0);
    atomic_store(&stall_frame_start_ns, start_ns);
    if (!atomic_exchange(&stall_watching, true)) {
        pthread_t watcher;
        if (pthread_create(&watcher, NULL, stall_watch, NULL) == 0) pthread_detach(watcher);
    }
}

void pc_vr_stall_sampler_presented(void) {
    atomic_store(&stall_frame_start_ns, 0);
}

#else  // (the sampler stops and reads another thread through the Windows API; elsewhere the trace goes without it)
void pc_vr_stall_sampler_register(bool presenting) { (void)presenting; }
void pc_vr_stall_sampler_frame(uint64_t start_ns, uint64_t after_present_id) { (void)start_ns; (void)after_present_id; }
void pc_vr_stall_sampler_presented(void) {}
#endif
