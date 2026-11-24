// The game's RomFS image as one readable thing (runtime/romfs_image.c), without the image on disk: served from the
// romfs/ folder the setup copies (the header and tables laid out in memory by runtime/romfs_build.c, each file's data
// read from its own file, the gaps zeros), so the game data isn't kept twice. The game's reads (runtime/services/fs.c)
// and the shader precompilation (runtime/nvn/shader_archive.c) both go through it.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct RomfsImage RomfsImage;

// The image of the folder dir (extracted/romfs), or, with no folder there, dir.bin (the image file older versions
// built). NULL = neither could be read (why: a short message).
RomfsImage *romfs_image_open(const char *dir, char *why, size_t why_size);
uint64_t romfs_image_size(const RomfsImage *img);
// n bytes of the image at off, past its end zeros; any thread. n, or -1 when a file couldn't be read (logged).
ssize_t romfs_image_pread(RomfsImage *img, void *dst, uint64_t n, uint64_t off);
void romfs_image_close(RomfsImage *img);
// (profiling) The files opened to serve reads since the start, every image together: how many, how many of them were
// opened before (closed again for want of a free slot), and the time the opens took. Any thread.
void romfs_image_open_stats(uint64_t *opens, uint64_t *reopens, uint64_t *open_ns);

// (experiment) Every file of a folder image read into memory with `threads` threads, before any read: the image's
// reads after it are copies from memory, with no file opened or read. false: it could not (memory, a file), and the
// image goes on reading its files. Before the image is shared between threads. Without keep_files (decompress given),
// only the Yaz0 files are read, kept decompressed, and let go: the reads go to the files as before.
// With decompress (a Yaz0 decoder: services/archive.c yaz0_unpack), each Yaz0 file is kept decompressed as well, for
// the game's stream decoder to copy from (hle/yaz0.c, romfs_image_find_decompressed).
typedef uint8_t *(*RomfsDecompressFn)(const uint8_t *data, size_t size, size_t *out_size);
typedef struct {
    uint64_t files, bytes;
    uint64_t wall_ns;                   // the whole preload
    uint64_t open_ns, read_ns;          // summed over the threads
    uint64_t slowest_open_ns, slowest_read_ns;
    uint64_t decompressed_files, decompressed_in, decompressed_out, decompress_ns, decompress_failed;
    int threads;
} RomfsPreloadStats;
bool romfs_image_preload(RomfsImage *img, int threads, RomfsDecompressFn decompress, bool keep_files,
                         RomfsPreloadStats *stats);
// (experiment) A read of the image's [off, off + size) went to dst (services/fs.c): kept for the lookup below while
// any image holds decompressed files. Any thread.
void romfs_image_note_read(RomfsImage *img, const void *dst, uint64_t size, uint64_t off);
// The decompressed copy of the Yaz0 file whose start a noted read put at src, when the len bytes there are that file's
// own (compared with its preloaded copy). false: none. Any thread.
bool romfs_image_find_decompressed(const void *src, uint64_t len, const uint8_t **data, uint64_t *size);

#ifdef __cplusplus
}
#endif
