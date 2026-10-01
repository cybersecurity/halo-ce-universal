/*
MACOS_BINK.C

Bink video playback implementation for the macOS port using libavformat,
libavcodec, libswscale, and SDL3 audio.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/avutil.h>
#include <SDL3/SDL.h>

void platform_translate_path(const char *xbox_path, char *host_path, unsigned int host_path_size);
void platform_log(const char *format, ...) __attribute__((format(printf, 1, 2)));

#ifndef __stdcall
#define __stdcall
#endif

typedef void *(*rad_memory_allocate_proc)(unsigned int size);
typedef void (*rad_memory_free_proc)(void *memory);
typedef void *(*bink_sound_system_open_proc)(unsigned int param);

typedef struct BINK
{
	/* Public fields accessed directly by bink_playback.c */
	uint32_t Width;
	uint32_t Height;
	uint32_t Frames;
	uint32_t FrameNum;
	uint32_t LastFrameNum;

	/* Internal decoder context */
	AVFormatContext *format_ctx;
	AVCodecContext *video_codec_ctx;
	AVCodecContext *audio_codec_ctx;
	struct SwsContext *sws_ctx;
	AVFrame *video_frame;
	AVFrame *audio_frame;
	AVPacket *packet;

	int video_stream_idx;
	int audio_stream_idx;

	SDL_AudioStream *audio_stream;

	double fps;
	uint64_t start_ticks;
	int eof;
	int frame_ready;
} *HBINK;

typedef struct BINKSUMMARY
{
	uint32_t Width;
	uint32_t Height;
	uint32_t TotalTime;
	uint32_t FileFrameRate;
	uint32_t FileFrameRateDiv;
	uint32_t FrameRate;
	uint32_t FrameRateDiv;
	uint32_t TotalOpenTime;
	uint32_t TotalFrames;
	uint32_t TotalPlayedFrames;
	uint32_t SkippedFrames;
	uint32_t SkippedBlits;
	uint32_t SoundSkips;
	uint32_t TotalBlitTime;
	uint32_t TotalReadTime;
	uint32_t TotalVideoDecompTime;
	uint32_t TotalAudioDecompTime;
	uint32_t TotalIdleReadTime;
	uint32_t TotalBackReadTime;
	uint32_t TotalReadSpeed;
	uint32_t SlowestFrameTime;
	uint32_t Slowest2FrameTime;
	uint32_t SlowestFrameNum;
	uint32_t Slowest2FrameNum;
	uint32_t AverageDataRate;
	uint32_t AverageFrameSize;
	uint32_t HighestMemAmount;
	uint32_t TotalIOMemory;
	uint32_t HighestIOUsed;
	uint32_t Highest1SecRate;
	uint32_t Highest1SecFrame;
} BINKSUMMARY;

typedef struct BINKREALTIME
{
	uint32_t FrameNum;
	uint32_t FrameRate;
	uint32_t FrameRateDiv;
	uint32_t Frames;
	uint32_t FramesTime;
	uint32_t FramesVideoDecompTime;
	uint32_t FramesAudioDecompTime;
	uint32_t FramesReadTime;
	uint32_t FramesIdleReadTime;
	uint32_t FramesThreadReadTime;
	uint32_t FramesBlitTime;
	uint32_t ReadBufferSize;
	uint32_t ReadBufferUsed;
	uint32_t FramesDataRate;
} BINKREALTIME;

void __stdcall RADSetMemory(rad_memory_allocate_proc allocate, rad_memory_free_proc release)
{
	(void)allocate;
	(void)release;
}

void *__stdcall BinkOpenDirectSound(unsigned int param)
{
	(void)param;
	return (void *)1;
}

int __stdcall BinkSetSoundSystem(bink_sound_system_open_proc open, unsigned int param)
{
	(void)open;
	(void)param;
	return 1;
}

void __stdcall BinkSetIOSize(unsigned int io_size)
{
	(void)io_size;
}

HBINK __stdcall BinkOpen(const char *name, unsigned int flags)
{
	(void)flags;
	char host_path[1024];

	if (!name || !name[0])
		return NULL;

	platform_translate_path(name, host_path, sizeof(host_path));

	/* Fallback: if translated path doesn't exist, search directly in assets/bink/ */
	if (access(host_path, F_OK) != 0)
	{
		const char *base = strrchr(name, '\\');
		if (!base)
			base = strrchr(name, '/');
		base = base ? base + 1 : name;
		snprintf(host_path, sizeof(host_path), "assets/bink/%s", base);
	}

	AVFormatContext *fmt_ctx = NULL;
	if (avformat_open_input(&fmt_ctx, host_path, NULL, NULL) < 0)
	{
		platform_log("Bink: failed to open '%s'", host_path);
		return NULL;
	}

	if (avformat_find_stream_info(fmt_ctx, NULL) < 0)
	{
		platform_log("Bink: failed to find stream info in '%s'", host_path);
		avformat_close_input(&fmt_ctx);
		return NULL;
	}

	int video_idx = -1;
	int audio_idx = -1;
	for (unsigned int i = 0; i < fmt_ctx->nb_streams; i++)
	{
		if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && video_idx < 0)
			video_idx = (int)i;
		else if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && audio_idx < 0)
			audio_idx = (int)i;
	}

	if (video_idx < 0)
	{
		platform_log("Bink: no video stream in '%s'", host_path);
		avformat_close_input(&fmt_ctx);
		return NULL;
	}

	AVStream *vstream = fmt_ctx->streams[video_idx];
	const AVCodec *vcodec = avcodec_find_decoder(vstream->codecpar->codec_id);
	if (!vcodec)
	{
		platform_log("Bink: no decoder found for video codec %d", vstream->codecpar->codec_id);
		avformat_close_input(&fmt_ctx);
		return NULL;
	}

	AVCodecContext *vctx = avcodec_alloc_context3(vcodec);
	if (!vctx || avcodec_parameters_to_context(vctx, vstream->codecpar) < 0 || avcodec_open2(vctx, vcodec, NULL) < 0)
	{
		platform_log("Bink: failed to open video codec for '%s'", host_path);
		if (vctx)
			avcodec_free_context(&vctx);
		avformat_close_input(&fmt_ctx);
		return NULL;
	}

	struct BINK *bink = calloc(1, sizeof(struct BINK));
	if (!bink)
	{
		avcodec_free_context(&vctx);
		avformat_close_input(&fmt_ctx);
		return NULL;
	}

	bink->format_ctx = fmt_ctx;
	bink->video_codec_ctx = vctx;
	bink->video_stream_idx = video_idx;
	bink->audio_stream_idx = audio_idx;

	bink->Width = (uint32_t)vctx->width;
	bink->Height = (uint32_t)vctx->height;
	bink->FrameNum = 0;
	bink->LastFrameNum = 0;

	if (vstream->avg_frame_rate.den > 0 && vstream->avg_frame_rate.num > 0)
		bink->fps = (double)vstream->avg_frame_rate.num / (double)vstream->avg_frame_rate.den;
	else
		bink->fps = 29.97;

	if (vstream->nb_frames > 0)
		bink->Frames = (uint32_t)vstream->nb_frames;
	else if (fmt_ctx->duration > 0)
		bink->Frames = (uint32_t)((fmt_ctx->duration * bink->fps) / AV_TIME_BASE);
	else
		bink->Frames = 1000;

	/* Bink header offset 0x08 has exact frame count */
	FILE *bf = fopen(host_path, "rb");
	if (bf)
	{
		uint32_t hdr[6];
		if (fread(hdr, sizeof(uint32_t), 6, bf) == 6 && hdr[2] > 0)
			bink->Frames = hdr[2];
		fclose(bf);
	}

	/* Color conversion from Bink YUV420P to Xbox linear X8R8G8B8 (BGRA with full alpha) */
	bink->sws_ctx = sws_getContext(
		vctx->width, vctx->height, vctx->pix_fmt,
		vctx->width, vctx->height, AV_PIX_FMT_BGRA,
		SWS_FAST_BILINEAR, NULL, NULL, NULL);

	/* Initialize audio decoder & SDL3 audio playback if audio stream is present */
	if (audio_idx >= 0)
	{
		AVStream *astream = fmt_ctx->streams[audio_idx];
		const AVCodec *acodec = avcodec_find_decoder(astream->codecpar->codec_id);
		if (acodec)
		{
			AVCodecContext *actx = avcodec_alloc_context3(acodec);
			if (actx && avcodec_parameters_to_context(actx, astream->codecpar) == 0 && avcodec_open2(actx, acodec, NULL) == 0)
			{
				bink->audio_codec_ctx = actx;
				SDL_AudioSpec spec;
				spec.format = SDL_AUDIO_F32;
				spec.channels = actx->ch_layout.nb_channels > 0 ? actx->ch_layout.nb_channels : 2;
				spec.freq = actx->sample_rate > 0 ? actx->sample_rate : 44100;
				bink->audio_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
				if (bink->audio_stream)
				{
					SDL_ResumeAudioStreamDevice(bink->audio_stream);
				}
			}
			else if (actx)
			{
				avcodec_free_context(&actx);
			}
		}
	}

	bink->video_frame = av_frame_alloc();
	bink->audio_frame = av_frame_alloc();
	bink->packet = av_packet_alloc();
	bink->start_ticks = 0;

	platform_log("Bink: opened '%s' (%ux%u, %u frames, %.2f fps, audio: %s)",
		host_path, bink->Width, bink->Height, bink->Frames, bink->fps,
		bink->audio_stream ? "yes" : "no");

	return bink;
}

void __stdcall BinkClose(HBINK bink)
{
	if (!bink)
		return;

	platform_log("Bink: closed movie");

	if (bink->audio_stream)
	{
		SDL_DestroyAudioStream(bink->audio_stream);
		bink->audio_stream = NULL;
	}
	if (bink->sws_ctx)
	{
		sws_freeContext(bink->sws_ctx);
		bink->sws_ctx = NULL;
	}
	if (bink->video_frame)
		av_frame_free(&bink->video_frame);
	if (bink->audio_frame)
		av_frame_free(&bink->audio_frame);
	if (bink->packet)
		av_packet_free(&bink->packet);
	if (bink->video_codec_ctx)
		avcodec_free_context(&bink->video_codec_ctx);
	if (bink->audio_codec_ctx)
		avcodec_free_context(&bink->audio_codec_ctx);
	if (bink->format_ctx)
		avformat_close_input(&bink->format_ctx);

	free(bink);
}

/* the frame's samples to SDL as interleaved floats (Bink audio decodes to
float, packed or, depending on the FFmpeg version, one plane per channel) */
static void queue_audio(HBINK bink, const AVFrame *frame)
{
	static float *interleaved;
	static size_t interleaved_size;
	static int reported;
	int samples = frame->nb_samples;
	int channels = bink->audio_codec_ctx->ch_layout.nb_channels;
	size_t bytes = (size_t)samples * (size_t)channels * sizeof(float);

	if (frame->format == AV_SAMPLE_FMT_FLT)
	{
		SDL_PutAudioStreamData(bink->audio_stream, frame->data[0], (int)bytes);
	}
	else if (frame->format == AV_SAMPLE_FMT_FLTP)
	{
		int sample, channel;

		if (interleaved_size < bytes)
		{
			float *grown = realloc(interleaved, bytes);

			if (!grown)
				return;
			interleaved = grown;
			interleaved_size = bytes;
		}
		for (sample = 0; sample < samples; sample++)
		{
			for (channel = 0; channel < channels; channel++)
				interleaved[sample * channels + channel] = ((const float *)frame->data[channel])[sample];
		}
		SDL_PutAudioStreamData(bink->audio_stream, interleaved, (int)bytes);
	}
	else if (!reported)
	{
		reported = 1;
		platform_log("Bink: audio in sample format %d is not played", frame->format);
	}
}

int __stdcall BinkDoFrame(HBINK bink)
{
	if (!bink || !bink->format_ctx || !bink->video_codec_ctx)
		return 0;

	bink->frame_ready = 0;

	/* Loop handling if reached end of movie */
	if (bink->FrameNum >= bink->Frames)
	{
		bink->FrameNum = 0;
		bink->LastFrameNum = 0;
		bink->start_ticks = SDL_GetTicks();
		bink->eof = 0;
		av_seek_frame(bink->format_ctx, -1, 0, AVSEEK_FLAG_BACKWARD);
		if (bink->video_codec_ctx)
			avcodec_flush_buffers(bink->video_codec_ctx);
		if (bink->audio_codec_ctx)
			avcodec_flush_buffers(bink->audio_codec_ctx);
		if (bink->audio_stream)
			SDL_ClearAudioStream(bink->audio_stream);
	}

	while (!bink->frame_ready && !bink->eof)
	{
		if (avcodec_receive_frame(bink->video_codec_ctx, bink->video_frame) == 0)
		{
			bink->frame_ready = 1;
			break;
		}

		int ret = av_read_frame(bink->format_ctx, bink->packet);
		if (ret < 0)
		{
			avcodec_send_packet(bink->video_codec_ctx, NULL);
			if (avcodec_receive_frame(bink->video_codec_ctx, bink->video_frame) == 0)
				bink->frame_ready = 1;
			else
				bink->eof = 1;
			break;
		}

		if (bink->packet->stream_index == bink->video_stream_idx)
		{
			if (avcodec_send_packet(bink->video_codec_ctx, bink->packet) == 0)
			{
				if (avcodec_receive_frame(bink->video_codec_ctx, bink->video_frame) == 0)
					bink->frame_ready = 1;
			}
		}
		else if (bink->packet->stream_index == bink->audio_stream_idx && bink->audio_codec_ctx && bink->audio_stream)
		{
			if (avcodec_send_packet(bink->audio_codec_ctx, bink->packet) == 0)
			{
				while (avcodec_receive_frame(bink->audio_codec_ctx, bink->audio_frame) == 0)
					queue_audio(bink, bink->audio_frame);
			}
		}

		av_packet_unref(bink->packet);
	}

	return 0;
}

void __stdcall BinkNextFrame(HBINK bink)
{
	if (!bink)
		return;

	bink->LastFrameNum = bink->FrameNum;
	bink->FrameNum++;
}

int __stdcall BinkWait(HBINK bink)
{
	if (!bink)
		return 0;

	uint64_t now = SDL_GetTicks();
	if (bink->start_ticks == 0)
	{
		bink->start_ticks = now;
		return 0;
	}

	uint64_t target_ticks = bink->start_ticks + (uint64_t)((double)bink->FrameNum * 1000.0 / bink->fps);
	if (now < target_ticks)
	{
		uint64_t wait_ms = target_ticks - now;
		if (wait_ms > 1)
			SDL_Delay(1);
		return 1;
	}
	else if (now > target_ticks + 500)
	{
		/* Re-anchor if the frame rate stalled */
		bink->start_ticks = now - (uint64_t)((double)bink->FrameNum * 1000.0 / bink->fps);
	}

	return 0;
}

int __stdcall BinkCopyToBuffer(HBINK bink, void *destination, int destination_pitch,
	unsigned int destination_height, unsigned int destination_x, unsigned int destination_y,
	unsigned int flags)
{
	(void)flags;
	(void)destination_height;

	if (!bink || !bink->sws_ctx || !destination || !bink->frame_ready)
		return 0;

	uint8_t *dst_data[4] = {
		(uint8_t *)destination + (size_t)destination_y * destination_pitch + destination_x * 4,
		NULL, NULL, NULL
	};
	int dst_linesize[4] = { destination_pitch, 0, 0, 0 };

	sws_scale(
		bink->sws_ctx,
		(const uint8_t * const *)bink->video_frame->data,
		bink->video_frame->linesize,
		0,
		bink->Height,
		dst_data,
		dst_linesize);

	return 0;
}

void __stdcall BinkGetSummary(HBINK bink, void *summary)
{
	if (!summary)
		return;

	BINKSUMMARY *s = (BINKSUMMARY *)summary;
	memset(s, 0, sizeof(*s));
	if (bink)
	{
		s->Width = bink->Width;
		s->Height = bink->Height;
		s->TotalFrames = bink->Frames;
		s->TotalPlayedFrames = bink->FrameNum;
		s->FrameRate = (uint32_t)(bink->fps * 100.0);
		s->FrameRateDiv = 100;
	}
}

void __stdcall BinkGetRealtime(HBINK bink, void *realtime, unsigned int frame_count)
{
	(void)frame_count;
	if (!realtime)
		return;

	BINKREALTIME *r = (BINKREALTIME *)realtime;
	memset(r, 0, sizeof(*r));
	if (bink)
	{
		r->FrameNum = bink->FrameNum;
		r->Frames = bink->FrameNum > 0 ? bink->FrameNum : 1;
		r->FrameRate = (uint32_t)(bink->fps * 100.0);
		r->FrameRateDiv = 100;
	}
	else
	{
		r->Frames = 1;
	}
}
