#pragma once
#include <cstddef>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
void *heap_caps_aligned_alloc(std::size_t alignment, std::size_t bytes, unsigned caps);
void heap_caps_free(void *pointer);
void *heap_caps_malloc(std::size_t bytes, unsigned caps);
void *heap_caps_calloc(std::size_t count, std::size_t bytes, unsigned caps);
