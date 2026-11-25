// Randomized check of runtime/nvn/live_list.h against a flat sorted array (the structure it replaced in cmdbuf.c),
// through cmdbuf.c's three operations: a new list's chunk (shadow_switch), a recording growing over the lists after it
// (shadow_claim) and a memory pool going away (cmdbuf_pool_gone). Small chunks make splits, emptied chunks and repacks
// frequent. After every operation both hold the same entries in the same order and find the same entry for random
// addresses.
#define LIVE_CHUNK_MAX 8
#include "../runtime/nvn/live_list.h"

#include <stdio.h>

static unsigned checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { failures++; if (failures < 10) printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)

// The reference: the same entries in one flat sorted array.
static LiveRange **reference;
static int reference_count, reference_capacity;

// The index of the first reference entry not wholly before lo (cmdbuf.c's former live_index).
static int reference_index(uint64_t lo) {
    int low = 0, high = reference_count;
    while (low < high) {
        int middle = (low + high) / 2;
        if (reference[middle]->hi <= lo && reference[middle]->lo < lo) low = middle + 1;
        else high = middle;
    }
    return low;
}

// A xorshift random number below limit (0 for a limit of 0).
static uint64_t random_state = 0x9E3779B97F4A7C15ull;
static uint64_t random_below(uint64_t limit) {
    random_state ^= random_state << 13;
    random_state ^= random_state >> 7;
    random_state ^= random_state << 17;
    return limit ? random_state % limit : 0;
}

static unsigned repacks;
static LivePos find(LiveList *list, uint64_t lo) {  // (live_find, counting the repacks it does)
    int chunk_count = list->chunk_count;
    LivePos position = live_find(list, lo);
    if (list->chunk_count != chunk_count) repacks++;
    return position;
}

// Checks that list holds exactly the reference's entries, finds what the reference finds, and walks both ways.
static void compare(LiveList *list) {
    CHECK(list->count == reference_count);
    int seen = 0;
    for (int chunk = 0; chunk < list->chunk_count; chunk++) {
        CHECK(list->chunks[chunk].count > 0 && list->chunks[chunk].count < LIVE_CHUNK_MAX);
        for (int i = 0; i < list->chunks[chunk].count; i++, seen++)
            CHECK(seen < reference_count && list->chunks[chunk].entries[i] == reference[seen]);
    }
    CHECK(seen == reference_count);
    for (int probe = 0; probe < 8; probe++) {
        uint64_t address = random_below(1 << 16);
        int expected = reference_index(address);
        CHECK(live_get(list, find(list, address)) == (expected < reference_count ? reference[expected] : NULL));
    }
    // walking both ways visits every entry
    LivePos position = live_next(list, (LivePos){-1, 0});
    for (int i = 0; i < reference_count; i++, position = live_next(list, position))
        CHECK(live_get(list, position) == reference[i]);
    CHECK(!live_get(list, position));
    position = live_prev(list, live_end(list));
    for (int i = reference_count - 1; i >= 0; i--, position = live_prev(list, position))
        CHECK(live_get(list, position) == reference[i]);
    CHECK(!live_get(list, position));
}

int main(void) {
    LiveList list = {0};
    for (int step = 0; step < 200000; step++) {
        uint64_t operation = random_below(10);
        if (operation <
            5) {  // a new chunk at lo: the lists starting at or before it there die, a new one takes their place
            uint64_t lo = random_below(1 << 16);
            int first = reference_index(lo), end = first;
            while (end < reference_count && reference[end]->lo <= lo) end++;
            LivePos position = find(&list, lo);
            int dying = 0;
            for (LivePos at = position; live_get(&list, at) && live_get(&list, at)->lo <= lo; at = live_next(&list, at))
                dying++;
            CHECK(dying == end - first);
            LiveRange *entry = malloc(sizeof *entry);
            entry->lo = entry->hi = lo;
            if (reference_count - (end - first) + 1 > reference_capacity)
                reference =
                    realloc(reference, (size_t)(reference_capacity = reference_capacity * 2 + 64) * sizeof *reference);
            memmove(reference + first + 1, reference + end, (size_t)(reference_count - end) * sizeof *reference);
            reference_count += 1 - (end - first);
            reference[first] = entry;
            position = live_remove(&list, position, dying);
            position = live_insert(&list, position, entry);
            CHECK(live_get(&list, position) == entry);
            LiveRange *prev = live_get(&list, live_prev(&list, position));
            LiveRange *next = live_get(&list, live_next(&list, position));
            CHECK(prev == (first > 0 ? reference[first - 1] : NULL) &&
                  next == (first + 1 < reference_count ? reference[first + 1] : NULL));
        } else if (operation < 9 &&
                   reference_count) {  // a recording grows to `end`: the lists after it that start below end die
            int grown = (int)random_below((uint64_t)reference_count);
            LiveRange *shadow = reference[grown];
            uint64_t end = shadow->hi + random_below(1 << 11);
            int last = grown + 1;
            while (last < reference_count && reference[last]->lo < end) last++;
            LivePos position = find(&list, shadow->lo);
            while (live_get(&list, position) && live_get(&list, position) != shadow)
                position = live_next(&list, position);
            CHECK(live_get(&list, position) == shadow);
            LivePos first = live_next(&list, position);
            int dying = 0;
            for (LivePos at = first; live_get(&list, at) && live_get(&list, at)->lo < end; at = live_next(&list, at))
                dying++;
            CHECK(dying == last - (grown + 1));
            LivePos after = live_remove(&list, first, dying);
            memmove(reference + grown + 1, reference + last, (size_t)(reference_count - last) * sizeof *reference);
            reference_count -= last - (grown + 1);
            shadow->hi = end;
            CHECK(live_get(&list, after) == (grown + 1 < reference_count ? reference[grown + 1] : NULL));
        } else {  // a memory pool [lo, hi) goes away
            uint64_t lo = random_below(1 << 16), hi = lo + random_below(1 << 12);
            int first = reference_index(lo), end = first;
            while (end < reference_count && reference[end]->lo < hi) end++;
            LivePos position = find(&list, lo);
            int dying = 0;
            for (LivePos at = position; live_get(&list, at) && live_get(&list, at)->lo < hi; at = live_next(&list, at))
                dying++;
            CHECK(dying == end - first);
            live_remove(&list, position, dying);
            memmove(reference + first, reference + end, (size_t)(reference_count - end) * sizeof *reference);
            reference_count -= end - first;
        }
        compare(&list);
        if (failures) break;
    }
    live_clear(&list);
    CHECK(!list.count && !list.chunk_count);
    CHECK(repacks > 0);
    printf("live list fixture: %u checks, %u failures, %u repacks\n", checks, failures, repacks);
    return failures != 0;
}
