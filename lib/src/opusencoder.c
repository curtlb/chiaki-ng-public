// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include <chiaki/config.h>
#if CHIAKI_LIB_ENABLE_OPUS

#include <chiaki/opusencoder.h>

#include <opus/opus.h>

#include <string.h>

CHIAKI_EXPORT void chiaki_opus_encoder_init(ChiakiOpusEncoder *encoder, ChiakiLog *log)
{
	encoder->log = log;
	encoder->opus_encoder = NULL;
	memset(&encoder->audio_header, 0, sizeof(encoder->audio_header));
	encoder->audio_sender = NULL;
	encoder->opus_frame_buf = NULL;
	encoder->opus_frame_buf_size = 0;
}

CHIAKI_EXPORT void chiaki_opus_encoder_fini(ChiakiOpusEncoder *encoder)
{
	free(encoder->opus_frame_buf);
	chiaki_audio_sender_free(encoder->audio_sender);
	if(encoder->opus_encoder)
		opus_encoder_destroy(encoder->opus_encoder);
}

CHIAKI_EXPORT void chiaki_opus_encoder_header(ChiakiAudioHeader *header, ChiakiOpusEncoder *encoder, ChiakiSession *session)
{
	memcpy(&encoder->audio_header, header, sizeof(encoder->audio_header));

	opus_encoder_destroy(encoder->opus_encoder);
	int error;
	int application = OPUS_APPLICATION_RESTRICTED_LOWDELAY;
	encoder->audio_sender = chiaki_audio_sender_new(encoder->log, session);

	if(!encoder->audio_sender)
	{
		CHIAKI_LOGE(encoder->log, "Opus encoder failed to initialize Audio Sender");
		return;
	}

	encoder->opus_encoder = opus_encoder_create(header->rate, header->channels, application, &error);

	if(error != OPUS_OK)
	{
		CHIAKI_LOGE(encoder->log, "ChiakiOpusEncoder failed to initialize opus encoder: %s", opus_strerror(error));
		encoder->opus_encoder = NULL;
		return;
	}

	if(session && chiaki_service_type_is_cloud(session->service_type))
	{
		// 480 samples at 48 kHz is 10 ms. 40 bytes * 100 frames/s is 32 kbit/s.
		// The console counts a microphone unit only when it is exactly 40 bytes.
		opus_encoder_ctl(encoder->opus_encoder, OPUS_SET_BITRATE(32000));
		opus_encoder_ctl(encoder->opus_encoder, OPUS_SET_VBR(0));
		CHIAKI_LOGI(encoder->log, "Cloud microphone encoder: 2-channel constant 40-byte frames");
	}

	CHIAKI_LOGI(encoder->log, "ChiakiOpusEncoder initialized");

	size_t opus_frame_buf_size_required = 40;
	uint8_t *opus_frame_buf_old = encoder->opus_frame_buf;
	if(!encoder->opus_frame_buf || encoder->opus_frame_buf_size != opus_frame_buf_size_required)
		encoder->opus_frame_buf = realloc(encoder->opus_frame_buf, opus_frame_buf_size_required);

	if(!encoder->opus_frame_buf)
	{
		free(opus_frame_buf_old);
		CHIAKI_LOGE(encoder->log, "ChiakiOpusEncoder failed to alloc opus buffer");
		opus_encoder_destroy(encoder->opus_encoder);
		encoder->opus_encoder = NULL;
		encoder->opus_frame_buf_size = 0;
		return;
	}

	encoder->opus_frame_buf_size = opus_frame_buf_size_required;
}

CHIAKI_EXPORT void chiaki_opus_encoder_frame(int16_t *pcm_buf, ChiakiOpusEncoder *encoder)
{
	if(!encoder->opus_encoder)
	{
		CHIAKI_LOGE(encoder->log, "Received audio frame, but opus encoder is not initialized");
		return;
	}

	int r = opus_encode(encoder->opus_encoder, pcm_buf, encoder->audio_header.frame_size, encoder->opus_frame_buf, encoder->opus_frame_buf_size);
	if(r < 1)
		CHIAKI_LOGE(encoder->log, "Encoding audio frame with opus failed: %s", opus_strerror(r));
	else
	{
		chiaki_audio_sender_opus_data(encoder->audio_sender, encoder->opus_frame_buf, (size_t)r);
	}
}

#endif