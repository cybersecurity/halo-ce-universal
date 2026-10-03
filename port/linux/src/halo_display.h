/* Physical render size, independent of the Xbox's 480-line layout space. */
#ifndef HALO_DISPLAY_H
#define HALO_DISPLAY_H
#include <math.h>

struct halo_display_size { long width, height; };

static inline struct halo_display_size halo_display_render_size(
    long display_width, long display_height, long logical_width,
    long requested_width, long requested_height, long maximum_dimension)
{
    struct halo_display_size size;
    double ratio, height;
    if (display_width <= 0 || display_height <= 0) {
        display_width = logical_width;
        display_height = 480;
    }
    /* Auto uses the physical display's exact ratio, not the rounded number
       of logical columns. Explicit screen_width keeps its chosen aspect. */
    ratio = requested_width <= 0 ? (double)display_width / display_height :
        (double)logical_width / 480.0;
    if (ratio < 4.0 / 3.0) ratio = 4.0 / 3.0;
    if (ratio > 1600.0 / 480.0) ratio = 1600.0 / 480.0;
    height = fmin((double)display_height, (double)display_width / ratio);
    if (requested_height > 0) height = fmin(height, fmax(240.0, (double)requested_height));
    if (maximum_dimension <= 0) maximum_dimension = 8192;
    height = fmin(height, fmin((double)maximum_dimension, maximum_dimension / ratio));
    size.width = (long)floor(height * ratio + 0.5);
    size.height = (long)floor(height + 0.5);
    if (size.width < 1) size.width = 1;
    if (size.height < 1) size.height = 1;
    return size;
}
#endif
