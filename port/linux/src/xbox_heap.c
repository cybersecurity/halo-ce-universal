/*
XBOX_HEAP.C

The game's heap, inside the Xbox address space.

Everything the game allocates (system_malloc is GlobalAlloc) can end up in
a structure that holds 32-bit pointers: tag data built at run time, script
values, Direct3D resource headers. So the heap lives in the Xbox address
space too (cseries/xbox_address.h), below the contiguous window: Xbox addresses
XBOX_HEAP_BASE to XBOX_HEAP_END, committed read-write at start-up and backed
by the host on first touch.

Blocks carry a 16-byte header. Requests up to 64 KB come from power-of-two
size classes with free lists; larger ones are rounded to 64 KB and reuse
freed large blocks first fit. The game allocates mostly at start-up and map
load, so this stays simple rather than clever.
*/

#include "platform.h"

#ifdef HALO_64BIT

#include <errno.h>
#include <string.h>
#include <sys/mman.h>

#define XBOX_HEAP_BASE 0x10000000U
#define XBOX_HEAP_END 0x7f000000U
#define HEAP_MAGIC 0x68656170U /* 'heap' */
#define SMALL_CLASS_COUNT 13 /* 16 bytes .. 64 KB of payload */
#define LARGE_GRANULARITY 0x10000U

struct heap_block
{
	unsigned long long capacity; /* payload bytes */
	unsigned int magic;
	unsigned int free;
};

struct free_node
{
	struct free_node *next;
};

static pthread_mutex_t heap_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned char *heap_next;
static unsigned char *heap_end;
static struct free_node *small_free[SMALL_CLASS_COUNT];
static struct free_node *large_free;

static BOOL heap_initialize(void)
{
	void *base = xbox_pointer(XBOX_HEAP_BASE);

	if (heap_next)
		return TRUE;
	if (mmap(base, XBOX_HEAP_END - XBOX_HEAP_BASE, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) != base)
	{
		platform_log("cannot commit the Xbox heap (%s)", strerror(errno));
		return FALSE;
	}
	heap_next = base;
	heap_end = xbox_pointer(XBOX_HEAP_END);
	return TRUE;
}

static int small_class(size_t size)
{
	int index = 0;

	while (index < SMALL_CLASS_COUNT && ((size_t)16 << index) < size)
		index++;
	return index; /* SMALL_CLASS_COUNT: a large block */
}

static struct heap_block *block_of(void *payload)
{
	return (struct heap_block *)payload - 1;
}

static struct heap_block *carve(size_t capacity)
{
	struct heap_block *block;

	if ((size_t)(heap_end - heap_next) < sizeof(*block) + capacity)
		return NULL;
	block = (struct heap_block *)heap_next;
	heap_next += sizeof(*block) + capacity;
	block->capacity = capacity;
	block->magic = HEAP_MAGIC;
	return block;
}

BOOL xbox_heap_contains(const void *pointer)
{
	unsigned long long offset = (unsigned long long)(uintptr_t)pointer - XBOX_ADDRESS_SPACE_BASE;

	return offset >= XBOX_HEAP_BASE && offset < XBOX_HEAP_END;
}

void *xbox_heap_allocate(size_t size, BOOL zero)
{
	struct heap_block *block = NULL;
	int index = small_class(size ? size : 1);

	pthread_mutex_lock(&heap_lock);
	if (heap_initialize())
	{
		if (index < SMALL_CLASS_COUNT)
		{
			if (small_free[index])
			{
				block = block_of(small_free[index]);
				small_free[index] = small_free[index]->next;
			}
			else
			{
				block = carve((size_t)16 << index);
			}
		}
		else
		{
			size_t capacity = (size + LARGE_GRANULARITY - 1) & ~(size_t)(LARGE_GRANULARITY - 1);
			struct free_node **link;

			for (link = &large_free; *link; link = &(*link)->next)
			{
				if (block_of(*link)->capacity >= capacity)
				{
					block = block_of(*link);
					*link = (*link)->next;
					break;
				}
			}
			if (!block)
				block = carve(capacity);
		}
	}
	if (block)
		block->free = FALSE;
	pthread_mutex_unlock(&heap_lock);
	if (!block)
	{
		platform_log("Xbox heap exhausted (%lu bytes requested)", (unsigned long)size);
		return NULL;
	}
	if (zero)
		memset(block + 1, 0, size);
	return block + 1;
}

void xbox_heap_free(void *pointer)
{
	struct heap_block *block;
	struct free_node *node = pointer;
	int index;

	if (!pointer)
		return;
	block = block_of(pointer);
	if (block->magic != HEAP_MAGIC || block->free)
	{
		platform_log("Xbox heap: bad free of %p", pointer);
		return;
	}
	index = small_class(block->capacity);
	pthread_mutex_lock(&heap_lock);
	block->free = TRUE;
	if (index < SMALL_CLASS_COUNT && block->capacity == ((size_t)16 << index))
	{
		node->next = small_free[index];
		small_free[index] = node;
	}
	else
	{
		node->next = large_free;
		large_free = node;
	}
	pthread_mutex_unlock(&heap_lock);
}

size_t xbox_heap_capacity(void *pointer)
{
	return pointer ? (size_t)block_of(pointer)->capacity : 0;
}

#endif /* HALO_64BIT */
