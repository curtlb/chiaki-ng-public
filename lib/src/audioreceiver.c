// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include <chiaki/audioreceiver.h>
#include <chiaki/session.h>
#include <chiaki/log.h>

#include <stdlib.h>
#include <string.h>

static void chiaki_audio_receiver_frame(ChiakiAudioReceiver *audio_receiver, ChiakiSeqNum16 frame_index, bool is_haptics, uint8_t *buf, size_t buf_size);
static void chiaki_audio_receiver_emit(ChiakiAudioReceiver *audio_receiver, bool is_haptics, uint8_t *buf, size_t buf_size);

CHIAKI_EXPORT ChiakiErrorCode chiaki_audio_receiver_init(ChiakiAudioReceiver *audio_receiver, ChiakiSession *session, ChiakiPacketStats *packet_stats)
{
	audio_receiver->session = session;
	audio_receiver->log = session->log;
	audio_receiver->packet_stats = packet_stats;

	audio_receiver->frame_index_prev = 0;
	audio_receiver->frame_index_prev_valid = false;
	audio_receiver->frame_index_startup = true;
	audio_receiver->pscloud_audio_reassembler = NULL;

	ChiakiErrorCode err = chiaki_mutex_init(&audio_receiver->mutex, false);
	if(err != CHIAKI_ERR_SUCCESS)
		return err;

	if(session)
	{
		CHIAKI_LOGI(session->log, "Audio Receiver: standard decoder for %s",
			chiaki_service_type_string(session->service_type));
	}

	return CHIAKI_ERR_SUCCESS;
}

CHIAKI_EXPORT void chiaki_audio_receiver_fini(ChiakiAudioReceiver *audio_receiver)
{
	// The GUI owns the Opus decoder. Destroying one here raced the sink callback
	// on stream close, and this receiver never created a decoder of its own.
	chiaki_mutex_fini(&audio_receiver->mutex);
}

CHIAKI_EXPORT void chiaki_audio_receiver_stream_info(ChiakiAudioReceiver *audio_receiver, ChiakiAudioHeader *audio_header)
{
	chiaki_mutex_lock(&audio_receiver->mutex);

	CHIAKI_LOGI(audio_receiver->log, "Audio Header:");
	CHIAKI_LOGI(audio_receiver->log, "  channels = %d", audio_header->channels);
	CHIAKI_LOGI(audio_receiver->log, "  bits = %d", audio_header->bits);
	CHIAKI_LOGI(audio_receiver->log, "  rate = %d", audio_header->rate);
	CHIAKI_LOGI(audio_receiver->log, "  frame size = %d", audio_header->frame_size);
	CHIAKI_LOGI(audio_receiver->log, "  unknown = %d", audio_header->unknown);

	if(audio_receiver->session->audio_sink.header_cb)
		audio_receiver->session->audio_sink.header_cb(audio_header, audio_receiver->session->audio_sink.user);

	chiaki_mutex_unlock(&audio_receiver->mutex);
}

CHIAKI_EXPORT void chiaki_audio_receiver_av_packet(ChiakiAudioReceiver *audio_receiver, ChiakiTakionAVPacket *packet)
{
	if(packet->codec != 5)
	{
		CHIAKI_LOGE(audio_receiver->log, "Received Audio Packet with unknown Codec");
		return;
	}

	bool is_pscloud = audio_receiver->session
		&& audio_receiver->session->service_type == CHIAKI_SERVICE_TYPE_PSCLOUD;

	uint8_t source_units_count = chiaki_takion_av_packet_audio_source_units_count(packet);
	uint8_t fec_units_count = chiaki_takion_av_packet_audio_fec_units_count(packet);
	uint8_t unit_size = chiaki_takion_av_packet_audio_unit_size(packet);
	// Remote Play packs unit size in the high byte and the source/FEC counts in the low nibbles.
	// 0x3C21 is 60-byte units, 1 source + 2 FEC. A small literal FEC count is one unit per datagram.
	bool packed_header = unit_size > 0
		&& (uint16_t)source_units_count + (uint16_t)fec_units_count == packet->units_in_frame_total;
	bool packed_frame = packed_header
		&& packet->data_size == (size_t)unit_size * (size_t)packet->units_in_frame_total;

	if(is_pscloud && !packed_frame)
	{
		if(packed_header && packet->unit_index < source_units_count && packet->data && packet->data_size)
			chiaki_audio_receiver_emit(audio_receiver, packet->is_haptics, packet->data, packet->data_size);
		else if(!packed_header && packet->data && packet->data_size
			&& packet->units_in_frame_fec <= packet->units_in_frame_total)
		{
			uint16_t source_units = (uint16_t)(packet->units_in_frame_total - packet->units_in_frame_fec);
			if(source_units && packet->unit_index < source_units)
				chiaki_audio_receiver_emit(audio_receiver, packet->is_haptics, packet->data, packet->data_size);
		}
		return;
	}

	if(is_pscloud && packed_frame)
	{
		if(source_units_count && unit_size && packet->data && packet->data_size >= unit_size)
		{
			size_t n = source_units_count;
			if(n * (size_t)unit_size > packet->data_size)
				n = packet->data_size / unit_size;
			for(size_t i = 0; i < n; i++)
				chiaki_audio_receiver_emit(audio_receiver, packet->is_haptics,
					packet->data + i * unit_size, unit_size);
		}
		return;
	}

	if(!packet->data_size)
	{
		CHIAKI_LOGE(audio_receiver->log, "Audio AV Packet is empty");
		return;
	}

	if((uint16_t)fec_units_count + (uint16_t)source_units_count != packet->units_in_frame_total)
	{
		CHIAKI_LOGE(audio_receiver->log, "Source Units + FEC Units != Total Units in Audio AV Packet");
		return;
	}

	if(packet->data_size != (size_t)unit_size * (size_t)packet->units_in_frame_total)
	{
		CHIAKI_LOGE(audio_receiver->log, "Audio AV Packet size mismatch %#llx vs %#llx",
			(unsigned long long)packet->data_size,
			(unsigned long long)(unit_size * packet->units_in_frame_total));
		return;
	}

	if(packet->frame_index > (1 << 15))
		audio_receiver->frame_index_startup = false;

	for(size_t i = 0; i < source_units_count + fec_units_count; i++)
	{
		ChiakiSeqNum16 frame_index;
		if(i < source_units_count)
			frame_index = packet->frame_index + i;
		else
		{
			size_t fec_index = i - source_units_count;

			if(audio_receiver->frame_index_startup && packet->frame_index + fec_index < fec_units_count + 1)
				continue;

			frame_index = packet->frame_index - fec_units_count + fec_index;
		}

		chiaki_audio_receiver_frame(audio_receiver, frame_index, packet->is_haptics, packet->data + unit_size * i, unit_size);
	}

	if(audio_receiver->packet_stats && !is_pscloud)
		chiaki_packet_stats_push_seq(audio_receiver->packet_stats, packet->frame_index);
}

static void chiaki_audio_receiver_emit(ChiakiAudioReceiver *audio_receiver, bool is_haptics, uint8_t *buf, size_t buf_size)
{
	if(is_haptics && audio_receiver->session->haptics_sink.frame_cb)
		audio_receiver->session->haptics_sink.frame_cb(buf, buf_size, audio_receiver->session->haptics_sink.user);
	else if(!is_haptics && audio_receiver->session->audio_sink.frame_cb)
		audio_receiver->session->audio_sink.frame_cb(buf, buf_size, audio_receiver->session->audio_sink.user);
}

static void chiaki_audio_receiver_frame(ChiakiAudioReceiver *audio_receiver, ChiakiSeqNum16 frame_index, bool is_haptics, uint8_t *buf, size_t buf_size)
{
	chiaki_mutex_lock(&audio_receiver->mutex);

	if(audio_receiver->frame_index_prev_valid && !chiaki_seq_num_16_gt(frame_index, audio_receiver->frame_index_prev))
		goto beach;
	audio_receiver->frame_index_prev_valid = true;
	audio_receiver->frame_index_prev = frame_index;

	if(is_haptics && audio_receiver->session->haptics_sink.frame_cb)
		audio_receiver->session->haptics_sink.frame_cb(buf, buf_size, audio_receiver->session->haptics_sink.user);
	else if(!is_haptics && audio_receiver->session->audio_sink.frame_cb)
		audio_receiver->session->audio_sink.frame_cb(buf, buf_size, audio_receiver->session->audio_sink.user);

beach:
	chiaki_mutex_unlock(&audio_receiver->mutex);
}
