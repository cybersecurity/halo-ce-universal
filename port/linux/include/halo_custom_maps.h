/* Compatible Xbox v5 multiplayer caches present when the game starts. */
#ifndef __HALO_CUSTOM_MAPS_H
#define __HALO_CUSTOM_MAPS_H

#define HALO_CUSTOM_MAP_LIMIT 128
#define HALO_CUSTOM_MAP_NAME_SIZE 32
#define HALO_STOCK_MULTIPLAYER_MAP_COUNT 13
/* A text box owns its caption instead of indexing an authored string list. */
#define HALO_CUSTOM_MAP_TEXT (-2)

char **native_multiplayer_map_list(char **stock, short stock_count, short *count);
char const *native_map_basename(char const *map);
int native_map_is_custom(char const *map);
int native_map_header_valid(unsigned char const *header, char const *filename);
void native_map_display_name(char const *map, char *text, unsigned int size);

#endif
