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
    uint8_t **preloaded;     // (experiment) by file: its whole data in memory (romfs_image_preload); NULL: from its file
    uint8_t **decompressed;  // (experiment) by file: a Yaz0 file's data decompressed (NULL: not one, or not kept)
    uint64_t *decompressed_size;
    uint64_t *prefix_hash;   // (experiment) by file, when the files are not kept: the hash of its first bytes
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
    if (img->preloaded) {  // (no lock: written before the image was shared, never after)
        memcpy(dst, img->preloaded[i] + off, n);
        return true;
    }
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

// romfs_image_preload's threads: each takes the next file not yet taken, reads it whole into its own block.
typedef struct {
    RomfsImage *img;
    bool keep;                     // the files kept in memory; else only the Yaz0 ones read, decompressed and let go
    uint8_t **blocks;
    RomfsDecompressFn decompress;  // NULL: keep the files as they are
    uint8_t **decompressed;
    uint64_t *decompressed_size, *prefix_hash;
    _Atomic int next_file;
    _Atomic bool failed;
    _Atomic uint64_t files_read, bytes_read, open_ns, read_ns, slowest_open_ns, slowest_read_ns;
    _Atomic uint64_t decompressed_files, decompressed_in, decompressed_out, decompress_ns, decompress_failed;
} PreloadWork;

// What romfs_image_find_decompressed compares when the files are not kept: a hash of a Yaz0 file's first bytes
enum { PREFIX_HASHED = 4096 };
static uint64_t prefix_hash(const uint8_t *data, uint64_t size) {  // (FNV-1a)
    uint64_t hash = 0xcbf29ce484222325ull;
    for (uint64_t k = 0; k < size && k < PREFIX_HASHED; k++) hash = (hash ^ data[k]) * 0x100000001b3ull;
    return hash;
}

static void atomic_max(_Atomic uint64_t *slot, uint64_t value) {
    uint64_t seen = atomic_load_explicit(slot, memory_order_relaxed);
    while (value > seen && !atomic_compare_exchange_weak_explicit(slot, &seen, value, memory_order_relaxed, memory_order_relaxed)) {}
}

static void *preload_thread(void *arg) {
    PreloadWork *work = arg;
    const RomfsLayout *layout = &work->img->layout;
    for (int i; !atomic_load(&work->failed) && (i = atomic_fetch_add(&work->next_file, 1)) < layout->nfiles;) {
        const RomfsLayoutFile *file = &layout->files[i];
        const uint64_t open_start_ns = monotonic_ns();
        int fd = open(file->path, O_RDONLY | O_BINARY);
        const uint64_t read_start_ns = monotonic_ns();
        uint8_t head[4] = {0};
        const bool wanted = work->keep ||
                            (file->size >= 16 && fd >= 0 && pread(fd, head, 4, 0) == 4 && !memcmp(head, "Yaz0", 4));
        uint8_t *block = fd >= 0 && wanted ? malloc(file->size ? file->size : 1) : NULL;
        uint64_t done = 0;
        while (block && done < file->size) {
            const uint64_t chunk = file->size - done < (256u << 20) ? file->size - done : (256u << 20);
            const ssize_t got = pread(fd, block + done, (size_t)chunk, (off_t)done);
            if (got <= 0) break;
            done += (uint64_t)got;
        }
        const uint64_t end_ns = monotonic_ns();
        if (fd >= 0) close(fd);
        if (fd < 0 || (wanted && done != file->size)) {
            LOG("[romfs] preload: can't read %s\n", file->path);
            free(block);
            atomic_store(&work->failed, true);
            break;
        }
        atomic_fetch_add_explicit(&work->open_ns, read_start_ns - open_start_ns, memory_order_relaxed);
        atomic_fetch_add_explicit(&work->read_ns, end_ns - read_start_ns, memory_order_relaxed);
        atomic_max(&work->slowest_open_ns, read_start_ns - open_start_ns);
        atomic_max(&work->slowest_read_ns, end_ns - read_start_ns);
        if (!block) continue;
        atomic_fetch_add_explicit(&work->files_read, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&work->bytes_read, file->size, memory_order_relaxed);
        if (work->decompress && file->size >= 16 && !memcmp(block, "Yaz0", 4)) {
            const uint64_t decompress_start_ns = monotonic_ns();
            size_t out_size = 0;
            uint8_t *out = work->decompress(block, (size_t)file->size, &out_size);
            atomic_fetch_add_explicit(&work->decompress_ns, monotonic_ns() - decompress_start_ns, memory_order_relaxed);
            if (out) {
                work->decompressed[i] = out, work->decompressed_size[i] = out_size;
                if (!work->keep) work->prefix_hash[i] = prefix_hash(block, file->size);
                atomic_fetch_add_explicit(&work->decompressed_files, 1, memory_order_relaxed);
                atomic_fetch_add_explicit(&work->decompressed_in, file->size, memory_order_relaxed);
                atomic_fetch_add_explicit(&work->decompressed_out, out_size, memory_order_relaxed);
            } else {
                atomic_fetch_add_explicit(&work->decompress_failed, 1, memory_order_relaxed);
            }
        }
        if (work->keep) work->blocks[i] = block;
        else free(block);
    }
    return NULL;
}

// romfs_image_find_decompressed: the images holding decompressed files, and the latest reads of any image
static _Atomic int decompressed_images;
enum { NOTED_READS = 64 };
static struct {
    const uint8_t *dst;
    uint64_t size, off;
    RomfsImage *img;
} noted_reads[NOTED_READS];
static unsigned noted_next;
static pthread_mutex_t noted_lock = PTHREAD_MUTEX_INITIALIZER;

void romfs_image_note_read(RomfsImage *img, const void *dst, uint64_t size, uint64_t off) {
    if (!atomic_load_explicit(&decompressed_images, memory_order_relaxed) || !img->decompressed) return;
    pthread_mutex_lock(&noted_lock);
    const unsigned slot = noted_next++ % NOTED_READS;
    noted_reads[slot].dst = dst, noted_reads[slot].size = size, noted_reads[slot].off = off, noted_reads[slot].img = img;
    pthread_mutex_unlock(&noted_lock);
}

bool romfs_image_find_decompressed(const void *src_, uint64_t len, const uint8_t **data, uint64_t *size) {
    if (!atomic_load_explicit(&decompressed_images, memory_order_relaxed)) return false;
    const uint8_t *src = src_;
    RomfsImage *img = NULL;
    uint64_t at = 0;
    pthread_mutex_lock(&noted_lock);
    for (unsigned k = 0; k < NOTED_READS && k < noted_next; k++) {  // (the latest first)
        const unsigned slot = (noted_next - 1 - k) % NOTED_READS;
        if (src >= noted_reads[slot].dst && src < noted_reads[slot].dst + noted_reads[slot].size) {
            img = noted_reads[slot].img, at = noted_reads[slot].off + (uint64_t)(src - noted_reads[slot].dst);
            break;
        }
    }
    pthread_mutex_unlock(&noted_lock);
    if (!img || !img->decompressed) return false;
    const RomfsLayout *layout = &img->layout;
    int lo = 0, hi = layout->nfiles - 1;
    while (lo <= hi) {  // the file that starts at `at`
        const int mid = (lo + hi) / 2;
        if (layout->files[mid].start < at) lo = mid + 1;
        else if (layout->files[mid].start > at) hi = mid - 1;
        else {
            if (!img->decompressed[mid]) return false;
            const uint64_t file_size = layout->files[mid].size;
            if (img->preloaded) {  // the chunk's bytes are the file's own
                if (memcmp(src, img->preloaded[mid], (size_t)(len < file_size ? len : file_size))) return false;
            } else {  // the file's first bytes, by their hash (the read noted above says which file they came from)
                const uint64_t hashed = file_size < PREFIX_HASHED ? file_size : PREFIX_HASHED;
                if (len < hashed || prefix_hash(src, file_size) != img->prefix_hash[mid]) return false;
            }
            *data = img->decompressed[mid], *size = img->decompressed_size[mid];
            return true;
        }
    }
    return false;
}

bool romfs_image_preload(RomfsImage *img, int threads, RomfsDecompressFn decompress, bool keep_files,
                         RomfsPreloadStats *stats) {
    memset(stats, 0, sizeof *stats);
    if (img->bin_fd >= 0 || img->preloaded || img->decompressed || threads < 1 || (!keep_files && !decompress)) return false;
    if (threads > 64) threads = 64;
    const int nfiles = img->layout.nfiles;
    PreloadWork work = {.img = img, .keep = keep_files, .blocks = calloc((size_t)nfiles + 1, sizeof(uint8_t *)),
                        .decompress = decompress};
    if (!work.blocks) return false;
    if (decompress) {
        work.decompressed = calloc((size_t)nfiles + 1, sizeof(uint8_t *));
        work.decompressed_size = calloc((size_t)nfiles + 1, sizeof(uint64_t));
        work.prefix_hash = calloc((size_t)nfiles + 1, sizeof(uint64_t));
        if (!work.decompressed || !work.decompressed_size || !work.prefix_hash) {
            free(work.blocks), free(work.decompressed), free(work.decompressed_size), free(work.prefix_hash);
            return false;
        }
    }
    const uint64_t start_ns = monotonic_ns();
    pthread_t workers[64];
    int started = 0;
    for (; started < threads; started++)
        if (pthread_create(&workers[started], NULL, preload_thread, &work)) break;
    if (!started) preload_thread(&work);
    for (int k = 0; k < started; k++) pthread_join(workers[k], NULL);
    stats->wall_ns = monotonic_ns() - start_ns;
    stats->threads = started ? started : 1;
    stats->open_ns = atomic_load(&work.open_ns), stats->read_ns = atomic_load(&work.read_ns);
    stats->slowest_open_ns = atomic_load(&work.slowest_open_ns), stats->slowest_read_ns = atomic_load(&work.slowest_read_ns);
    stats->decompressed_files = atomic_load(&work.decompressed_files), stats->decompress_ns = atomic_load(&work.decompress_ns);
    stats->decompressed_in = atomic_load(&work.decompressed_in), stats->decompressed_out = atomic_load(&work.decompressed_out);
    stats->decompress_failed = atomic_load(&work.decompress_failed);
    if (atomic_load(&work.failed)) {
        for (int i = 0; i < nfiles; i++) {
            free(work.blocks[i]);
            if (work.decompressed) free(work.decompressed[i]);
        }
        free(work.blocks), free(work.decompressed), free(work.decompressed_size), free(work.prefix_hash);
        return false;
    }
    stats->files = atomic_load(&work.files_read), stats->bytes = atomic_load(&work.bytes_read);
    if (keep_files) img->preloaded = work.blocks;
    else free(work.blocks);  // (each let go already)
    if (decompress) {
        img->decompressed = work.decompressed, img->decompressed_size = work.decompressed_size;
        if (keep_files) free(work.prefix_hash);
        else img->prefix_hash = work.prefix_hash;
        atomic_fetch_add(&decompressed_images, 1);
    }
    return true;
}

void romfs_image_close(RomfsImage *img) {
    if (!img) return;
    if (img->preloaded) {
        for (int i = 0; i < img->layout.nfiles; i++) free(img->preloaded[i]);
        free(img->preloaded);
    }
    if (img->decompressed) {  // (closed when no read is running: a reboot; its noted reads are forgotten)
        atomic_fetch_sub(&decompressed_images, 1);
        pthread_mutex_lock(&noted_lock);
        for (int k = 0; k < NOTED_READS; k++)
            if (noted_reads[k].img == img) noted_reads[k].img = NULL, noted_reads[k].size = 0;
        pthread_mutex_unlock(&noted_lock);
        for (int i = 0; i < img->layout.nfiles; i++) free(img->decompressed[i]);
        free(img->decompressed), free(img->decompressed_size), free(img->prefix_hash);
    }
    for (int k = 0; k < OPEN_FILES; k++)
        if (img->open[k].fd >= 0) close(img->open[k].fd);
    if (img->bin_fd >= 0) close(img->bin_fd);
    free(img->opened_before);
    romfs_layout_free(&img->layout);
    pthread_mutex_destroy(&img->lock);
    free(img);
}
