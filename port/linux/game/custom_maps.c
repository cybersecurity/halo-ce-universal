/* Discover compatible Xbox v5 multiplayer maps once per process. The
   platform file API matches names and .map extensions case-insensitively.
   Custom identities are cache basenames, fitting the engine's 64-byte stage
   name without inventing a stock levels\\test\\<name>\\<name> path. */
#include "cseries.h"
#include "cache/cache_files.h"
#include "halo_custom_maps.h"
#include <xtl.h>

static char *map_list[HALO_CUSTOM_MAP_LIMIT];
static char custom_names[HALO_CUSTOM_MAP_LIMIT][HALO_CUSTOM_MAP_NAME_SIZE];
static short map_count;
static int initialized;

static char const *const stock_names[] = {
    "beavercreek", "sidewinder", "damnation", "ratrace", "prisoner",
    "hangemhigh", "chillout", "carousel", "boardingaction", "bloodgulch",
    "wizard", "putput", "longest"
};

char const *native_map_basename(char const *map)
{
    char const *p, *name = map;
    if (!map) return "";
    for (p = map; *p; p++)
        if (*p == '\\' || *p == '/') name = p + 1;
    return name;
}

int native_map_is_custom(char const *map)
{
    unsigned int i;
    char const *name = native_map_basename(map);
    for (i = 0; i < NUMBEROF(stock_names); i++)
        if (!_stricmp(name, stock_names[i])) return FALSE;
    return name[0] != 0;
}

void native_map_display_name(char const *map, char *text, unsigned int size)
{
    unsigned int i;
    int capitalize = TRUE;
    char const *name = native_map_basename(map);
    if (!size) return;
    for (i = 0; i + 1 < size && name[i]; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c == '_') c = ' ';
        if (capitalize && c >= 'a' && c <= 'z') c -= 'a' - 'A';
        text[i] = c;
        capitalize = c == ' ';
    }
    text[i] = 0;
}

static unsigned long little_u32(unsigned char const *p)
{
    return (unsigned long)p[0] | (unsigned long)p[1] << 8 |
        (unsigned long)p[2] << 16 | (unsigned long)p[3] << 24;
}

int native_map_header_valid(unsigned char const *header, char const *filename)
{
    char name[32], build[32];
    unsigned long length, tag_offset, tag_size;
    unsigned int i, name_length;
    if (memcmp(header, "daeh", 4) || memcmp(header + 0x7FC, "toof", 4) ||
        little_u32(header + 4) != 5 || header[0x60] != 1 || header[0x61] != 0 ||
        !memchr(header + 0x20, 0, 32) || !memchr(header + 0x40, 0, 32))
        return FALSE;
    memcpy(name, header + 0x20, 32);
    memcpy(build, header + 0x40, 32);
    length = little_u32(header + 8);
    tag_offset = little_u32(header + 0x10);
    tag_size = little_u32(header + 0x14);
    if (length < 2048 || length > HALO_PORT_MULTIPLAYER_CACHE_SIZE ||
        tag_offset < 2048 || tag_offset > length ||
        /* Nine 32-bit fields in the Xbox cache_file_tag_header. */
        tag_size < 0x24 || tag_size > 0x01600000 || tag_size > length - tag_offset ||
        !cache_files_build_region(build)) return FALSE;
    name_length = (unsigned int)strlen(name);
    if (!name_length || strlen(filename) != name_length + 4 ||
        _strnicmp(filename, name, name_length) ||
        _stricmp(filename + name_length, ".map")) return FALSE;
    for (i = 0; i < name_length; i++) {
        unsigned char c = (unsigned char)name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-' || c == ' '))
            return FALSE;
    }
    return TRUE;
}

static int map_compare(void const *a, void const *b)
{
    return _stricmp(*(char *const *)a, *(char *const *)b);
}

char **native_multiplayer_map_list(char **stock, short stock_count, short *count)
{
    WIN32_FIND_DATAA entry;
    HANDLE search;
    char pattern[256];
    if (!initialized) {
        short i;
        /* Only the established stock list belongs ahead of discovered maps. */
        if (!stock || stock_count != HALO_STOCK_MULTIPLAYER_MAP_COUNT) {
            *count = 0;
            return map_list;
        }
        initialized = TRUE;
        map_count = stock_count;
        for (i = 0; i < stock_count; i++) map_list[i] = stock[i];
        if (strlen(cache_files_map_directory()) + 5 >= sizeof(pattern)) {
            *count = map_count;
            return map_list;
        }
        snprintf(pattern, sizeof(pattern), "%s*.map", cache_files_map_directory());
        search = FindFirstFileA(pattern, &entry);
        if (search != INVALID_HANDLE_VALUE) {
            do {
                unsigned char header[2048];
                unsigned long read;
                HANDLE file;
                char path[256];
                int duplicate = FALSE;
                if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
                    strlen(entry.cFileName) + strlen(cache_files_map_directory()) >= sizeof(path))
                    continue;
                snprintf(path, sizeof(path), "%s%s", cache_files_map_directory(), entry.cFileName);
                file = CreateFileA(path, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
                if (file == INVALID_HANDLE_VALUE) continue;
                read = 0;
                if (!ReadFile(file, header, sizeof(header), &read, NULL)) read = 0;
                CloseHandle(file);
                if (read != sizeof(header) || !native_map_header_valid(header, entry.cFileName)) continue;
                for (i = 0; i < map_count; i++)
                    if (!_stricmp(native_map_basename(map_list[i]), (char *)header + 0x20)) duplicate = TRUE;
                if (duplicate) continue;
                if (map_count == HALO_CUSTOM_MAP_LIMIT) {
                    fprintf(stderr, "[maps] multiplayer map list is full (%d)\n", HALO_CUSTOM_MAP_LIMIT);
                    break;
                }
                memcpy(custom_names[map_count], header + 0x20, HALO_CUSTOM_MAP_NAME_SIZE);
                map_list[map_count] = custom_names[map_count];
                map_count++;
            } while (FindNextFileA(search, &entry));
            CloseHandle(search);
        }
        qsort(map_list + stock_count, map_count - stock_count, sizeof(map_list[0]), map_compare);
        fprintf(stderr, "[maps] %d stock and %d custom multiplayer maps\n", stock_count, map_count - stock_count);
    }
    *count = map_count;
    return map_list;
}
