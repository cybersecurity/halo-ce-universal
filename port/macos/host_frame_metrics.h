#ifndef HALO_HOST_FRAME_METRICS_H
#define HALO_HOST_FRAME_METRICS_H

#include <stdint.h>
#include <stddef.h>

#define HALO_FRAME_SAMPLE_COUNT 120
struct halo_frame_metrics
{
	uint64_t previous_ns, elapsed_ns, swap_ns;
	uint64_t intervals[HALO_FRAME_SAMPLE_COUNT];
	unsigned count;
};

/* Called only on the GL thread, after a successful swap. Timestamp gaps
 * measure delivered frames; swap time separately includes vsync waits. */
static inline int halo_frame_metrics_add(struct halo_frame_metrics *metrics,
	uint64_t completed_ns, uint64_t swap_ns)
{
	if (!metrics->previous_ns || completed_ns <= metrics->previous_ns)
	{
		metrics->previous_ns = completed_ns;
		return 0;
	}
	uint64_t interval = completed_ns - metrics->previous_ns;
	metrics->previous_ns = completed_ns;
	metrics->intervals[metrics->count++] = interval;
	metrics->elapsed_ns += interval;
	metrics->swap_ns += swap_ns;
	return metrics->count == HALO_FRAME_SAMPLE_COUNT;
}

static inline uint64_t halo_frame_metrics_percentile(const struct halo_frame_metrics *metrics,
	unsigned percentile)
{
	uint64_t sorted[HALO_FRAME_SAMPLE_COUNT];
	if (!metrics->count || percentile > 100) return 0;
	for (unsigned i = 0; i < metrics->count; ++i)
	{
		uint64_t value = metrics->intervals[i];
		unsigned j = i;
		while (j && sorted[j - 1] > value) { sorted[j] = sorted[j - 1]; --j; }
		sorted[j] = value;
	}
	unsigned rank = (metrics->count * percentile + 99) / 100;
	return sorted[rank ? rank - 1 : 0];
}

static inline void halo_frame_metrics_reset_window(struct halo_frame_metrics *metrics)
{
	metrics->count = 0;
	metrics->elapsed_ns = 0;
	metrics->swap_ns = 0;
}

#endif
