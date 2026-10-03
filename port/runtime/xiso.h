#ifndef HALO_XISO_H
#define HALO_XISO_H
#include <stddef.h>
#include <stdint.h>

/* Return zero to cancel. Called on the extraction thread. */
typedef int (*xiso_progress_proc)(void *context, const char *file, uint64_t done, uint64_t total);

/* Validate original Xbox cache headers and the complete campaign map set. */
int xiso_maps_ready(const char *maps, char *error, size_t error_size);

/* Extract into a new <destination>/maps, staging in maps.partial first.
   destination must be a private, existing directory. Existing files are never
   replaced. On failure the caller removes its private staging directory. */
int xiso_extract_maps(const char *image_path, const char *destination,
    xiso_progress_proc progress, void *context, char *error, size_t error_size);
#endif
