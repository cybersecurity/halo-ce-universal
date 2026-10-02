/*
XBOX_XAPI.C

Xbox-specific XAPI services for the Linux build: launch information,
language, save game containers, content signatures, nicknames and the
controller/memory unit device interfaces.

Save games follow the Xbox layout: each save is a directory
<root>\UDATA\<id>\ whose display name is kept in SaveMeta.xbx. Content
signatures are SHA-1 digests keyed with a fixed title key, which is what the
game needs (a stable, content-dependent signature it can verify later).

There is no controller backend yet: no gamepad or memory unit is ever
reported as inserted, so the game runs with no input devices.
*/

#include "platform.h"
#include "port_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* ---------- launch and system settings */

DWORD WINAPI XGetLaunchInfo(PDWORD launch_data_type, PLAUNCH_DATA launch_data)
{
	/* launched from the "dashboard": no title-specific launch data */
	if (launch_data_type)
		*launch_data_type = LDT_FROM_DASHBOARD;
	if (launch_data)
		memset(launch_data, 0, sizeof(*launch_data));
	return ERROR_SUCCESS;
}

DWORD WINAPI XLaunchNewImageA(LPCSTR image_path, PLAUNCH_DATA launch_data)
{
	(void)launch_data;
	/* On the Xbox this reboots into another executable and never returns */
	platform_log("XLaunchNewImage(\"%s\"): exiting", image_path ? image_path : "(dashboard)");
#ifdef HALO_WEB
	{
		/* the page offers to start again (port/web/src/web_library.js) */
		extern void web_js_post(int kind, const char *text);

		web_js_post(5, "The game quit to the Xbox dashboard.");
	}
#endif
	exit(EXIT_SUCCESS);
}

DWORD WINAPI XGetLanguage(void)
{
	const char *language = config_string("game.language");

	if (*language)
	{
		if (!strncmp(language, "ja", 2)) return XC_LANGUAGE_JAPANESE;
		if (!strncmp(language, "de", 2)) return XC_LANGUAGE_GERMAN;
		if (!strncmp(language, "fr", 2)) return XC_LANGUAGE_FRENCH;
		if (!strncmp(language, "es", 2)) return XC_LANGUAGE_SPANISH;
		if (!strncmp(language, "it", 2)) return XC_LANGUAGE_ITALIAN;
	}
	return XC_LANGUAGE_ENGLISH;
}

/* ---------- save games */

#define SAVE_DATA_DIRECTORY "UDATA"
#define SAVE_META_FILE "SaveMeta.xbx"

static unsigned long long save_name_hash(LPCWSTR name)
{
	/* FNV-1a over the UTF-16 code units */
	unsigned long long hash = 1469598103934665603ULL;

	for (; *name; name++)
	{
		hash ^= (unsigned long long)*name;
		hash *= 1099511628211ULL;
	}
	return hash;
}

static void save_directory_for(LPCSTR root, LPCWSTR name, char *directory, unsigned long size)
{
	unsigned long long hash = save_name_hash(name);
	unsigned long length = (unsigned long)strlen(root);
	const char *separator = (length && (root[length - 1] == '\\' || root[length - 1] == '/')) ? "" : "\\";

	snprintf(directory, size, "%s%s" SAVE_DATA_DIRECTORY "\\%012llX\\", root, separator, hash & 0xffffffffffffULL);
}

static BOOL save_read_name(const char *directory, WCHAR *name, unsigned long name_count)
{
	char meta_path[MAX_PATH + 32];
	HANDLE file;
	DWORD read = 0;
	BOOL success;

	snprintf(meta_path, sizeof(meta_path), "%s" SAVE_META_FILE, directory);
	file = CreateFileA(meta_path, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return FALSE;
	memset(name, 0, name_count * sizeof(WCHAR));
	success = ReadFile(file, name, (name_count - 1) * sizeof(WCHAR), &read, NULL);
	CloseHandle(file);
	return success;
}

DWORD WINAPI XCreateSaveGame(LPCSTR root_path_name, LPCWSTR save_game_name, DWORD creation_disposition,
	DWORD create_flags, LPSTR path_buffer, UINT buffer_size)
{
	char directory[MAX_PATH];
	char data_directory[MAX_PATH];
	char meta_path[MAX_PATH + 32];
	unsigned long root_length = (unsigned long)strlen(root_path_name);
	DWORD attributes;
	HANDLE file;
	DWORD written;

	(void)create_flags;
	save_directory_for(root_path_name, save_game_name, directory, sizeof(directory));
	attributes = GetFileAttributesA(directory);
	if (attributes != (DWORD)-1)
	{
		if (creation_disposition == CREATE_NEW)
			return ERROR_ALREADY_EXISTS;
	}
	else
	{
		if (creation_disposition == OPEN_EXISTING)
			return ERROR_PATH_NOT_FOUND;
		snprintf(data_directory, sizeof(data_directory), "%s%s" SAVE_DATA_DIRECTORY, root_path_name,
			(root_length && (root_path_name[root_length - 1] == '\\' || root_path_name[root_length - 1] == '/')) ? "" : "\\");
		CreateDirectoryA(root_path_name, NULL);
		CreateDirectoryA(data_directory, NULL);
		if (!CreateDirectoryA(directory, NULL))
			return GetLastError();
		snprintf(meta_path, sizeof(meta_path), "%s" SAVE_META_FILE, directory);
		file = CreateFileA(meta_path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
		if (file == INVALID_HANDLE_VALUE)
			return GetLastError();
		WriteFile(file, save_game_name, (DWORD)(wcslen(save_game_name) * sizeof(WCHAR)), &written, NULL);
		CloseHandle(file);
	}
	if (path_buffer && buffer_size)
	{
		if (strlen(directory) + 1 > buffer_size)
			return ERROR_INSUFFICIENT_BUFFER;
		strcpy(path_buffer, directory);
	}
	return ERROR_SUCCESS;
}

static void delete_tree(const char *xbox_directory)
{
	char pattern[MAX_PATH + 4];
	WIN32_FIND_DATAA data;
	HANDLE find;

	snprintf(pattern, sizeof(pattern), "%s*", xbox_directory);
	find = FindFirstFileA(pattern, &data);
	if (find != INVALID_HANDLE_VALUE)
	{
		do
		{
			char child[MAX_PATH * 2];

			snprintf(child, sizeof(child), "%s%s", xbox_directory, data.cFileName);
			if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			{
				strcat(child, "\\");
				delete_tree(child);
			}
			else
			{
				DeleteFileA(child);
			}
		} while (FindNextFileA(find, &data));
		CloseHandle(find);
	}
	RemoveDirectoryA(xbox_directory);
}

DWORD WINAPI XDeleteSaveGame(LPCSTR root_path_name, LPCWSTR save_game_name)
{
	char directory[MAX_PATH];

	save_directory_for(root_path_name, save_game_name, directory, sizeof(directory));
	if (GetFileAttributesA(directory) == (DWORD)-1)
		return ERROR_PATH_NOT_FOUND;
	delete_tree(directory);
	return ERROR_SUCCESS;
}

struct save_game_find
{
	HANDLE directory_find;
	char data_directory[MAX_PATH];
};

static void save_game_find_destroy(struct platform_handle *handle)
{
	struct save_game_find *find = handle->data;

	if (find->directory_find != INVALID_HANDLE_VALUE)
		CloseHandle(find->directory_find);
	free(find);
}

static BOOL save_game_find_fill(struct save_game_find *find, WIN32_FIND_DATAA *entry, PXGAME_FIND_DATA data)
{
	do
	{
		if (!(entry->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
			continue;
		memset(data, 0, sizeof(*data));
		data->wfd = *entry;
		snprintf(data->szSaveGameDirectory, sizeof(data->szSaveGameDirectory), "%s%s\\",
			find->data_directory, entry->cFileName);
		if (save_read_name(data->szSaveGameDirectory, data->szSaveGameName, MAX_GAMENAME))
			return TRUE;
	} while (FindNextFileA(find->directory_find, entry));
	SetLastError(ERROR_NO_MORE_FILES);
	return FALSE;
}

HANDLE WINAPI XFindFirstSaveGame(LPCSTR root_path_name, PXGAME_FIND_DATA find_game_data)
{
	struct save_game_find *find = calloc(1, sizeof(*find));
	struct platform_handle *handle;
	unsigned long root_length = (unsigned long)strlen(root_path_name);
	char pattern[MAX_PATH + 4];
	WIN32_FIND_DATAA entry;

	if (!find)
	{
		SetLastError(ERROR_NOT_ENOUGH_MEMORY);
		return INVALID_HANDLE_VALUE;
	}
	snprintf(find->data_directory, sizeof(find->data_directory), "%s%s" SAVE_DATA_DIRECTORY "\\", root_path_name,
		(root_length && (root_path_name[root_length - 1] == '\\' || root_path_name[root_length - 1] == '/')) ? "" : "\\");
	snprintf(pattern, sizeof(pattern), "%s*", find->data_directory);
	find->directory_find = FindFirstFileA(pattern, &entry);
	if (find->directory_find == INVALID_HANDLE_VALUE || !save_game_find_fill(find, &entry, find_game_data))
	{
		if (find->directory_find != INVALID_HANDLE_VALUE)
			CloseHandle(find->directory_find);
		free(find);
		SetLastError(ERROR_NO_MORE_FILES);
		return INVALID_HANDLE_VALUE;
	}
	handle = platform_handle_new(_platform_handle_find, find, save_game_find_destroy);
	if (!handle)
	{
		CloseHandle(find->directory_find);
		free(find);
		SetLastError(ERROR_NOT_ENOUGH_MEMORY);
		return INVALID_HANDLE_VALUE;
	}
	/* distinguish from plain FindFirstFile handles */
	handle->type = _platform_handle_other;
	return handle;
}

BOOL WINAPI XFindNextSaveGame(HANDLE find_game, PXGAME_FIND_DATA find_game_data)
{
	struct platform_handle *handle = platform_handle_get(find_game, _platform_handle_other);
	struct save_game_find *find;
	WIN32_FIND_DATAA entry;

	if (!handle)
		return FALSE;
	find = handle->data;
	if (!FindNextFileA(find->directory_find, &entry))
		return FALSE;
	return save_game_find_fill(find, &entry, find_game_data);
}

BOOL WINAPI XFindClose(HANDLE find)
{
	return CloseHandle(find);
}

/* ---------- nicknames (recently used names, per title) */

static WCHAR last_nickname[MAX_NICKNAME];

BOOL WINAPI XSetNicknameW(LPCWSTR nickname, BOOL preserve_case)
{
	(void)preserve_case;
	wcsncpy(last_nickname, nickname, MAX_NICKNAME - 1);
	last_nickname[MAX_NICKNAME - 1] = 0;
	return TRUE;
}

HANDLE WINAPI XFindFirstNicknameW(BOOL this_title_only, LPWSTR nickname, UINT size)
{
	(void)this_title_only;
	if (!last_nickname[0] || !size)
	{
		SetLastError(ERROR_NO_MORE_FILES);
		return INVALID_HANDLE_VALUE;
	}
	wcsncpy(nickname, last_nickname, size - 1);
	nickname[size - 1] = 0;
	return platform_handle_new(_platform_handle_other, NULL, NULL);
}

/* ---------- content signatures (SHA-1, keyed with a fixed title key) */

struct sha1_context
{
	unsigned long state[5];
	unsigned long long length;
	unsigned char buffer[64];
	unsigned long buffered;
};

#define SHA1_ROTATE(value, bits) (((value) << (bits)) | ((value) >> (32 - (bits))))

static void sha1_block(struct sha1_context *context, const unsigned char *block)
{
	unsigned long words[80];
	unsigned long a, b, c, d, e;
	int index;

	for (index = 0; index < 16; index++)
	{
		words[index] = ((unsigned long)block[index * 4] << 24) | ((unsigned long)block[index * 4 + 1] << 16) |
			((unsigned long)block[index * 4 + 2] << 8) | block[index * 4 + 3];
	}
	for (; index < 80; index++)
		words[index] = SHA1_ROTATE(words[index - 3] ^ words[index - 8] ^ words[index - 14] ^ words[index - 16], 1);

	a = context->state[0];
	b = context->state[1];
	c = context->state[2];
	d = context->state[3];
	e = context->state[4];
	for (index = 0; index < 80; index++)
	{
		unsigned long f, k, temporary;

		if (index < 20) { f = (b & c) | (~b & d); k = 0x5a827999; }
		else if (index < 40) { f = b ^ c ^ d; k = 0x6ed9eba1; }
		else if (index < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8f1bbcdc; }
		else { f = b ^ c ^ d; k = 0xca62c1d6; }
		temporary = SHA1_ROTATE(a, 5) + f + e + k + words[index];
		e = d;
		d = c;
		c = SHA1_ROTATE(b, 30);
		b = a;
		a = temporary;
	}
	context->state[0] += a;
	context->state[1] += b;
	context->state[2] += c;
	context->state[3] += d;
	context->state[4] += e;
}

static void sha1_initialize(struct sha1_context *context)
{
	memset(context, 0, sizeof(*context));
	context->state[0] = 0x67452301;
	context->state[1] = 0xefcdab89;
	context->state[2] = 0x98badcfe;
	context->state[3] = 0x10325476;
	context->state[4] = 0xc3d2e1f0;
}

static void sha1_update(struct sha1_context *context, const unsigned char *data, unsigned long size)
{
	context->length += size;
	while (size)
	{
		unsigned long count = 64 - context->buffered;

		if (count > size)
			count = size;
		memcpy(context->buffer + context->buffered, data, count);
		context->buffered += count;
		data += count;
		size -= count;
		if (context->buffered == 64)
		{
			sha1_block(context, context->buffer);
			context->buffered = 0;
		}
	}
}

static void sha1_finish(struct sha1_context *context, unsigned char digest[20])
{
	unsigned long long bits = context->length * 8;
	unsigned char padding = 0x80;
	unsigned char length_bytes[8];
	int index;

	sha1_update(context, &padding, 1);
	padding = 0;
	while (context->buffered != 56)
		sha1_update(context, &padding, 1);
	for (index = 0; index < 8; index++)
		length_bytes[index] = (unsigned char)(bits >> (56 - index * 8));
	sha1_update(context, length_bytes, 8);
	for (index = 0; index < 20; index++)
		digest[index] = (unsigned char)(context->state[index / 4] >> (24 - (index % 4) * 8));
}

static const unsigned char signature_title_key[] = "halo-linux content signature";

static void signature_destroy(struct platform_handle *handle)
{
	free(handle->data);
}

HANDLE WINAPI XCalculateSignatureBegin(DWORD flags)
{
	struct sha1_context *context = malloc(sizeof(*context));
	struct platform_handle *handle;

	(void)flags;
	if (!context)
	{
		SetLastError(ERROR_NOT_ENOUGH_MEMORY);
		return INVALID_HANDLE_VALUE;
	}
	sha1_initialize(context);
	sha1_update(context, signature_title_key, sizeof(signature_title_key));
	handle = platform_handle_new(_platform_handle_signature, context, signature_destroy);
	if (!handle)
	{
		free(context);
		SetLastError(ERROR_NOT_ENOUGH_MEMORY);
		return INVALID_HANDLE_VALUE;
	}
	return handle;
}

DWORD WINAPI XCalculateSignatureUpdate(HANDLE calculation, const BYTE *data, ULONG size)
{
	struct platform_handle *handle = platform_handle_get(calculation, _platform_handle_signature);

	if (!handle)
		return ERROR_INVALID_HANDLE;
	sha1_update(handle->data, data, size);
	return ERROR_SUCCESS;
}

DWORD WINAPI XCalculateSignatureEnd(HANDLE calculation, PXCALCSIG_SIGNATURE signature)
{
	struct platform_handle *handle = platform_handle_get(calculation, _platform_handle_signature);

	if (!handle)
		return ERROR_INVALID_HANDLE;
	if (signature)
		sha1_finish(handle->data, signature->Signature);
	CloseHandle(calculation);
	return ERROR_SUCCESS;
}

/* devices: xinput_sdl.c */
