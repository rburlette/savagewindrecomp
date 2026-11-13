// The live command lists' shadows (cmdbuf.c): non-overlapping guest memory ranges sorted by start, kept in chunks of
// fewer than LIVE_CHUNK_MAX entries. Inserting or removing an entry moves the rest of one chunk; one flat sorted array
// moved every entry after it, and BOTW keeps about 40,000 live lists with about 80 inserts and 80 removals a frame:
// 2-3 million pointers moved a frame (8-10% of each busy game thread). Entries point to structures that start with
// their range (LiveRange). No locking: cmdbuf.c holds its lock. A position (chunk, index) is valid until the next
// insert or removal, except the position each of those returns.
#ifndef SAVAGEWIND_LIVE_LIST_H
#define SAVAGEWIND_LIVE_LIST_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint64_t lo, hi; } LiveRange;
#ifndef LIVE_CHUNK_MAX
#define LIVE_CHUNK_MAX 512  // (tools/test_live_list.c uses small chunks)
#endif
// One chunk: its entries, in order. Never empty; room for LIVE_CHUNK_MAX.
typedef struct { LiveRange **entries; int count; } LiveChunk;
typedef struct {
    LiveChunk *chunks;
    int chunk_count, chunk_capacity;
    int count;         // entries in all
    uint64_t shifted;  // entries moved by inserts and removals (statistics)
} LiveList;
// A position: chunk == chunk_count is the end, chunk < 0 is before the first entry.
typedef struct { int chunk, index; } LivePos;

static inline LiveRange *live_get(const LiveList *list, LivePos position) {
    return position.chunk >= 0 && position.chunk < list->chunk_count ?
        list->chunks[position.chunk].entries[position.index] : NULL;
}

static inline LivePos live_end(const LiveList *list) { return (LivePos){list->chunk_count, 0}; }

static inline LivePos live_next(const LiveList *list, LivePos position) {
    if (position.chunk < 0) return (LivePos){0, 0};
    if (position.chunk >= list->chunk_count) return position;
    if (++position.index == list->chunks[position.chunk].count) {
        position.chunk++;
        position.index = 0;
    }
    return position;
}

static inline LivePos live_prev(const LiveList *list, LivePos position) {
    if (position.chunk >= list->chunk_count) {
        return list->chunk_count ? (LivePos){list->chunk_count - 1, list->chunks[list->chunk_count - 1].count - 1} :
                                   (LivePos){-1, 0};
    }
    if (position.chunk < 0) return position;
    if (position.index) return (LivePos){position.chunk, position.index - 1};
    return position.chunk ? (LivePos){position.chunk - 1, list->chunks[position.chunk - 1].count - 1}
                          : (LivePos){-1, 0};
}

// Inserts a chunk into the list's chunk index at `at`.
static inline void live_chunk_insert(LiveList *list, int at, LiveChunk chunk) {
    if (list->chunk_count == list->chunk_capacity) {
        list->chunk_capacity = list->chunk_capacity ? list->chunk_capacity * 2 : 64;
        list->chunks = realloc(list->chunks, (size_t)list->chunk_capacity * sizeof *list->chunks);
        if (!list->chunks) abort();
    }
    memmove(list->chunks + at + 1, list->chunks + at, (size_t)(list->chunk_count - at) * sizeof *list->chunks);
    list->chunks[at] = chunk;
    list->chunk_count++;
}

static inline LiveChunk live_chunk_new(void) {
    LiveChunk chunk = {malloc(LIVE_CHUNK_MAX * sizeof(LiveRange *)), 0};
    if (!chunk.entries) abort();
    return chunk;
}

// Chunks gone sparse (removals): repacked half full. Positions held before are invalid afterwards.
static inline void live_repack(LiveList *list) {
    LiveRange **all = malloc((size_t)(list->count ? list->count : 1) * sizeof *all);
    if (!all) abort();
    int total = 0;
    for (int chunk = 0; chunk < list->chunk_count; chunk++) {
        memcpy(all + total, list->chunks[chunk].entries, (size_t)list->chunks[chunk].count * sizeof *all);
        total += list->chunks[chunk].count;
        free(list->chunks[chunk].entries);
    }
    list->chunk_count = 0;
    for (int at = 0; at < total; at += LIVE_CHUNK_MAX / 2) {
        LiveChunk chunk = live_chunk_new();
        chunk.count = total - at < LIVE_CHUNK_MAX / 2 ? total - at : LIVE_CHUNK_MAX / 2;
        memcpy(chunk.entries, all + at, (size_t)chunk.count * sizeof *all);
        live_chunk_insert(list, list->chunk_count, chunk);
    }
    free(all);
}

// The first entry not wholly before `lo` (the end when there is none). Repacks sparse chunks first, so it starts
// each of cmdbuf.c's operations.
static inline LivePos live_find(LiveList *list, uint64_t lo) {
    if (list->chunk_count > 8 && list->chunk_count * (LIVE_CHUNK_MAX / 4) > list->count)
        live_repack(list);  // (under a quarter full on average)
#define LIVE_BEFORE(entry) ((entry)->hi <= lo && (entry)->lo < lo)
    // Binary search: the first chunk whose last entry is not wholly before lo, then the first such entry in it.
    int chunk_low = 0, chunk_high = list->chunk_count;
    while (chunk_low < chunk_high) {
        int middle = (chunk_low + chunk_high) / 2;
        if (LIVE_BEFORE(list->chunks[middle].entries[list->chunks[middle].count - 1])) chunk_low = middle + 1;
        else chunk_high = middle;
    }
    if (chunk_low == list->chunk_count) return live_end(list);
    int entry_low = 0, entry_high = list->chunks[chunk_low].count - 1;
    while (entry_low < entry_high) {
        int middle = (entry_low + entry_high) / 2;
        if (LIVE_BEFORE(list->chunks[chunk_low].entries[middle])) entry_low = middle + 1;
        else entry_high = middle;
    }
#undef LIVE_BEFORE
    return (LivePos){chunk_low, entry_low};
}

// Inserts entry before position (the end: last); returns entry's position.
static inline LivePos live_insert(LiveList *list, LivePos position, LiveRange *entry) {
    if (!list->chunk_count) {
        live_chunk_insert(list, 0, live_chunk_new());
        position = (LivePos){0, 0};
    } else if (position.chunk >= list->chunk_count) {
        position = (LivePos){list->chunk_count - 1, list->chunks[list->chunk_count - 1].count};
    }
    if (list->chunks[position.chunk].count == LIVE_CHUNK_MAX - 1) {  // (full after this one: split in halves first)
        LiveChunk *chunk = &list->chunks[position.chunk], half = live_chunk_new();
        int keep = chunk->count / 2;
        half.count = chunk->count - keep;
        memcpy(half.entries, chunk->entries + keep, (size_t)half.count * sizeof *half.entries);
        chunk->count = keep;
        live_chunk_insert(list, position.chunk + 1, half);
        if (position.index > keep) position = (LivePos){position.chunk + 1, position.index - keep};
    }
    LiveChunk *chunk = &list->chunks[position.chunk];
    memmove(chunk->entries + position.index + 1, chunk->entries + position.index,
            (size_t)(chunk->count - position.index) * sizeof *chunk->entries);
    list->shifted += (uint64_t)(chunk->count - position.index);
    chunk->entries[position.index] = entry;
    chunk->count++;
    list->count++;
    return position;
}

// Removes `count` entries from position on; returns the position of the entry that followed them.
static inline LivePos live_remove(LiveList *list, LivePos position, int count) {
    while (count > 0 && position.chunk < list->chunk_count) {
        LiveChunk *chunk = &list->chunks[position.chunk];
        int taken = chunk->count - position.index < count ? chunk->count - position.index : count;
        memmove(chunk->entries + position.index, chunk->entries + position.index + taken,
                (size_t)(chunk->count - position.index - taken) * sizeof *chunk->entries);
        list->shifted += (uint64_t)(chunk->count - position.index - taken);
        chunk->count -= taken;
        list->count -= taken;
        count -= taken;
        if (!chunk->count) {
            free(chunk->entries);
            memmove(list->chunks + position.chunk, list->chunks + position.chunk + 1,
                    (size_t)(list->chunk_count - position.chunk - 1) * sizeof *list->chunks);
            list->chunk_count--;
            position.index = 0;  // (the next chunk is at position.chunk now)
        } else if (position.index == chunk->count) {
            position = (LivePos){position.chunk + 1, 0};
        }
    }
    return position;
}

static inline void live_clear(LiveList *list) {
    for (int chunk = 0; chunk < list->chunk_count; chunk++) free(list->chunks[chunk].entries);
    list->chunk_count = list->count = 0;
}

#endif
