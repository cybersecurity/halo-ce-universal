/* Run through tools/test_web_geometry.py. Real cache code, mocked GL storage.
   Sanitizers catch any over-read of tightly sized vertex/index allocations. */
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned int GLuint;
typedef unsigned int GLenum;
typedef ptrdiff_t GLsizeiptr;
typedef int BOOL;
#define TRUE 1
#define FALSE 0
#define GL_ARRAY_BUFFER 1
#define GL_ELEMENT_ARRAY_BUFFER 2
#define GL_STREAM_DRAW 3
#define GL_STATIC_DRAW 4

#define MOCK_BUFFERS 4096
static struct { unsigned char *data; size_t size; GLenum usage; } buffers[MOCK_BUFFERS];
static GLuint next_buffer, bound[3];
static unsigned long upload_count;

static void glGenBuffers(int count, GLuint *output)
{
	while (count--)
	{
		assert(next_buffer + 1 < MOCK_BUFFERS);
		*output++ = ++next_buffer;
	}
}

static void state_array_buffer(GLuint buffer) { bound[GL_ARRAY_BUFFER] = buffer; }
static void state_element_array_buffer(GLuint buffer) { bound[GL_ELEMENT_ARRAY_BUFFER] = buffer; }
static void glBufferData(GLenum target, GLsizeiptr size, const void *data, GLenum usage)
{
	GLuint buffer = bound[target];
	assert(buffer && buffer < MOCK_BUFFERS && size >= 0);
	free(buffers[buffer].data);
	buffers[buffer].data = size ? malloc((size_t)size) : NULL;
	buffers[buffer].size = (size_t)size;
	buffers[buffer].usage = usage;
	if (size) { assert(data); memcpy(buffers[buffer].data, data, (size_t)size); }
	upload_count++;
}

#include "../../port/linux/src/web_geometry.h"

static void reset(void)
{
	unsigned int bucket, way, buffer;
	for (bucket = 0; bucket < WEB_GEOMETRY_CACHE_BUCKETS; bucket++)
		for (way = 0; way < WEB_GEOMETRY_CACHE_WAYS; way++)
			free(web_geometry.entries[bucket][way].snapshot);
	for (buffer = 1; buffer <= next_buffer; buffer++) free(buffers[buffer].data);
	memset(buffers, 0, sizeof(buffers));
	memset(&web_geometry, 0, sizeof(web_geometry));
	memset(bound, 0, sizeof(bound));
	next_buffer = 0;
	upload_count = 0;
}

static void contents(GLuint buffer, const void *data, size_t size)
{
	assert(buffers[buffer].size == size);
	assert(!memcmp(buffers[buffer].data, data, size));
}

static void retained_vertices(void)
{
	unsigned char data[] = {1, 2, 3, 4, 5, 6, 7};
	GLuint first, retained, next;
	unsigned long uploads;
	reset();
	web_geometry_begin_draw(0);
	first = web_geometry_vertices(data, data, sizeof(data));
	assert(buffers[first].usage == GL_STREAM_DRAW);
	web_geometry_begin_draw(1);
	retained = web_geometry_vertices(data, data, sizeof(data));
	assert(retained != first && buffers[retained].usage == GL_STATIC_DRAW);
	uploads = upload_count;
	web_geometry_begin_draw(2);
	assert(web_geometry_vertices(data, data, sizeof(data)) == retained);
	assert(upload_count == uploads && web_geometry.stats.avoided_bytes == sizeof(data));
	/* D3D locks need not report writes: exact bytes invalidate the cache. */
	data[6] ^= 1;
	web_geometry_begin_draw(3);
	next = web_geometry_vertices(data, data, sizeof(data));
	contents(next, data, sizeof(data));
	assert(buffers[next].usage == GL_STREAM_DRAW);
	assert(web_geometry.stats.invalidations == 1);
	web_geometry_begin_draw(4);
	assert(web_geometry_vertices(data, data, sizeof(data)) == retained);
	contents(retained, data, sizeof(data));
}

static void multistream_aliases(void)
{
	unsigned char source[] = {1, 2, 3, 4};
	unsigned char converted[] = {3, 2, 1, 4};
	unsigned char other[] = {5, 6, 7, 8};
	GLuint first, second, third;
	reset();
	web_geometry_begin_draw(0);
	web_geometry_vertices(source, source, sizeof(source));
	web_geometry_begin_draw(1);
	first = web_geometry_vertices(source, source, sizeof(source));
	/* The same source with a different declaration cannot replace data
	already referenced by another stream in this unsubmitted draw. */
	second = web_geometry_vertices(source, converted, sizeof(converted));
	third = web_geometry_vertices(other, other, sizeof(other));
	assert(first != second && first != third && second != third);
	contents(first, source, sizeof(source));
	contents(second, converted, sizeof(converted));
	contents(third, other, sizeof(other));
}

static void rotating_streams(void)
{
	unsigned char *data = malloc(6);
	GLuint frames[4], streams[16], index;
	unsigned int frame, draw, stream;
	reset();
	memset(data, 19, 6);
	for (frame = 0; frame < 4; frame++)
	{
		web_geometry_begin_draw(frame);
		frames[frame] = web_geometry_stream(GL_ARRAY_BUFFER, data, 6);
		contents(frames[frame], data, 6);
	}
	assert(frames[0] != frames[1] && frames[1] != frames[2] && frames[2] != frames[0]);
	assert(frames[0] == frames[3]);
	/* Wrap within a busy frame, then cross the end with a 16-stream draw. */
	for (draw = 1; draw < WEB_GEOMETRY_SLOTS - 4; draw++)
	{
		web_geometry_begin_draw(3);
		web_geometry_stream(GL_ARRAY_BUFFER, data, 6);
	}
	web_geometry_begin_draw(3);
	for (stream = 0; stream < 16; stream++)
	{
		data[0] = (unsigned char)stream;
		streams[stream] = web_geometry_stream(GL_ARRAY_BUFFER, data, 6);
	}
	index = web_geometry_stream(GL_ELEMENT_ARRAY_BUFFER, data, 6);
	for (stream = 0; stream < 16; stream++)
	{
		assert(streams[stream] != index);
		data[0] = (unsigned char)stream;
		contents(streams[stream], data, 6);
	}
	assert(bound[GL_ELEMENT_ARRAY_BUFFER] == index);
	/* 512 uploads are allowed, and a saturated draw cannot overwrite one. */
	for (stream = 16; stream < WEB_GEOMETRY_SLOTS; stream++)
		assert(web_geometry_stream(GL_ARRAY_BUFFER, data, 6));
	assert(!web_geometry_stream(GL_ARRAY_BUFFER, data, 6));
	free(data);
}

static const void *key_for_bucket(unsigned long bucket, unsigned long serial, unsigned long size)
{
	uintptr_t key = 16 + serial * 65536;
	while ((((key >> 4) ^ (key >> 15) ^ size) % WEB_GEOMETRY_CACHE_BUCKETS) != bucket) key += 16;
	return (const void *)key;
}

static void bounded_cache_and_collisions(void)
{
	unsigned char *data = malloc(WEB_GEOMETRY_CACHE_ENTRY_BYTES + 1);
	const void *keys[WEB_GEOMETRY_CACHE_WAYS + 1];
	GLuint pinned[WEB_GEOMETRY_CACHE_WAYS], overflow;
	unsigned int way, entry;
	reset();
	memset(data, 23, WEB_GEOMETRY_CACHE_ENTRY_BYTES + 1);
	for (way = 0; way <= WEB_GEOMETRY_CACHE_WAYS; way++) keys[way] = key_for_bucket(5, way, 4);
	for (way = 0; way < WEB_GEOMETRY_CACHE_WAYS; way++)
	{
		web_geometry_begin_draw(0);
		web_geometry_vertices(keys[way], data, 4);
	}
	web_geometry_begin_draw(0);
	for (way = 0; way < WEB_GEOMETRY_CACHE_WAYS; way++) pinned[way] = web_geometry_vertices(keys[way], data, 4);
	data[0] = 42;
	overflow = web_geometry_vertices(keys[WEB_GEOMETRY_CACHE_WAYS], data, 4);
	assert(buffers[overflow].usage == GL_STREAM_DRAW);
	data[0] = 23;
	for (way = 0; way < WEB_GEOMETRY_CACHE_WAYS; way++) contents(pinned[way], data, 4);
	/* Eviction is allowed only once those draws have been submitted. */
	web_geometry_begin_draw(1);
	web_geometry_vertices(keys[WEB_GEOMETRY_CACHE_WAYS], data, 4);
	web_geometry_begin_draw(1);
	assert(buffers[web_geometry_vertices(keys[WEB_GEOMETRY_CACHE_WAYS], data, 4)].usage == GL_STATIC_DRAW);
	reset();
	for (entry = 0; entry < 40; entry++)
	{
		web_geometry_begin_draw(entry);
		web_geometry_vertices(key_for_bucket(entry, entry, WEB_GEOMETRY_CACHE_ENTRY_BYTES), data,
			WEB_GEOMETRY_CACHE_ENTRY_BYTES);
		assert(web_geometry.snapshot_bytes <= WEB_GEOMETRY_CACHE_BYTES);
	}
	assert(web_geometry.snapshot_bytes == WEB_GEOMETRY_CACHE_BYTES);
	web_geometry_begin_draw(41);
	web_geometry_vertices(data, data, WEB_GEOMETRY_CACHE_ENTRY_BYTES + 1);
	assert(web_geometry.snapshot_bytes == WEB_GEOMETRY_CACHE_BYTES);
	free(data);
}

static void bounded_stream_storage(void)
{
	const unsigned long size = 256 * 1024;
	unsigned char *data = malloc(size);
	GLuint streams[16];
	unsigned long draw, stream, total = 0;
	reset();
	memset(data, 17, size);
	for (draw = 0; draw < WEB_GEOMETRY_STREAM_BYTES / size + 20; draw++)
	{
		web_geometry_begin_draw(draw / 48);
		contents(web_geometry_stream(GL_ARRAY_BUFFER, data, size), data, size);
		assert(web_geometry.stream_bytes <= WEB_GEOMETRY_STREAM_BYTES);
	}
	assert(web_geometry.stats.trimmed_buffers > 0);
	web_geometry_begin_draw(4);
	for (stream = 0; stream < 16; stream++)
	{
		data[0] = (unsigned char)stream;
		streams[stream] = web_geometry_stream(GL_ARRAY_BUFFER, data, size);
	}
	for (stream = 0; stream < 16; stream++)
	{
		data[0] = (unsigned char)stream;
		contents(streams[stream], data, size);
	}
	for (stream = 1; stream <= next_buffer; stream++) total += buffers[stream].size;
	assert(total == web_geometry.stream_bytes && total <= WEB_GEOMETRY_STREAM_BYTES);
	free(data);
}

int main(void)
{
	retained_vertices();
	multistream_aliases();
	rotating_streams();
	bounded_cache_and_collisions();
	bounded_stream_storage();
	reset();
	puts("web geometry: byte invalidation, retained uploads, stream isolation, frame rotation, exact reads and cache bounds passed");
	return 0;
}
