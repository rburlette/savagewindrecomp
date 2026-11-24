// The RomFS image served from the romfs/ folder (romfs_image.h). The game reads from several threads, so a read only
// holds the lock to find its files' descriptors: the pread runs outside it, and a descriptor in use is never closed.
#include "romfs_image.h"

#include "platform.h"  // (Windows: pread)
#include "romfs_build.h"
#include "rt.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef O_BINARY
#define O_BINARY 0
#endif

// Files kept open (macOS allows 256 open files by default, and the overlay keeps its own open: runtime/services/fs.c)
#define OPEN_FILES 32

typedef struct {
    int file, fd, busy;  // busy: reads using fd right now
    uint64_t used;       // for closing the least recently used
} OpenFile;

struct RomfsImage {
    RomfsLayout layout;
    int bin_fd;  // an older version's romfs.bin; -1: served from the folder
    uint64_t size;
    pthread_mutex_t lock;
    OpenFile open[OPEN_FILES];
    uint64_t tick;
    uint8_t *opened_before;  // (profiling) by file: opened once already, so another open is a reopen
};

// (profiling) romfs_image_open_stats. The clock is the C library's, not the platform's: tools/test_romfs.c builds
// this file on its own.
static _Atomic uint64_t open_count, reopen_count, open_time_ns;
static uint64_t monotonic_ns(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000000u + (uint64_t)now.tv_nsec;
}

void romfs_image_open_stats(uint64_t *opens, uint64_t *reopens, uint64_t *open_ns) {
    *opens = atomic_load_explicit(&open_count, memory_order_relaxed);
    *reopens = atomic_load_explicit(&reopen_count, memory_order_relaxed);
    *open_ns = atomic_load_explicit(&open_time_ns, memory_order_relaxed);
}

RomfsImage *romfs_image_open(const char *dir, char *why, size_t why_size) {
    RomfsImage *img = calloc(1, sizeof *img);
    img->bin_fd = -1;
    pthread_mutex_init(&img->lock, NULL);
    for (int k = 0; k < OPEN_FILES; k++) img->open[k].fd = -1;
    struct stat sb;
    char layout_why[256] = "";
    if (stat(dir, &sb) == 0 && S_ISDIR(sb.st_mode) && romfs_layout(dir, &img->layout, layout_why, sizeof layout_why)) {
        img->size = img->layout.size;
        img->opened_before = calloc((size_t)img->layout.nfiles + 1, 1);
        return img;
    }
    char bin[1100];
    snprintf(bin, sizeof bin, "%s.bin", dir);
    img->bin_fd = open(bin, O_RDONLY | O_BINARY);
    if (img->bin_fd >= 0 && fstat(img->bin_fd, &sb) == 0) {
        img->size = (uint64_t)sb.st_size;
        return img;
    }
    if (why) snprintf(why, why_size, "%s", layout_why[0] ? layout_why : "no game data folder");
    romfs_image_close(img);
    return NULL;
}

uint64_t romfs_image_size(const RomfsImage *img) { return img->size; }

// The descriptor of file i, marked busy in its slot (*slot; -1: every slot busy, the caller closes it). lock held.
static int file_fd(RomfsImage *img, int i, int *slot) {
    int pick = -1;
    for (int k = 0; k < OPEN_FILES; k++) {
        OpenFile *o = &img->open[k];
        if (o->fd >= 0 && o->file == i) {
            o->busy++, o->used = ++img->tick, *slot = k;
            return o->fd;
        }
        if (!o->busy && (pick < 0 || o->used < img->open[pick].used)) pick = k;
    }
    const uint64_t open_start_ns = monotonic_ns();
    // (debug) SAVAGEWIND_DEBUG_ROMFS_FIRST_OPEN_MS=<ms>: each file's first open takes that much longer, as a cold one
    // does on a machine whose virus scanner reads a file at its first open (about 9 ms on the user's, 2026-11-24), so
    // a run with the scanner's cache warm can still be compared with a cold one.
    static long first_open_delay_us = -1;
    if (first_open_delay_us < 0) {
        const char *e = getenv("SAVAGEWIND_DEBUG_ROMFS_FIRST_OPEN_MS");
        first_open_delay_us = e ? (long)(atof(e) * 1000) : 0;
    }
    if (first_open_delay_us > 0 && img->opened_before && !img->opened_before[i]) usleep((useconds_t)first_open_delay_us);
    int fd = open(img->layout.files[i].path, O_RDONLY | O_BINARY);
    atomic_fetch_add_explicit(&open_time_ns, monotonic_ns() - open_start_ns, memory_order_relaxed);
    atomic_fetch_add_explicit(&open_count, 1, memory_order_relaxed);
    if (img->opened_before && img->opened_before[i]) atomic_fetch_add_explicit(&reopen_count, 1, memory_order_relaxed);
    else if (img->opened_before) img->opened_before[i] = 1;
    *slot = fd >= 0 ? pick : -1;
    if (*slot < 0) return fd;
    if (img->open[pick].fd >= 0) close(img->open[pick].fd);
    img->open[pick] = (OpenFile){i, fd, 1, ++img->tick};
    return fd;
}

static bool read_file(RomfsImage *img, int i, uint8_t *dst, uint64_t n, uint64_t off) {
    int slot;
    pthread_mutex_lock(&img->lock);
    int fd = file_fd(img, i, &slot);
    pthread_mutex_unlock(&img->lock);
    if (fd < 0) return false;
    bool ok = pread(fd, dst, n, (off_t)off) == (ssize_t)n;
    pthread_mutex_lock(&img->lock);
    if (slot >= 0) img->open[slot].busy--;
    else close(fd);
    pthread_mutex_unlock(&img->lock);
    return ok;
}

ssize_t romfs_image_pread(RomfsImage *img, void *dst_, uint64_t n, uint64_t off) {
    if (img->bin_fd >= 0) return pread(img->bin_fd, dst_, n, (off_t)off);
    const RomfsLayout *l = &img->layout;
    uint8_t *dst = dst_;
    memset(dst, 0, n);
    uint64_t end = off + n;
    if (off < ROMFS_DATA_START) memcpy(dst, l->header + off, (end < ROMFS_DATA_START ? end : ROMFS_DATA_START) - off);
    uint64_t ta = l->tables_off, tb = ta + l->tables_size;
    if (off < tb && end > ta) {
        uint64_t a = off > ta ? off : ta, b = end < tb ? end : tb;
        memcpy(dst + (a - off), l->tables + (a - ta), b - a);
    }
    // the files overlapping [off, end): the first by binary search, then in order
    int lo = 0, hi = l->nfiles - 1, first = l->nfiles;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (l->files[mid].start + l->files[mid].size > off) first = mid, hi = mid - 1;
        else lo = mid + 1;
    }
    for (int i = first; i < l->nfiles && l->files[i].start < end; i++) {
        const RomfsLayoutFile *f = &l->files[i];
        uint64_t a = off > f->start ? off : f->start, b = end < f->start + f->size ? end : f->start + f->size;
        if (a >= b) continue;
        if (!read_file(img, i, dst + (a - off), b - a, a - f->start)) {
            LOG("[romfs] can't read %s\n", f->path);
            return -1;
        }
    }
    return (ssize_t)n;
}

void romfs_image_close(RomfsImage *img) {
    if (!img) return;
    for (int k = 0; k < OPEN_FILES; k++)
        if (img->open[k].fd >= 0) close(img->open[k].fd);
    if (img->bin_fd >= 0) close(img->bin_fd);
    free(img->opened_before);
    romfs_layout_free(&img->layout);
    pthread_mutex_destroy(&img->lock);
    free(img);
}
