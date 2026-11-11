// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include <chiaki/config.h>
#if CHIAKI_LIB_ENABLE_OPUS

#include <chiaki/opusdecoder.h>

#include <opus/opus.h>

#include <string.h>

static void chiaki_opus_decoder_header(ChiakiAudioHeader *header, void *user);
static void chiaki_opus_decoder_frame(uint8_t *buf, size_t buf_size, void *user);

CHIAKI_EXPORT void chiaki_opus_decoder_init(ChiakiOpusDecoder *decoder, ChiakiLog *log)
{
	decoder->log = log;
	decoder->opus_decoder = NULL;
	memset(&decoder->audio_header, 0, sizeof(decoder->audio_header));

	decoder->pcm_buf = NULL;
	decoder->pcm_buf_size = 0;

	decoder->cb_user = NULL;
	decoder->settings_cb = NULL;
	decoder->frame_cb = NULL;
}

CHIAKI_EXPORT void chiaki_opus_decoder_fini(ChiakiOpusDecoder *decoder)
{
	free(decoder->pcm_buf);
	if(decoder->opus_decoder)
		opus_decoder_destroy(decoder->opus_decoder);
}

CHIAKI_EXPORT void chiaki_opus_decoder_get_sink(ChiakiOpusDecoder *decoder, ChiakiAudioSink *sink)
{
	sink->user = decoder;
	sink->header_cb = chiaki_opus_decoder_header;
	sink->frame_cb = chiaki_opus_decoder_frame;
}

static void chiaki_opus_decoder_header(ChiakiAudioHeader *header, void *user)
{
	ChiakiOpusDecoder *decoder = user;
	memcpy(&decoder->audio_header, header, sizeof(decoder->audio_header));

	opus_decoder_destroy(decoder->opus_decoder);

	int error;
	decoder->opus_decoder = opus_decoder_create(header->rate, header->channels, &error);

	if(error != OPUS_OK)
	{
		CHIAKI_LOGE(decoder->log, "ChiakiOpusDecoder failed to initialize opus decoder: %s", opus_strerror(error));
		decoder->opus_decoder = NULL;
		return;
	}

	CHIAKI_LOGI(decoder->log, "ChiakiOpusDecoder initialized");

	// The header frame is 10 ms, but a cloud unit can carry two of those.
	// opus_decode rejects the packet with "buffer too small" if the PCM cap is 480.
	int cap_samples = 5760;
	if(header->frame_size > (uint32_t)cap_samples)
		cap_samples = (int)header->frame_size;
	int cap_channels = header->channels ? header->channels : 2;
	size_t pcm_buf_size_required = (size_t)cap_samples * (size_t)cap_channels * sizeof(int16_t);
	int16_t *pcm_buf_old = decoder->pcm_buf;
	if(!decoder->pcm_buf || decoder->pcm_buf_size != pcm_buf_size_required)
		decoder->pcm_buf = realloc(decoder->pcm_buf, pcm_buf_size_required);

	if(!decoder->pcm_buf)
	{
		free(pcm_buf_old);
		CHIAKI_LOGE(decoder->log, "ChiakiOpusDecoder failed to alloc pcm buffer");
		opus_decoder_destroy(decoder->opus_decoder);
		decoder->opus_decoder = NULL;
		decoder->pcm_buf_size = 0;
		return;
	}

	decoder->pcm_buf_size = pcm_buf_size_required;

	if(decoder->settings_cb)
		decoder->settings_cb(header->channels, header->rate, decoder->cb_user);
}

static int opus_decode_buf(struct OpusDecoder *opus_decoder, const uint8_t *buf, size_t buf_size, int16_t *pcm, int frame_size)
{
	if(!buf || buf_size < 1 || buf_size > 0x7fffffff)
		return OPUS_INVALID_PACKET;
	return opus_decode(opus_decoder, buf, (opus_int32)buf_size, pcm, frame_size, 0);
}

static size_t opus_trim_trailing_zeros(const uint8_t *buf, size_t buf_size)
{
	while(buf_size > 1 && buf[buf_size - 1] == 0)
		buf_size--;
	return buf_size;
}

// Accept only a whole 10 ms frame or a small multiple of it, up to 60 ms.
static int opus_whole_frame_samples(const uint8_t *buf, size_t buf_size, int channels)
{
	int samples;
	int config;
	if(!buf || buf_size < 1 || buf_size > 0x7fffffff)
		return -1;
	if(channels >= 2 && (buf[0] & 0x04) == 0)
		return -1;
	config = buf[0] >> 3;
	if(config < 12)
		return -1;
	samples = opus_packet_get_nb_samples(buf, (opus_int32)buf_size, 48000);
	if(samples < 480 || samples > 2880 || (samples % 480) != 0)
		return -1;
	return samples;
}

static void opus_log_decode_failure(ChiakiLog *log, const char *why, const uint8_t *buf, size_t buf_size)
{
	static unsigned logs;
	if(logs >= 4)
		return;
	logs++;
	uint8_t b[8] = { 0 };
	size_t n = buf_size < sizeof(b) ? buf_size : sizeof(b);
	if(buf && n)
		memcpy(b, buf, n);
	CHIAKI_LOGI(log, "Opus decode failed: %s size=%zu head=%02x %02x %02x %02x %02x %02x %02x %02x",
		why, buf_size, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
}

static void chiaki_opus_decoder_frame(uint8_t *buf, size_t buf_size, void *user)
{
	ChiakiOpusDecoder *decoder = user;
	if(!decoder->opus_decoder)
	{
		CHIAKI_LOGE(decoder->log, "Received audio frame, but opus decoder is not initialized");
		return;
	}

	int channels = decoder->audio_header.channels ? decoder->audio_header.channels : 2;
	int frame_size = (int)(decoder->pcm_buf_size / ((size_t)channels * sizeof(int16_t)));
	if(frame_size < (int)decoder->audio_header.frame_size)
		frame_size = (int)decoder->audio_header.frame_size;
	size_t packet_size = buf ? opus_trim_trailing_zeros(buf, buf_size) : 0;
	int expect = opus_whole_frame_samples(buf, packet_size, channels);
	if(expect < 1)
	{
		opus_log_decode_failure(decoder->log, "not a whole frame", buf, buf_size);
		return;
	}
	int r = opus_decode_buf(decoder->opus_decoder, buf, packet_size, decoder->pcm_buf, frame_size);
	if(r != expect)
	{
		opus_decoder_ctl(decoder->opus_decoder, OPUS_RESET_STATE);
		opus_log_decode_failure(decoder->log, r < 1 ? opus_strerror(r) : "short frame", buf, buf_size);
		return;
	}
	if(decoder->frame_cb)
		decoder->frame_cb(decoder->pcm_buf, (size_t)r, decoder->cb_user);
}

#endif