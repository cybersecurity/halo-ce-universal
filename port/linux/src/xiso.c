/*
XISO.C

Copies the maps folder out of an Xbox disc image (an "xiso"), for the
desktop ports' first start without game data (sdl_platform.c).

The image's file system is XDVDFS, read as extract-xiso does
(https://github.com/XboxDev/extract-xiso, extract-xiso.c, whose format
handling this follows; its license is below and in
port/third_party/extract-xiso/LICENSE.TXT): 2048-byte sectors; a
volume descriptor at 0x10000 that starts and ends with
"MICROSOFT*XBOX*MEDIA" and gives the root directory's sector and size; and
directories whose entries form a binary tree (each entry: left and right
subtree offsets in 4-byte units, the start sector, the size, attributes, the
name's length and the name, 4-byte aligned). Images made from a whole disc
put the game partition further in, at one of the offsets below.

The files are written to <destination>/maps.partial first, which becomes
<destination>/maps once they all are, so an interrupted extraction never
leaves a maps folder that looks complete.

Some parts of this code are copyright in@fishtank.com. This product
includes software developed by in <in@fishtank.com>.

 * Copyright (c) 2003 in <in@fishtank.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *
 *    This product includes software developed by in <in@fishtank.com>.
 *
 * 4. Neither the name of "in" nor the email address "in@fishtank.com"
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED `AS IS' AND ANY EXPRESS OR IMPLIED WARRANTIES
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE
 * AUTHOR OR ANY CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

/* (the desktop ports only: the Android app imports the game data itself) */
#ifndef HALO_ANDROID

#ifdef HALO_MACOS
/* The Mac importer runs in the native host, outside the guest's XDK ABI. */
#include <stdarg.h>
#include <stdio.h>
static void platform_log(const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    vfprintf(stderr, format, arguments);
    va_end(arguments);
    fputc('\n', stderr);
}
#else
#include "platform.h"
#endif
#include "posix.h"
#include "xiso.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef O_LARGEFILE
#define O_LARGEFILE 0
#endif
#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

enum
{
	SECTOR_SIZE = 2048,
	VOLUME_DESCRIPTOR_OFFSET = 0x10000,
	ENTRY_HEADER_SIZE = 14,
	ATTRIBUTE_DIRECTORY = 0x10,
	/* directory tables are a few sectors; anything much larger is not one */
	MAXIMUM_DIRECTORY_SIZE = 4 << 20,
	MAXIMUM_FILES = 256,
	COPY_BUFFER_SIZE = 1 << 20,
};

static const char volume_magic[] = "MICROSOFT*XBOX*MEDIA";

/* where the game partition starts: a plain image, and whole-disc images
(extract-xiso's GLOBAL, XGD3 and XGD1 offsets) */
static const unsigned long long partition_offsets[] = { 0, 0x0FD90000ull, 0x02080000ull, 0x18300000ull };

struct xiso_file
{
	char name[256];
	unsigned long sector;
	unsigned long size;
};

struct xiso_image
{
	int descriptor;
	unsigned long long partition;
	char *error;
	int error_size;
};

static unsigned long read_u32(const unsigned char *bytes)
{
	return (unsigned long)bytes[0] | (unsigned long)bytes[1] << 8 | (unsigned long)bytes[2] << 16 |
		(unsigned long)bytes[3] << 24;
}

static int fail(struct xiso_image *image, const char *format, const char *detail)
{
	snprintf(image->error, (size_t)image->error_size, format, detail ? detail : "");
	return 0;
}

/* reads size bytes at offset (from the start of the file) */
static int read_at(struct xiso_image *image, unsigned long long offset, void *buffer, unsigned long size)
{
	posix_ulong low, high;
	unsigned char *cursor = buffer;

	if (posix_seek(image->descriptor, (posix_long)(offset & 0xFFFFFFFFu), (posix_long)(offset >> 32), SEEK_SET,
		&low, &high) != 0)
	{
		return 0;
	}
	while (size > 0)
	{
		long count = (long)read(image->descriptor, cursor, size > COPY_BUFFER_SIZE ? COPY_BUFFER_SIZE : size);

		if (count <= 0)
			return 0;
		cursor += count;
		size -= (unsigned long)count;
	}
	return 1;
}

/* the partition's volume descriptor: the root directory's sector and size */
static int find_volume(struct xiso_image *image, unsigned long *root_sector, unsigned long *root_size)
{
	int index;

	for (index = 0; index < (int)(sizeof(partition_offsets) / sizeof(*partition_offsets)); index++)
	{
		unsigned char descriptor[SECTOR_SIZE];
		unsigned long long offset = partition_offsets[index] + VOLUME_DESCRIPTOR_OFFSET;

		if (!read_at(image, offset, descriptor, sizeof(descriptor)))
			continue;
		if (memcmp(descriptor, volume_magic, 20) || memcmp(descriptor + 0x7EC, volume_magic, 20))
			continue;
		image->partition = partition_offsets[index];
		*root_sector = read_u32(descriptor + 20);
		*root_size = read_u32(descriptor + 24);
		return 1;
	}
	return fail(image, "This is not an Xbox disc image.%s", NULL);
}

/* a directory's table, read whole */
static unsigned char *read_directory(struct xiso_image *image, unsigned long sector, unsigned long size)
{
	unsigned char *table;

	if (!size || size > MAXIMUM_DIRECTORY_SIZE)
		return NULL;
	table = malloc(size);
	if (table && !read_at(image, image->partition + (unsigned long long)sector * SECTOR_SIZE, table, size))
	{
		free(table);
		table = NULL;
	}
	return table;
}

struct directory_walk
{
	const unsigned char *table;
	unsigned long size;
	struct xiso_file *entries;
	int entry_count;
	int maximum_count;
	int visited;
	int directories;
};

/* collects the entries of the subtree at offset (4-byte units), files or
directories as asked */
static void walk_directory(struct directory_walk *walk, unsigned long offset, int depth)
{
	const unsigned char *entry;
	unsigned long left, right;
	int name_length;

	offset *= 4;
	/* a malformed table must not loop or run off its end */
	if (depth > 64 || ++walk->visited > 4096 || offset + ENTRY_HEADER_SIZE > walk->size)
		return;
	entry = walk->table + offset;
	left = (unsigned long)entry[0] | (unsigned long)entry[1] << 8;
	right = (unsigned long)entry[2] | (unsigned long)entry[3] << 8;
	/* 0xFFFF: padding, an empty directory */
	if (left == 0xFFFF)
		return;
	name_length = entry[13];
	if (left)
		walk_directory(walk, left, depth + 1);
	if (offset + ENTRY_HEADER_SIZE + (unsigned long)name_length <= walk->size && name_length > 0 &&
		!(entry[12] & ATTRIBUTE_DIRECTORY) == !walk->directories && walk->entry_count < walk->maximum_count)
	{
		struct xiso_file *file = &walk->entries[walk->entry_count];

		memcpy(file->name, entry + ENTRY_HEADER_SIZE, (size_t)name_length);
		file->name[name_length] = 0;
		file->sector = read_u32(entry + 4);
		file->size = read_u32(entry + 8);
		/* (as extract-xiso refuses them: no name may leave the folder) */
		if (strcmp(file->name, ".") && strcmp(file->name, "..") && !strchr(file->name, '/') &&
			!strchr(file->name, '\\'))
		{
			walk->entry_count++;
		}
	}
	if (right)
		walk_directory(walk, right, depth + 1);
}

static int names_match(const char *a, const char *b)
{
	for (; *a && *b; a++, b++)
	{
		char x = *a >= 'A' && *a <= 'Z' ? (char)(*a - 'A' + 'a') : *a;
		char y = *b >= 'A' && *b <= 'Z' ? (char)(*b - 'A' + 'a') : *b;

		if (x != y)
			return 0;
	}
	return !*a && !*b;
}

static int copy_file(struct xiso_image *image, const struct xiso_file *file, const char *path,
	unsigned char *buffer, unsigned long long *done, unsigned long long total,
	xiso_progress_proc progress, void *context)
{
	unsigned long long offset = image->partition + (unsigned long long)file->sector * SECTOR_SIZE;
	unsigned long remaining = file->size;
	int output = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);

	if (output < 0)
		return fail(image, "Could not write %s.", path);
	while (remaining > 0)
	{
		unsigned long count = remaining > COPY_BUFFER_SIZE ? COPY_BUFFER_SIZE : remaining;
		unsigned long written = 0;

		if (!read_at(image, offset, buffer, count))
		{
			close(output);
			return fail(image, "Could not read %s from the disc image (is it complete?).", file->name);
		}
		while (written < count)
		{
			long result = (long)write(output, buffer + written, count - written);

			if (result <= 0)
			{
				close(output);
				return fail(image, "Could not write %s (is the disk full?).", path);
			}
			written += (unsigned long)result;
		}
		offset += count;
		remaining -= count;
		*done += count;
		if (progress)
			progress(context, file->name, *done, total);
	}
	if (close(output) != 0)
		return fail(image, "Could not write %s (is the disk full?).", path);
	return 1;
}

int xiso_extract_maps(const char *image_path, const char *destination, xiso_progress_proc progress, void *context,
	char *error, int error_size)
{
	struct xiso_image image;
	struct xiso_file *entries = NULL;
	unsigned char *table = NULL;
	unsigned char *buffer = NULL;
	unsigned long root_sector, root_size;
	unsigned long long total = 0, done = 0;
	char partial[1024], final[1024], path[1300];
	int result = 0;
	int index;

	image.error = error;
	image.error_size = error_size;
	image.descriptor = open(image_path, O_RDONLY | O_LARGEFILE | O_CLOEXEC);
	if (image.descriptor < 0)
		return fail(&image, "Could not open %s.", image_path);
	entries = calloc(MAXIMUM_FILES, sizeof(*entries));
	buffer = malloc(COPY_BUFFER_SIZE);
	if (!entries || !buffer)
	{
		fail(&image, "Out of memory.%s", NULL);
		goto done;
	}
	if (!find_volume(&image, &root_sector, &root_size))
		goto done;

	/* the root's maps folder */
	table = read_directory(&image, root_sector, root_size);
	if (!table)
	{
		fail(&image, "The disc image's file system is damaged.%s", NULL);
		goto done;
	}
	{
		struct directory_walk walk = { table, root_size, entries, 0, MAXIMUM_FILES, 0, 1 };

		walk_directory(&walk, 0, 0);
		for (index = 0; index < walk.entry_count && !names_match(entries[index].name, "maps"); index++)
			;
		if (index == walk.entry_count)
		{
			fail(&image, "The disc image has no maps folder: it is not a Halo disc.%s", NULL);
			goto done;
		}
		free(table);
		table = read_directory(&image, entries[index].sector, entries[index].size);
		if (!table)
		{
			fail(&image, "The disc image's maps folder is damaged.%s", NULL);
			goto done;
		}
		root_size = entries[index].size;
	}

	/* its files */
	{
		struct directory_walk walk = { table, root_size, entries, 0, MAXIMUM_FILES, 0, 0 };
		int has_ui = 0;

		walk_directory(&walk, 0, 0);
		for (index = 0; index < walk.entry_count; index++)
		{
			total += entries[index].size;
			has_ui |= names_match(entries[index].name, "ui.map");
		}
		if (!has_ui)
		{
			fail(&image, "The disc image's maps folder has no ui.map: it is not a Halo disc.%s", NULL);
			goto done;
		}
		snprintf(partial, sizeof(partial), "%s/maps.partial", destination);
		snprintf(final, sizeof(final), "%s/maps", destination);
		posix_make_directory(partial);
		for (index = 0; index < walk.entry_count; index++)
		{
			snprintf(path, sizeof(path), "%s/%s", partial, entries[index].name);
			platform_log("extracting maps/%s (%lu bytes)", entries[index].name, entries[index].size);
			if (!copy_file(&image, &entries[index], path, buffer, &done, total, progress, context))
				goto done;
		}
		if (rename(partial, final) != 0)
		{
			fail(&image, "Could not create %s.", final);
			goto done;
		}
	}
	result = 1;

done:
	close(image.descriptor);
	free(table);
	free(entries);
	free(buffer);
	return result;
}

#endif
