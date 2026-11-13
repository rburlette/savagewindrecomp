// Vulkan Memory Allocator implementation (header-only library, MSYS2 package vulkan-memory-allocator).
#include <cstdio>
#include <cstdlib>
// A failed VMA check prints the host backtrace (runtime/platform.h) so the allocation's caller is known.
extern "C" void plat_print_backtrace(void);
#define VMA_ASSERT(expr)                                                                                      \
    do {                                                                                                      \
        if (!(expr)) {                                                                                        \
            fprintf(stderr, "VMA assertion failed: %s (vk_mem_alloc.h:%d)\n", #expr, __LINE__);               \
            plat_print_backtrace();                                                                           \
            abort();                                                                                          \
        }                                                                                                     \
    } while (0)
#define VMA_IMPLEMENTATION
#define VMA_STATIC_VULKAN_FUNCTIONS 1
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0
#if __has_include(<vma/vk_mem_alloc.h>)
#include <vma/vk_mem_alloc.h>
#else
#include <vk_mem_alloc.h>  // (Debian / Ubuntu libvulkan-memory-allocator-dev)
#endif
