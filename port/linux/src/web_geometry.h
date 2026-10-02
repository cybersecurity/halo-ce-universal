/* Browser-only geometry uploads. Included after the GL binding cache.

   Small stream buffers rotate through three frame generations. Each upload
   owns a slot until its draw is submitted; wrapping a busy generation may
   orphan an earlier draw's buffer, never another stream of the current draw.

   Stable vertices use a bounded retained buffer cache. WebAssembly has no
   write faults, so pointer identity alone cannot establish immutability:
   compare every uploaded byte, including any colour conversion. Promote
   only after a second identical observation; constantly changing data stays
   in the stream pool. No code or binary from another renderer is included. */
#ifndef HALO_WEB_GEOMETRY_H
#define HALO_WEB_GEOMETRY_H

#include <stdint.h>

#define WEB_GEOMETRY_FRAMES 3
#define WEB_GEOMETRY_SLOTS 512
/* Target for retained stream storage, excluding copies a driver keeps for
submitted draws. One unusually large draw can exceed it: its own streams
must stay alive until submission, and later draws trim the excess. */
#define WEB_GEOMETRY_STREAM_BYTES (32UL * 1024 * 1024)
#define WEB_GEOMETRY_CACHE_BUCKETS 128
#define WEB_GEOMETRY_CACHE_WAYS 4
#define WEB_GEOMETRY_CACHE_BYTES (8UL * 1024 * 1024)
#define WEB_GEOMETRY_CACHE_ENTRY_BYTES (256UL * 1024)

struct web_geometry_slot
{
	GLuint buffer;
	unsigned long size;
	uint64_t draw;
};

struct web_geometry_entry
{
	const void *key;
	unsigned char *snapshot;
	unsigned long size;
	GLuint buffer;
	BOOL ready;
	uint64_t last_use, pinned_draw;
};

static struct
{
	BOOL enabled;
	unsigned long frame, next[2], snapshot_bytes, stream_bytes, trim_cursor;
	uint64_t draw;
	struct web_geometry_slot slots[WEB_GEOMETRY_FRAMES][2][WEB_GEOMETRY_SLOTS];
	struct web_geometry_entry entries[WEB_GEOMETRY_CACHE_BUCKETS][WEB_GEOMETRY_CACHE_WAYS];
	struct
	{
		unsigned long hits, misses, promotions, invalidations;
		unsigned long uploaded_bytes, avoided_bytes, comparisons;
		unsigned long trimmed_buffers, peak_stream_bytes;
	} stats;
} web_geometry;

static void web_geometry_trim_streams(const struct web_geometry_slot *keep, unsigned long size)
{
	unsigned long attempts;

	for (attempts = 0; attempts < WEB_GEOMETRY_FRAMES * 2 * WEB_GEOMETRY_SLOTS &&
		web_geometry.stream_bytes - keep->size + size > WEB_GEOMETRY_STREAM_BYTES; attempts++)
	{
		unsigned long index = web_geometry.trim_cursor++ % (WEB_GEOMETRY_FRAMES * 2 * WEB_GEOMETRY_SLOTS);
		unsigned long frame = index / (2 * WEB_GEOMETRY_SLOTS);
		unsigned long kind = (index / WEB_GEOMETRY_SLOTS) % 2;
		struct web_geometry_slot *slot = &web_geometry.slots[frame][kind][index % WEB_GEOMETRY_SLOTS];

		if (!slot->size || slot == keep || slot->draw == web_geometry.draw)
			continue;
		if (kind)
			state_element_array_buffer(slot->buffer);
		else
			state_array_buffer(slot->buffer);
		glBufferData(kind ? GL_ELEMENT_ARRAY_BUFFER : GL_ARRAY_BUFFER, 0, NULL, GL_STREAM_DRAW);
		web_geometry.stream_bytes -= slot->size;
		slot->size = 0;
		web_geometry.stats.trimmed_buffers++;
	}
}

static void web_geometry_begin_draw(unsigned long frame)
{
	if (web_geometry.frame != frame)
	{
		web_geometry.frame = frame;
		web_geometry.next[0] = web_geometry.next[1] = 0;
	}
	web_geometry.draw++;
}

static GLuint web_geometry_stream(GLenum target, const void *data, unsigned long size)
{
	unsigned int kind = target == GL_ELEMENT_ARRAY_BUFFER;
	struct web_geometry_slot *slot;
	unsigned long attempts;

	for (attempts = 0; attempts < WEB_GEOMETRY_SLOTS; attempts++)
	{
		slot = &web_geometry.slots[web_geometry.frame % WEB_GEOMETRY_FRAMES][kind]
			[web_geometry.next[kind]++ % WEB_GEOMETRY_SLOTS];
		if (slot->draw != web_geometry.draw)
			break;
	}
	/* A draw has at most 16 vertex streams and one index stream. */
	if (attempts == WEB_GEOMETRY_SLOTS)
		return 0;
	if (!slot->buffer)
		glGenBuffers(1, &slot->buffer);
	slot->draw = web_geometry.draw;
	web_geometry_trim_streams(slot, size);
	if (kind)
		state_element_array_buffer(slot->buffer);
	else
		state_array_buffer(slot->buffer);
	/* Exact payload size: do not read padding beyond a rebased index array
	or an immediate vertex allocation. BufferData orphans prior draw data. */
	glBufferData(target, (GLsizeiptr)size, data, GL_STREAM_DRAW);
	web_geometry.stream_bytes = web_geometry.stream_bytes - slot->size + size;
	slot->size = size;
	if (web_geometry.stream_bytes > web_geometry.stats.peak_stream_bytes)
		web_geometry.stats.peak_stream_bytes = web_geometry.stream_bytes;
	web_geometry.stats.uploaded_bytes += size;
	return slot->buffer;
}

static GLuint web_geometry_vertices(const void *key, const void *data, unsigned long size)
{
	unsigned long hash = (((uintptr_t)key >> 4) ^ ((uintptr_t)key >> 15) ^ size) % WEB_GEOMETRY_CACHE_BUCKETS;
	struct web_geometry_entry *ways = web_geometry.entries[hash], *entry = NULL, *candidate = NULL;
	unsigned long way;

	if (!size || size > WEB_GEOMETRY_CACHE_ENTRY_BYTES)
		return web_geometry_stream(GL_ARRAY_BUFFER, data, size);
	for (way = 0; way < WEB_GEOMETRY_CACHE_WAYS; way++)
	{
		if (ways[way].key == key && ways[way].size == size)
		{
			entry = &ways[way];
			break;
		}
		if (ways[way].pinned_draw != web_geometry.draw &&
			(!candidate || ways[way].last_use < candidate->last_use))
			candidate = &ways[way];
	}
	if (entry)
	{
		entry->last_use = web_geometry.draw;
		web_geometry.stats.comparisons += size;
		if (!memcmp(entry->snapshot, data, size))
		{
			if (!entry->ready)
			{
				if (!entry->buffer)
					glGenBuffers(1, &entry->buffer);
				state_array_buffer(entry->buffer);
				glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)size, data, GL_STATIC_DRAW);
				entry->ready = TRUE;
				web_geometry.stats.promotions++;
				web_geometry.stats.uploaded_bytes += size;
			}
			else
			{
				web_geometry.stats.hits++;
				web_geometry.stats.avoided_bytes += size;
			}
			entry->pinned_draw = web_geometry.draw;
			return entry->buffer;
		}
		/* A source can be used with two different colour conversions by
		two streams in one draw. Keep the first stream's retained data alive. */
		if (entry->pinned_draw == web_geometry.draw)
			entry = NULL;
		else
			web_geometry.stats.invalidations++;
	}
	else
	{
		entry = candidate;
	}
	web_geometry.stats.misses++;
	if (entry && web_geometry.snapshot_bytes - entry->size + size <= WEB_GEOMETRY_CACHE_BYTES)
	{
		unsigned char *snapshot = entry->snapshot;

		if (entry->size != size)
			snapshot = malloc(size);
		if (snapshot)
		{
			/* Empty an old retained allocation before giving the entry a
			new source. Its earlier submitted draws retain the old storage. */
			if (entry->ready)
			{
				state_array_buffer(entry->buffer);
				glBufferData(GL_ARRAY_BUFFER, 0, NULL, GL_STATIC_DRAW);
			}
			if (snapshot != entry->snapshot)
				free(entry->snapshot);
			web_geometry.snapshot_bytes = web_geometry.snapshot_bytes - entry->size + size;
			entry->key = key;
			entry->snapshot = snapshot;
			entry->size = size;
			entry->ready = FALSE;
			entry->last_use = web_geometry.draw;
			memcpy(snapshot, data, size);
		}
	}
	return web_geometry_stream(GL_ARRAY_BUFFER, data, size);
}

#endif
