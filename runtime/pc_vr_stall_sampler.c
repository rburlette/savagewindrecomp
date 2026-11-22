// PC VR experiment: what the game's threads are in during a stall of a frame (pc_vr_frame_trace.h). Upstream's
// profiler answers the same for every thread all the time, but a capture slows the command thread enough to hold the
// game's thread itself (2026-11-16: no stall in a check without it, 188 stalled frames with it). This watches one
// thing and costs nothing until it happens.
//
// A stall: a frame has started, no stage of it has moved for STALL_AFTER_NS (its update beginning, a sync wait ending,
// lists submitted), and its presenting thread is not waiting on a sync. That last part matters: where the GPU cannot
// hold the pace, every frame waits on a sync for longer than that, and a sampler that counted those spent its whole
// run on them (the user's session of 2026-11-16: its 4,000 lines gone in the first 355 frames of play). While a stall
// lasts, the thread that runs the game's update and the one that presents are read every couple of milliseconds, and
// once in each stall every other thread of the game and the command thread: those in a wait are counted, the rest
// written.
//
// A read is the thread's instruction address and the return addresses of an unwind of its stack. The thread is stopped
// only for a context read and a copy of its stack; the unwind runs on the copy afterwards, when the thread runs again
// (a stopped thread may hold the log's lock, the heap's or the loader's), and a fault in it ends that one read. Where
// an unwind finds nothing, the code addresses on the copy are written instead, marked as a scan. Addresses are written
// against their module (named afterwards, scripts/summarize-pc-vr-stalls.py); the game's own code is written as "game"
// alone, never an address.
#include "pc_vr_frame_trace.h"

#include <stdatomic.h>
#include <stddef.h>
#include <string.h>

#include "kernel.h"
#include "platform.h"
#include "rt.h"

#ifdef _WIN32
#include <windows.h>
#define PSAPI_VERSION 2
#include <psapi.h>
#include <pthread.h>

enum {
    STALL_STACK_BYTES = 64 << 10,  // of a thread's stack, from its stack pointer up: the frames nearest where it is
    STALL_STACK_SLACK = 64 << 10,  // zeros behind the copy: an unwind may read past its end
    STALL_FRAMES = 40,             // return addresses kept from an unwind
    STALL_SCANNED = 14,            // code addresses kept from a scan
    STALL_SAMPLES = 40,            // reads of one stalled frame (a load's frames stall for seconds)
    STALL_SWEEP_AT = 2,            // the read of a stalled frame at which every other thread is read once (the third)
    STALL_SWEEPS = 200,            // of those in a run
    STALL_LINES = 20000,           // lines in a run
    STALL_THREADS = 128,
    STALL_MODULES = 512,
    STALL_WAIT_CALLS = 8
};
#define STALL_AFTER_NS 14000000ull  // (where the GPU bounds the frame, an update begins up to 12 ms after the start)
#define STALL_EVERY_MS 2
#define STALL_MODULES_EVERY_NS 10000000000ull  // the list of loaded modules is read again this long after the last

typedef struct {
    HANDLE handle;
    DWORD id;
    uintptr_t stack_top;  // the highest address of its stack: the copy stops there
} StallThread;
// [0] the thread that runs the game's update, [1] the one that presents (the same thread in both when they are one).
static StallThread stall_threads[2];
static atomic_int stall_registered[2];

static struct { uintptr_t low, high; char name[40]; bool game, runtime; } stall_modules[STALL_MODULES];
static unsigned stall_module_count;
static uint64_t stall_modules_listed_ns;
static uintptr_t stall_wait_calls[STALL_WAIT_CALLS];  // the system's calls a thread waits in
static unsigned stall_wait_call_count;

static _Atomic uint64_t stall_frame_start_ns;  // the frame being watched; 0: none, or its present has come
static _Atomic uint64_t stall_moved_ns;        // when a stage of it last moved
static atomic_bool stall_gpu_wait;             // its presenting thread is in a sync wait
static _Atomic uint64_t stall_after_present;
static atomic_uint stall_frame_samples;
static atomic_bool stall_watching;

// The watching thread's alone: the copy of a stack, and how much of it the last copy filled.
static _Alignas(16) uint8_t stall_stack[STALL_STACK_BYTES + STALL_STACK_SLACK];
static size_t stall_stack_filled;

// (never while a thread is stopped: listing the modules takes the loader's lock)
static void stall_modules_list(void) {
    static HMODULE modules[STALL_MODULES];
    DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), modules, sizeof modules, &needed)) return;
    const HMODULE runtime = GetModuleHandleA(NULL);
    stall_module_count = 0;
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
    stall_modules_listed_ns = plat_now_ns();
}

static int stall_module_of(uintptr_t address) {
    for (unsigned index = 0; index < stall_module_count; ++index)
        if (address >= stall_modules[index].low && address < stall_modules[index].high) return (int)index;
    return -1;
}

static void stall_wait_calls_list(void) {
    static const char *const names[STALL_WAIT_CALLS] = {
        "NtWaitForSingleObject", "NtWaitForMultipleObjects", "NtWaitForAlertByThreadId", "NtDelayExecution",
        "NtWaitForKeyedEvent", "NtSignalAndWaitForSingleObject", "NtRemoveIoCompletion", "NtRemoveIoCompletionEx"};
    const HMODULE system = GetModuleHandleA("ntdll.dll");
    for (unsigned index = 0; system && index < STALL_WAIT_CALLS; ++index) {
        const FARPROC call = GetProcAddress(system, names[index]);
        if (call) stall_wait_calls[stall_wait_call_count++] = (uintptr_t)(void *)call;
    }
}

// (each of those calls is 32 bytes of code; a thread waiting in one stands behind its system call instruction)
static bool stall_in_wait_call(uintptr_t address) {
    for (unsigned index = 0; index < stall_wait_call_count; ++index)
        if (address >= stall_wait_calls[index] && address < stall_wait_calls[index] + 32) return true;
    return false;
}

// The top of a thread's stack, from its thread block (read through the system's copy: the thread may have ended).
static uintptr_t stall_stack_top(HANDLE thread) {
    typedef LONG(NTAPI * QueryThread)(HANDLE, int, void *, ULONG, ULONG *);
    static QueryThread query;
    if (!query) query = (QueryThread)(void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationThread");
    struct { LONG status; void *block; uintptr_t process, thread, affinity; LONG priority, base_priority; } basic;
    if (!query || query(thread, 0, &basic, sizeof basic, NULL) < 0 || !basic.block) return 0;
    uintptr_t top = 0;
    SIZE_T read = 0;
    ReadProcessMemory(GetCurrentProcess(), (const char *)basic.block + offsetof(NT_TIB, StackBase), &top, sizeof top, &read);
    return read == sizeof top ? top : 0;
}

// One address as text: "exe+0x1a2b" for the runtime, "game" for the game's code, "<module>+0x..." for the rest.
static size_t stall_address_text(uintptr_t address, char *text, size_t size) {
    const int module = stall_module_of(address);
    if (module < 0) return (size_t)snprintf(text, size, " ?");
    if (stall_modules[module].game) return (size_t)snprintf(text, size, " game");
    return (size_t)snprintf(text, size, " %s+0x%llx", stall_modules[module].runtime ? "exe" : stall_modules[module].name,
                            (unsigned long long)(address - stall_modules[module].low));
}

// The thread stopped for the length of a context read and a copy of its stack into stall_stack, nothing else: no
// lock is taken while it is stopped. waiting: it stands in one of the system's waits (with leave_waiting its stack is
// not copied then).
static bool stall_stop_and_read(HANDLE thread, uintptr_t stack_top, bool leave_waiting, CONTEXT *context, size_t *copied,
                                bool *waiting) {
    *copied = 0;
    *waiting = false;
    memset(context, 0, sizeof *context);
    context->ContextFlags = CONTEXT_FULL;
    if (SuspendThread(thread) == (DWORD)-1) return false;
    const bool read = GetThreadContext(thread, context) != 0;
    if (read) {
        *waiting = stall_in_wait_call((uintptr_t)context->Rip);
        const uintptr_t stack = (uintptr_t)context->Rsp;
        if (!(leave_waiting && *waiting) && stack && stack < stack_top && !(stack & 7)) {
            const uintptr_t available = stack_top - stack;
            // (the system's copy, not memcpy: it stops at memory that cannot be read instead of faulting)
            SIZE_T done = 0;
            ReadProcessMemory(GetCurrentProcess(), (const void *)stack, stall_stack,
                              available < STALL_STACK_BYTES ? (SIZE_T)available : STALL_STACK_BYTES, &done);
            *copied = (size_t)done & ~(size_t)7;
        }
    }
    ResumeThread(thread);
    if (*copied < stall_stack_filled) memset(stall_stack + *copied, 0, stall_stack_filled - *copied);  // (zeros behind it)
    if (read && !(leave_waiting && *waiting)) stall_stack_filled = *copied;
    return read;
}

// The return addresses of the stack copied into stall_stack, nearest first, the thread's own place among them as the
// first. The copy stands at another address than the stack did, so every word of it and every register that pointed
// into the copied stretch is moved to point into the copy; the system's unwinder then reads nothing but the copy and
// the modules' unwind tables. It stops at an address outside the loaded modules (a lookup of one can fault) and at the
// copy's end.
static unsigned stall_unwind(CONTEXT *context, size_t copied, uintptr_t *frames) {
    volatile unsigned count = 0;
    __try {
        const DWORD64 original = context->Rsp, low = (DWORD64)(uintptr_t)stall_stack, high = low + copied;
        const DWORD64 moved_by = low - original;
        DWORD64 *words = (DWORD64 *)(void *)stall_stack;
        for (size_t index = 0; index < copied / sizeof *words; ++index)
            if (words[index] >= original && words[index] < original + copied) words[index] += moved_by;
        DWORD64 *registers = &context->Rax;  // (Rax, Rcx, Rdx, Rbx, Rsp, Rbp, Rsi, Rdi, R8 to R15, in the context's order)
        for (unsigned index = 0; index < 16; ++index)
            if (registers[index] >= original && registers[index] < original + copied) registers[index] += moved_by;
        while (count < STALL_FRAMES && context->Rip) {
            frames[count] = (uintptr_t)context->Rip;
            count = count + 1;
            if (stall_module_of((uintptr_t)context->Rip) < 0 || context->Rsp < low || context->Rsp + 16 > high) break;
            DWORD64 image = 0;
            PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context->Rip, &image, NULL);
            if (!function) {  // a leaf without unwind data: the return address is on top of the stack
                context->Rip = *(DWORD64 *)(uintptr_t)context->Rsp;
                context->Rsp += 8;
                continue;
            }
            // (a function that addresses its frame through a register: that register must point into the copy too)
            const uint8_t *unwind_data = (const uint8_t *)(uintptr_t)(image + function->UnwindData);
            const unsigned frame_register = unwind_data[3] & 15;
            if (frame_register && (registers[frame_register] < low || registers[frame_register] + 256 > high)) break;
            void *handler_data = NULL;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, context->Rip, function, context, &handler_data, &establisher, NULL);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return count;
}

// One thread read and written as a line. 0: it could not be read, 1: written, 2: in a wait and left out.
static int stall_sample(HANDLE thread, uintptr_t stack_top, const char *role, unsigned long thread_id, bool leave_waiting,
                        uint64_t after_present, double into_ms) {
    static CONTEXT context;  // (the watching thread alone)
    size_t copied = 0;
    bool waiting = false;
    if (!stall_stop_and_read(thread, stack_top, leave_waiting, &context, &copied, &waiting) || !context.Rip) return 0;
    if (leave_waiting && waiting) return 2;
    const uintptr_t at = (uintptr_t)context.Rip;
    uintptr_t frames[STALL_FRAMES];
    const unsigned count = stall_unwind(&context, copied, frames);
    char text[2560];
    size_t length = (size_t)snprintf(text, sizeof text, "[pc-vr] stall sample: after present %llu, %.1f ms into the frame, "
                                     "%s thread %lu: at", (unsigned long long)after_present, into_ms, role, thread_id);
    length += stall_address_text(at, text + length, sizeof text - length);
    length += (size_t)snprintf(text + length, sizeof text - length, "; stack");
    bool game_before = false;
    for (unsigned index = 1; index < count && sizeof text - length > 96; ++index) {
        const int module = stall_module_of(frames[index]);
        const bool game = module >= 0 && stall_modules[module].game;
        if (!(game && game_before)) length += stall_address_text(frames[index], text + length, sizeof text - length);
        game_before = game;  // (a run of the game's own code is written once)
    }
    if (count < 3 && copied) {  // the unwind found no way up: the code addresses on the copy, which may be old ones
        length += (size_t)snprintf(text + length, sizeof text - length, "; scan");
        const uintptr_t *words = (const uintptr_t *)(const void *)stall_stack;
        unsigned kept = 0;
        game_before = false;
        for (size_t index = 0; index < copied / sizeof *words && kept < STALL_SCANNED && sizeof text - length > 96; ++index) {
            const int module = stall_module_of(words[index]);
            if (module < 0) continue;
            const bool game = stall_modules[module].game;
            if (!(game && game_before)) length += stall_address_text(words[index], text + length, sizeof text - length), ++kept;
            game_before = game;
        }
    }
    LOG("%s\n", text);
    return 1;
}

// Every other thread of the game, and the host threads the runtime's own profiler takes (the command thread), once.
static unsigned stall_sweep(uint64_t after_present, double into_ms) {
    static pthread_t hosts[STALL_THREADS];
    static uint32_t handles[STALL_THREADS];
    static uint64_t entries[STALL_THREADS];
    const int count = kernel_running_threads(hosts, handles, entries, STALL_THREADS);
    unsigned waiting = 0, written = 0, others = 0;
    for (int index = 0; index < count; ++index) {
        const HANDLE thread = (HANDLE)pthread_gethandle(hosts[index]);
        const DWORD id = thread ? GetThreadId(thread) : 0;
        if (!id || id == GetCurrentThreadId() || id == stall_threads[0].id || id == stall_threads[1].id) continue;
        ++others;
        char role[24];
        if (handles[index] >= 0xf000) snprintf(role, sizeof role, "host-%x", (unsigned)(handles[index] - 0xf000));
        else snprintf(role, sizeof role, "guest-%x", (unsigned)handles[index]);
        const int outcome = stall_sample(thread, stall_stack_top(thread), role, (unsigned long)id, true, after_present, into_ms);
        if (outcome == 2) ++waiting;
        else if (outcome == 1) ++written;
    }
    LOG("[pc-vr] stall sweep: after present %llu, %.1f ms into the frame: %u other threads, %u in a wait, %u written\n",
        (unsigned long long)after_present, into_ms, others, waiting, written);
    return written + 1;
}

static void *stall_watch(void *unused) {
    (void)unused;
    unsigned lines = 0, sweeps = 0;
    stall_modules_list();
    stall_wait_calls_list();
    LOG("[pc-vr] stall sampler: watching for a frame with no stage moved for %.0f ms outside a sync wait (%u modules)\n",
        STALL_AFTER_NS / 1e6, stall_module_count);
    while (lines < STALL_LINES) {
        Sleep(STALL_EVERY_MS);
        const uint64_t start_ns = atomic_load(&stall_frame_start_ns);
        if (!start_ns || atomic_load(&stall_gpu_wait)) continue;
        const uint64_t moved_ns = atomic_load(&stall_moved_ns);
        const uint64_t now_ns = plat_now_ns();
        if (now_ns < (moved_ns > start_ns ? moved_ns : start_ns) + STALL_AFTER_NS) continue;
        const unsigned sample = atomic_fetch_add(&stall_frame_samples, 1);
        if (sample >= STALL_SAMPLES) continue;
        if (!sample && now_ns > stall_modules_listed_ns + STALL_MODULES_EVERY_NS) stall_modules_list();
        const double into_ms = (double)(now_ns - start_ns) / 1e6;
        const uint64_t after_present = atomic_load(&stall_after_present);
        const bool update = atomic_load(&stall_registered[0]) == 2, present = atomic_load(&stall_registered[1]) == 2;
        const bool one_thread = update && present && stall_threads[0].id == stall_threads[1].id;
        if (update)
            lines += stall_sample(stall_threads[0].handle, stall_threads[0].stack_top, one_thread ? "update and present" : "update",
                                  (unsigned long)stall_threads[0].id, false, after_present, into_ms) == 1;
        if (present && !one_thread)
            lines += stall_sample(stall_threads[1].handle, stall_threads[1].stack_top, "present",
                                  (unsigned long)stall_threads[1].id, false, after_present, into_ms) == 1;
        if (sample == STALL_SWEEP_AT && sweeps < STALL_SWEEPS) {
            lines += stall_sweep(after_present, into_ms);
            ++sweeps;
        }
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
    thread->stack_top = (uintptr_t)((NT_TIB *)NtCurrentTeb())->StackBase;
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

void pc_vr_stall_sampler_moved(void) {
    if (pc_vr_frame_trace_on()) atomic_store(&stall_moved_ns, plat_now_ns());
}

void pc_vr_stall_sampler_gpu_wait(bool waiting) {
    if (!pc_vr_frame_trace_on()) return;
    atomic_store(&stall_gpu_wait, waiting);
    if (!waiting) atomic_store(&stall_moved_ns, plat_now_ns());
}

void pc_vr_stall_sampler_presented(void) {
    atomic_store(&stall_frame_start_ns, 0);
}

#else  // (the sampler stops and reads another thread through the Windows API; elsewhere the trace goes without it)

void pc_vr_stall_sampler_register(bool presenting) { (void)presenting; }
void pc_vr_stall_sampler_frame(uint64_t start_ns, uint64_t after_present_id) { (void)start_ns; (void)after_present_id; }
void pc_vr_stall_sampler_moved(void) {}
void pc_vr_stall_sampler_gpu_wait(bool waiting) { (void)waiting; }
void pc_vr_stall_sampler_presented(void) {}

#endif
