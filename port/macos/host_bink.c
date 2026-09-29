#include "host_bink.h"
#include "guest_address.h"
#include "guest_call.h"
#include <SDL3/SDL.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct halo_bink
{
	AVFormatContext *format;
	AVCodecContext *video, *audio;
	AVFrame *video_frame, *audio_frame;
	AVPacket *packet;
	struct SwsContext *scale;
	struct SwrContext *resample;
	SDL_AudioStream *output;
	int video_index, audio_index;
	unsigned char *pixels;
	struct halo_bink_info info;
	struct halo_bink_evidence evidence;
	uint64_t start_ns;
	uint64_t copied_decode_count, presented_decode_count;
	uint64_t first_present_ns, last_present_ns;
	uint64_t presentation_gaps[1024], gap_count, long_gaps;
	uint64_t gap_min_ns, gap_max_ns;
	int pending_close, tail_error, audio_preloaded;
	AVRational rate;
};
_Static_assert(sizeof(struct halo_bink_info) == 20, "BINK leading five guest DWORDs");

static int preload_audio(struct halo_bink *movie, const char *path);
static void audio_snapshot(struct halo_bink *movie, const char *phase);

static AVCodecContext *open_codec(AVStream *stream)
{
	const AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
	AVCodecContext *context = codec ? avcodec_alloc_context3(codec) : NULL;
	if (!context) return NULL;
	if (avcodec_parameters_to_context(context, stream->codecpar) < 0 ||
		avcodec_open2(context, codec, NULL) < 0) { avcodec_free_context(&context); return NULL; }
	return context;
}

struct halo_bink *mac_bink_open_path(const char *path, int audio_output)
{
	struct halo_bink *movie = calloc(1, sizeof(*movie));
	if (!movie) return NULL;
	movie->video_index = movie->audio_index = -1;
	if (avformat_open_input(&movie->format, path, NULL, NULL) < 0 ||
		avformat_find_stream_info(movie->format, NULL) < 0) goto fail;
	movie->video_index = av_find_best_stream(movie->format, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
	if (movie->video_index < 0) goto fail;
	AVStream *stream = movie->format->streams[movie->video_index];
	if (stream->codecpar->codec_id != AV_CODEC_ID_BINKVIDEO) goto fail;
	movie->video = open_codec(stream);
	if (!movie->video || movie->video->width <= 0 || movie->video->height <= 0 ||
		movie->video->width > 4096 || movie->video->height > 4096) goto fail;
	movie->rate = av_guess_frame_rate(movie->format, stream, NULL);
	if (movie->rate.num <= 0 || movie->rate.den <= 0) goto fail;
	int64_t frame_count = stream->nb_frames > 0 ? stream->nb_frames :
		av_rescale_q(stream->duration, stream->time_base, av_inv_q(movie->rate));
	if (frame_count <= 0 || frame_count > UINT32_MAX) goto fail;
	movie->info = (struct halo_bink_info){(uint32_t)movie->video->width,
		(uint32_t)movie->video->height, (uint32_t)frame_count, 0, 0};
	movie->pixels = malloc((size_t)movie->info.width * movie->info.height * 4);
	movie->video_frame = av_frame_alloc(); movie->audio_frame = av_frame_alloc();
	movie->packet = av_packet_alloc();
	if (!movie->pixels || !movie->video_frame || !movie->audio_frame || !movie->packet) goto fail;
	movie->audio_index = av_find_best_stream(movie->format, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
	if (movie->audio_index >= 0)
	{
		movie->audio = open_codec(movie->format->streams[movie->audio_index]);
		if (!movie->audio) goto fail;
		AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
		if (swr_alloc_set_opts2(&movie->resample, &stereo, AV_SAMPLE_FMT_FLT, 48000,
			&movie->audio->ch_layout, movie->audio->sample_fmt, movie->audio->sample_rate, 0, NULL) < 0 ||
			swr_init(movie->resample) < 0) goto fail;
		if (audio_output)
		{
			SDL_AudioSpec spec = {SDL_AUDIO_F32, 2, 48000};
			movie->output = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
			if (!movie->output) { fprintf(stderr,"[bink] audio device failed: %s\n", SDL_GetError()); goto fail; }
		}
	}
	/* Decode the soundtrack independently of rendering. A slow first window
	 * frame must never starve audio or leave delayed PCM behind the movie. */
	if (movie->audio && !preload_audio(movie, path)) goto fail;
	return movie;
fail:
	mac_bink_close(movie);
	return NULL;
}

static int submit_pcm(struct halo_bink *movie, float *pcm, int frames)
{
	for (int i = 0; i < frames * 2; ++i)
	{
		if (!isfinite(pcm[i])) return 0;
		++movie->evidence.pcm_samples;
		movie->evidence.nonzero_pcm_samples += pcm[i] != 0;
		double amplitude = fabs((double)pcm[i]);
		if (amplitude > movie->evidence.pcm_peak) movie->evidence.pcm_peak = amplitude;
	}
	return !movie->output || SDL_PutAudioStreamData(movie->output, pcm, frames * 2 * (int)sizeof(float));
}

static int decode_audio(struct halo_bink *movie)
{
	while (avcodec_receive_frame(movie->audio, movie->audio_frame) == 0)
	{
		int capacity = swr_get_out_samples(movie->resample, movie->audio_frame->nb_samples);
		if (capacity <= 0 || capacity > 1048576) return 0;
		float *pcm = malloc((size_t)capacity * 2 * sizeof(float));
		if (!pcm) return 0;
		unsigned char *output = (unsigned char *)pcm;
		int frames = swr_convert(movie->resample, &output, capacity,
			(const unsigned char **)movie->audio_frame->extended_data, movie->audio_frame->nb_samples);
		if (frames < 0) { free(pcm); return 0; }
		int success = submit_pcm(movie, pcm, frames);
		free(pcm); av_frame_unref(movie->audio_frame);
		if (!success) return 0;
	}
	return 1;
}

static int preload_audio(struct halo_bink *movie, const char *path)
{
	AVFormatContext *audio_file = NULL;
	if (avformat_open_input(&audio_file, path, NULL, NULL) < 0) return 0;
	int ok = 1;
	while (av_read_frame(audio_file, movie->packet) >= 0)
	{
		if (movie->packet->stream_index == movie->audio_index)
			ok = avcodec_send_packet(movie->audio, movie->packet) >= 0 && decode_audio(movie);
		av_packet_unref(movie->packet);
		if (!ok) break;
	}
	avformat_close_input(&audio_file);
	if (!ok) return 0;
	int result = avcodec_send_packet(movie->audio, NULL);
	if ((result < 0 && result != AVERROR_EOF) || !decode_audio(movie)) return 0;
	for (;;)
	{
		float pcm[2048]; unsigned char *output = (unsigned char *)pcm;
		int frames = swr_convert(movie->resample, &output, 1024, NULL, 0);
		if (frames < 0 || !submit_pcm(movie, pcm, frames)) return 0;
		if (!frames) break;
	}
	if (movie->output && !SDL_FlushAudioStream(movie->output)) return 0;
	movie->audio_preloaded = movie->evidence.audio_drained = 1;
	audio_snapshot(movie, "preloaded");
	return 1;
}

static void audio_snapshot(struct halo_bink *movie, const char *phase)
{
	if (!movie->output) return;
	int queued = SDL_GetAudioStreamQueued(movie->output);
	int available = SDL_GetAudioStreamAvailable(movie->output);
	SDL_AudioDeviceID device = SDL_GetAudioStreamDevice(movie->output);
	SDL_AudioSpec src = {0}, dst = {0};
	SDL_GetAudioStreamFormat(movie->output, &src, &dst);
	uint64_t submitted = movie->evidence.pcm_samples * sizeof(float);
	/* This stream never changes input format. Consumption means delivery to
	 * the device mixer, with final hardware buffers accounted for on close. */
	uint64_t consumed = queued >= 0 && submitted >= (uint64_t)queued ? submitted - (uint64_t)queued : 0;
	fprintf(stderr,"[bink-audio] phase=%s frame=%llu elapsed_ms=%.3f queued=%d available=%d consumed_samples=%llu submitted_samples=%llu device=%u paused=%d input=%x/%d/%d output=%x/%d/%d\n",
		phase,(unsigned long long)movie->evidence.video_frames,
		movie->start_ns ? (double)(SDL_GetTicksNS()-movie->start_ns)/1e6 : 0,
		queued,available,(unsigned long long)(consumed/sizeof(float)),
		(unsigned long long)movie->evidence.pcm_samples,device,SDL_AudioDevicePaused(device),
		src.format,src.channels,src.freq,dst.format,dst.channels,dst.freq);
}

static int finish_audio(struct halo_bink *movie)
{
	if (movie->evidence.audio_drained) return 1;
	while (av_read_frame(movie->format, movie->packet) >= 0)
	{
		int ok = 1;
		if (movie->packet->stream_index == movie->audio_index)
			ok = avcodec_send_packet(movie->audio, movie->packet) >= 0 && decode_audio(movie);
		av_packet_unref(movie->packet);
		if (!ok) return 0;
	}
	if (movie->audio)
	{
		int result = avcodec_send_packet(movie->audio, NULL);
		if (result < 0 && result != AVERROR_EOF) return 0;
		if (!decode_audio(movie)) return 0;
		for (;;)
		{
			float pcm[2048];
			unsigned char *output = (unsigned char *)pcm;
			int frames = swr_convert(movie->resample, &output, 1024, NULL, 0);
			if (frames < 0 || !submit_pcm(movie, pcm, frames)) return 0;
			if (!frames) break;
		}
	}
	if (movie->output && !SDL_FlushAudioStream(movie->output)) return 0;
	movie->evidence.audio_drained = 1;
	return 1;
}

int mac_bink_decode(struct halo_bink *movie)
{
	if (!movie) return 0;
	for (;;)
	{
		int result = avcodec_receive_frame(movie->video, movie->video_frame);
		if (result == 0)
		{
			movie->scale = sws_getCachedContext(movie->scale, movie->video->width, movie->video->height,
				movie->video_frame->format, movie->video->width, movie->video->height,
				AV_PIX_FMT_BGRA, SWS_BILINEAR, NULL, NULL, NULL);
			if (!movie->scale) return 0;
			unsigned char *destination[4] = {movie->pixels, NULL, NULL, NULL};
			int pitches[4] = {(int)movie->info.width * 4, 0, 0, 0};
			if (sws_scale(movie->scale, (const unsigned char *const *)movie->video_frame->data,
				movie->video_frame->linesize, 0, movie->video->height, destination, pitches) <= 0) return 0;
			av_frame_unref(movie->video_frame); ++movie->evidence.video_frames;
			if (!movie->start_ns) movie->start_ns = SDL_GetTicksNS();
			if (movie->evidence.video_frames % 60 == 0) audio_snapshot(movie, "playing");
			return 1;
		}
		if (result != AVERROR(EAGAIN) || av_read_frame(movie->format, movie->packet) < 0) return 0;
		int ok = 1;
		if (movie->packet->stream_index == movie->video_index)
			ok = avcodec_send_packet(movie->video, movie->packet) >= 0;
		else if (!movie->audio_preloaded && movie->packet->stream_index == movie->audio_index)
			ok = avcodec_send_packet(movie->audio, movie->packet) >= 0 && decode_audio(movie);
		av_packet_unref(movie->packet);
		if (!ok) return 0;
	}
}

void mac_bink_next(struct halo_bink *movie)
{
	if (!movie) return;
	movie->info.last_frame_num = movie->info.frame_num;
	/* The game advances before it copies/draws and stops at Frames-1.
	 * FrameNum names the decoded zero-based frame, not the next frame. */
	if (movie->evidence.video_frames)
		movie->info.frame_num = (uint32_t)(movie->evidence.video_frames - 1);
	if (movie->evidence.video_frames == movie->info.frames && !finish_audio(movie))
		movie->tail_error = 1;
}
int mac_bink_wait(struct halo_bink *movie)
{
	if (!movie || !movie->start_ns) return 0;
	/* Draw frame zero immediately; subsequent swaps land at file PTS. The
	 * final frame receives its own full duration in natural close below. */
	uint64_t target = movie->start_ns + (uint64_t)av_rescale((int64_t)movie->evidence.video_frames - 1,
		INT64_C(1000000000) * movie->rate.den, movie->rate.num);
	uint64_t now = SDL_GetTicksNS();
	if (now >= target) return 0;
	/* The guest's unthrottled BinkWait loop polls; yield instead of burning CPU. */
	if (target - now > 1000000) SDL_Delay(1);
	return 1;
}
const struct halo_bink_info *mac_bink_info(const struct halo_bink *movie) { return &movie->info; }
const unsigned char *mac_bink_pixels(const struct halo_bink *movie) { return movie->pixels; }
const struct halo_bink_evidence *mac_bink_evidence(const struct halo_bink *movie) { return &movie->evidence; }
int mac_bink_start_playback(struct halo_bink *movie)
{
	if (!movie) return 0;
	/* The first displayed frame defines PTS zero for both soundtrack and video. */
	movie->start_ns = SDL_GetTicksNS();
	if (movie->output && !SDL_ResumeAudioStreamDevice(movie->output))
	{ fprintf(stderr,"[bink] audio resume failed: %s\n",SDL_GetError()); movie->tail_error = 1; return 0; }
	audio_snapshot(movie, "started");
	return 1;
}

int mac_bink_audio_pending(const struct halo_bink *movie)
{
	if (!movie || !movie->output) return 0;
	int queued = SDL_GetAudioStreamQueued(movie->output);
	int available = SDL_GetAudioStreamAvailable(movie->output);
	return queued < 0 || available < 0 ? -1 : queued > available ? queued : available;
}
void mac_bink_close(struct halo_bink *movie)
{
	if (!movie) return;
	if (movie->output) SDL_DestroyAudioStream(movie->output);
	swr_free(&movie->resample); sws_freeContext(movie->scale);
	avcodec_free_context(&movie->video); avcodec_free_context(&movie->audio);
	av_frame_free(&movie->video_frame); av_frame_free(&movie->audio_frame);
	av_packet_free(&movie->packet); avformat_close_input(&movie->format);
	free(movie->pixels); free(movie);
}

static struct halo_bink *movies[8]; /* GL-owning guest thread only. */
uint32_t mac_host_bink_open(uint32_t path_va, uint32_t info_va, int audio_enabled)
{
	const char *path = mac_guest_address_resolve(path_va, 1);
	size_t available = mac_guest_address_available(path_va);
	size_t limit = available < 4096 ? available : 4096;
	if (!path || !limit || strnlen(path, limit) == limit ||
		!mac_guest_address_resolve(info_va, sizeof(struct halo_bink_info))) return 0;
	for (uint32_t i = 1; i < 8; ++i) if (!movies[i])
	{
		const char *enabled = getenv("HALO_AUDIO_ENABLE");
		movies[i] = mac_bink_open_path(path, audio_enabled && enabled && strcmp(enabled, "1") == 0);
		if (!movies[i]) return 0;
		if (mac_guest_write(info_va, &movies[i]->info, sizeof(movies[i]->info)) != 0) { mac_host_bink_close(i); return 0; }
		fprintf(stderr,"[bink] opened native video %ux%u frames=%u audio=%d\n",movies[i]->info.width,movies[i]->info.height,movies[i]->info.frames,movies[i]->output != NULL);
		return i;
	}
	return 0;
}
int mac_host_bink_decode(uint32_t token) { return token < 8 && mac_bink_decode(movies[token]); }
int mac_host_bink_next(uint32_t token, uint32_t info_va)
{
	if (token >= 8 || !movies[token] || !mac_guest_address_resolve(info_va, sizeof(struct halo_bink_info))) return 0;
	mac_bink_next(movies[token]); return mac_guest_write(info_va, &movies[token]->info, sizeof(movies[token]->info)) == 0;
}
int mac_host_bink_wait(uint32_t token) { return token < 8 ? mac_bink_wait(movies[token]) : 0; }
int mac_host_bink_copy(uint32_t token, uint32_t destination_va, int32_t pitch,
	uint32_t height, uint32_t x, uint32_t y, uint32_t flags)
{
	struct halo_bink *movie = token < 8 ? movies[token] : NULL;
	if (!movie || !movie->evidence.video_frames || (flags & 0xff) != 3 || pitch <= 0 ||
		x > UINT32_MAX / 4 || (uint64_t)(x + (uint64_t)movie->info.width) * 4 > (uint32_t)pitch ||
		y > height || movie->info.height > height - y) return 0;
	uint64_t offset = (uint64_t)y * (uint32_t)pitch + (uint64_t)x * 4;
	uint64_t size = (uint64_t)(movie->info.height - 1) * (uint32_t)pitch + (uint64_t)movie->info.width * 4;
	if ((uint64_t)destination_va + offset > UINT32_MAX || size > SIZE_MAX) return 0;
	unsigned char *destination = mac_guest_address_resolve((uint32_t)(destination_va + offset), (size_t)size);
	if (!destination) return 0;
	for (uint32_t row = 0; row < movie->info.height; ++row)
		memcpy(destination + (size_t)row * (uint32_t)pitch, movie->pixels + (size_t)row * movie->info.width * 4, (size_t)movie->info.width * 4);
	if (movie->copied_decode_count != movie->evidence.video_frames)
	{
		movie->copied_decode_count = movie->evidence.video_frames;
		++movie->evidence.copied_frames;
	}
	return 1;
}
static int compare_gap(const void *a, const void *b)
{
	uint64_t first = *(const uint64_t *)a, second = *(const uint64_t *)b;
	return (first > second) - (first < second);
}

static void report_cadence(struct halo_bink *movie)
{
	size_t count = movie->gap_count < 1024 ? (size_t)movie->gap_count : 1024;
	qsort(movie->presentation_gaps, count, sizeof(movie->presentation_gaps[0]), compare_gap);
	double median_ms = count ? (double)movie->presentation_gaps[count/2]/1e6 : 0;
	if (count && !(count & 1)) median_ms = ((double)movie->presentation_gaps[count/2-1] + movie->presentation_gaps[count/2])/2e6;
	double first_error_ms = movie->first_present_ns && movie->start_ns ?
		(double)((int64_t)movie->first_present_ns - (int64_t)movie->start_ns)/1e6 : 0;
	double final_error_ms = movie->last_present_ns && movie->start_ns ?
		(double)((int64_t)movie->last_present_ns - (int64_t)movie->start_ns)/1e6 -
		(double)(movie->evidence.video_frames - 1) * movie->rate.den * 1000.0 / movie->rate.num : 0;
	fprintf(stderr,"[bink-cadence] presented=%llu gaps=%llu sampled_gaps=%zu min_ms=%.3f median_ms=%.3f max_ms=%.3f gaps_over_50ms=%llu first_pts_error_ms=%.3f final_pts_error_ms=%.3f\n",
		(unsigned long long)movie->evidence.presented_frames,(unsigned long long)movie->gap_count,count,
		(double)movie->gap_min_ns/1e6,median_ms,(double)movie->gap_max_ns/1e6,
		(unsigned long long)movie->long_gaps,first_error_ms,final_error_ms);
}

static void close_native_movie(uint32_t token, int natural)
{
	if (token >= 8 || !movies[token]) return;
	struct halo_bink *movie = movies[token];
	int queued = 0, available = 0, timeout = 0;
	if (natural && movie->output)
	{
		audio_snapshot(movie, "final-presented");
		uint64_t deadline = SDL_GetTicks() + 2000;
		for (;;)
		{
			queued = SDL_GetAudioStreamQueued(movie->output);
			available = SDL_GetAudioStreamAvailable(movie->output);
			if (queued == 0 && available == 0) break;
			if (queued < 0 || available < 0) { movie->tail_error = 1; break; }
			if (SDL_GetTicks() >= deadline) { timeout = 1; break; }
			SDL_Delay(1);
		}
		/* The queue reaches zero after delivery to the audio device. Allow
		 * its final buffer to play before destroying the stream. */
		SDL_AudioSpec spec; int frames = 0;
		if (!timeout && SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(movie->output), &spec, &frames) && spec.freq > 0)
			SDL_Delay((uint32_t)((uint64_t)frames * 2000 / (uint32_t)spec.freq + 1));
	}
	if (natural && movie->last_present_ns)
	{
		uint64_t until = movie->last_present_ns + (uint64_t)av_rescale(1,
			INT64_C(1000000000) * movie->rate.den, movie->rate.num);
		while (SDL_GetTicksNS() < until) SDL_Delay(1);
	}
	if (natural) audio_snapshot(movie, "closed");
	report_cadence(movie);
	fprintf(stderr,"[bink] closed frames=%llu copied=%llu presented=%llu expected=%u duration_ms=%.3f elapsed_ms=%.3f presentation_ms=%.3f audio_drained=%d queued=%d available=%d timeout=%d tail_error=%d natural=%d pcm_samples=%llu nonzero_samples=%llu peak=%.6f\n",
		(unsigned long long)movies[token]->evidence.video_frames,(unsigned long long)movies[token]->evidence.copied_frames,
		(unsigned long long)movie->evidence.presented_frames, movie->info.frames,
		(double)movie->info.frames * movie->rate.den * 1000.0 / movie->rate.num,
		movie->start_ns ? (double)(SDL_GetTicksNS()-movie->start_ns)/1e6 : 0,
		movie->first_present_ns ? (double)(movie->last_present_ns-movie->first_present_ns)/1e6 : 0,
		movie->evidence.audio_drained, queued, available, timeout, movie->tail_error, natural,
		(unsigned long long)movie->evidence.pcm_samples, (unsigned long long)movie->evidence.nonzero_pcm_samples, movie->evidence.pcm_peak);
	mac_bink_close(movies[token]); movies[token] = NULL;
}

void mac_host_bink_close(uint32_t token)
{
	if (token >= 8 || !movies[token]) return;
	if (movies[token]->evidence.video_frames == movies[token]->info.frames &&
		movies[token]->copied_decode_count > movies[token]->presented_decode_count)
	{
		/* Guest BinkClose runs inside its draw, before the final host swap. */
		movies[token]->pending_close = 1;
		return;
	}
	close_native_movie(token, 0);
}

void mac_host_bink_presented(void)
{
	for (uint32_t token = 1; token < 8; ++token)
	{
		struct halo_bink *movie = movies[token];
		if (!movie) continue;
		if (movie->copied_decode_count > movie->presented_decode_count)
		{
			movie->presented_decode_count = movie->copied_decode_count;
			++movie->evidence.presented_frames;
			uint64_t presented_ns = SDL_GetTicksNS();
			if (movie->last_present_ns)
			{
				uint64_t gap = presented_ns - movie->last_present_ns;
				if (movie->gap_count < 1024) movie->presentation_gaps[movie->gap_count] = gap;
				if (!movie->gap_count || gap < movie->gap_min_ns) movie->gap_min_ns = gap;
				if (gap > movie->gap_max_ns) movie->gap_max_ns = gap;
				movie->long_gaps += gap > UINT64_C(50000000);
				++movie->gap_count;
			}
			movie->last_present_ns = presented_ns;
			if (!movie->first_present_ns)
			{
				movie->first_present_ns = movie->last_present_ns;
				mac_bink_start_playback(movie);
			}
			if (getenv("HALO_BINK_FRAME_TRACE"))
				fprintf(stderr,"[bink-frame] token=%u frame=%llu expected=%u pts_ms=%.3f elapsed_ms=%.3f\n", token,
					(unsigned long long)movie->presented_decode_count, movie->info.frames,
					(double)(movie->presented_decode_count - 1) * movie->rate.den * 1000.0 / movie->rate.num,
					(double)(SDL_GetTicksNS()-movie->start_ns)/1e6);
		}
		if (movie->pending_close) close_native_movie(token, 1);
	}
}
