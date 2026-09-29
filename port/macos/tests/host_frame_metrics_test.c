#include "host_frame_metrics.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
	struct halo_frame_metrics metrics = {0};
	uint64_t timestamp = 1000000000;
	assert(!halo_frame_metrics_add(&metrics, timestamp, 0));
	for (unsigned i = 0; i < HALO_FRAME_SAMPLE_COUNT; ++i)
	{
		timestamp += 16666667;
		assert(halo_frame_metrics_add(&metrics, timestamp, 1000000) == (i == 119));
	}
	assert(metrics.elapsed_ns == UINT64_C(2000000040));
	assert(metrics.swap_ns == UINT64_C(120000000));
	assert(halo_frame_metrics_percentile(&metrics, 99) == 16666667);
	halo_frame_metrics_reset_window(&metrics);
	assert(metrics.count == 0 && metrics.previous_ns == timestamp);
	for (unsigned i = 1; i <= HALO_FRAME_SAMPLE_COUNT; ++i)
	{
		timestamp += i;
		(void)halo_frame_metrics_add(&metrics, timestamp, 0);
	}
	assert(halo_frame_metrics_percentile(&metrics, 50) == 60);
	assert(halo_frame_metrics_percentile(&metrics, 95) == 114);
	assert(halo_frame_metrics_percentile(&metrics, 99) == 119);
	assert(halo_frame_metrics_percentile(&metrics, 100) == 120);
	puts("host_frame_metrics_test: ok");
	return 0;
}
