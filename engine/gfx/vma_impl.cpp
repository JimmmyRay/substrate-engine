// Single translation unit that instantiates the Vulkan Memory Allocator.
// volk must be included first: it owns the Vulkan header and defines VK_NO_PROTOTYPES.
#include <volk.h>

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif

#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
