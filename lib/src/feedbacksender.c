// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include <chiaki/feedbacksender.h>
#include <chiaki/time.h>

#define FEEDBACK_STATE_TIMEOUT_MIN_MS 8 // minimum time to wait between sending 2 packets
#define FEEDBACK_STATE_TIMEOUT_MAX_MS 200 // maximum time to wait between sending 2 packets

#define FEEDBACK_HISTORY_BUFFER_SIZE 0x10

static void *feedback_sender_thread_func(void *user);

CHIAKI_EXPORT ChiakiErrorCode chiaki_feedback_sender_init(ChiakiFeedbackSender *feedback_sender, ChiakiTakion *takion)
{
	feedback_sender->log = takion->log;
	feedback_sender->takion = takion;

	chiaki_controller_state_set_idle(&feedback_sender->controller_state_prev);
	chiaki_controller_state_set_idle(&feedback_sender->controller_state_raw);
	chiaki_controller_state_set_idle(&feedback_sender->controller_state);

	feedback_sender->ps_chord.enabled = false; // seeded from the session on stream start; opt-in only
	feedback_sender->ps_chord.hold_ms = 2000;
	feedback_sender->ps_chord.chord_start_ms = 0;
	feedback_sender->ps_chord.pulse_until_ms = 0;
	feedback_sender->ps_chord.fired = false;
	feedback_sender->ps_chord.releasing = false;
	feedback_sender->ps_chord_fired_cb = NULL;
	feedback_sender->ps_chord_fired_user = NULL;

	feedback_sender->state_seq_num = 0;
	feedback_sender->should_stop = false;
	feedback_sender->controller_state_changed = false;

	feedback_sender->history_seq_num = 0;
	ChiakiErrorCode err = chiaki_feedback_history_buffer_init(&feedback_sender->history_buf, FEEDBACK_HISTORY_BUFFER_SIZE);
	if(err != CHIAKI_ERR_SUCCESS)
		return err;

	for(uint8_t pad = 0; pad < 4; pad++)
	{
		chiaki_controller_state_set_idle(&feedback_sender->extra_raw[pad]);
		chiaki_controller_state_set_idle(&feedback_sender->extra_state[pad]);
		chiaki_controller_state_set_idle(&feedback_sender->extra_prev[pad]);
		feedback_sender->extra_enabled[pad] = false;
		feedback_sender->extra_presence_pending[pad] = false;
		feedback_sender->extra_presence_on[pad] = false;
		err = chiaki_feedback_history_buffer_init(&feedback_sender->extra_history[pad], FEEDBACK_HISTORY_BUFFER_SIZE);
		if(err != CHIAKI_ERR_SUCCESS)
		{
			while(pad > 0)
			{
				pad--;
				chiaki_feedback_history_buffer_fini(&feedback_sender->extra_history[pad]);
			}
			chiaki_feedback_history_buffer_fini(&feedback_sender->history_buf);
			return err;
		}
	}

	err = chiaki_mutex_init(&feedback_sender->state_mutex, false);
	if(err != CHIAKI_ERR_SUCCESS)
		goto error_history_buffer;

	err = chiaki_cond_init(&feedback_sender->state_cond);
	if(err != CHIAKI_ERR_SUCCESS)
		goto error_mutex;

	err = chiaki_thread_create(&feedback_sender->thread, feedback_sender_thread_func, feedback_sender);
	if(err != CHIAKI_ERR_SUCCESS)
		goto error_cond;

	chiaki_thread_set_name(&feedback_sender->thread, "Chiaki Feedback Sender");

	return CHIAKI_ERR_SUCCESS;
error_cond:
	chiaki_cond_fini(&feedback_sender->state_cond);
error_mutex:
	chiaki_mutex_fini(&feedback_sender->state_mutex);
error_history_buffer:
	for(uint8_t pad = 0; pad < 4; pad++)
		chiaki_feedback_history_buffer_fini(&feedback_sender->extra_history[pad]);
	chiaki_feedback_history_buffer_fini(&feedback_sender->history_buf);
	return err;
}

CHIAKI_EXPORT void chiaki_feedback_sender_fini(ChiakiFeedbackSender *feedback_sender)
{
	chiaki_mutex_lock(&feedback_sender->state_mutex);
	feedback_sender->should_stop = true;
	chiaki_mutex_unlock(&feedback_sender->state_mutex);
	chiaki_cond_signal(&feedback_sender->state_cond);
	chiaki_thread_join(&feedback_sender->thread, NULL);
	chiaki_cond_fini(&feedback_sender->state_cond);
	chiaki_mutex_fini(&feedback_sender->state_mutex);
	for(uint8_t pad = 0; pad < 4; pad++)
		chiaki_feedback_history_buffer_fini(&feedback_sender->extra_history[pad]);
	chiaki_feedback_history_buffer_fini(&feedback_sender->history_buf);
}

CHIAKI_EXPORT ChiakiErrorCode chiaki_feedback_sender_set_controller_state(ChiakiFeedbackSender *feedback_sender, ChiakiControllerState *state)
{
	ChiakiErrorCode err = chiaki_mutex_lock(&feedback_sender->state_mutex);
	if(err != CHIAKI_ERR_SUCCESS)
		return err;

	// Dedupe against the RAW state: controller_state is post-chord-transform and
	// may legitimately differ from what platforms push (suppression/synthesis).
	if(chiaki_controller_state_equals(&feedback_sender->controller_state_raw, state))
	{
		chiaki_mutex_unlock(&feedback_sender->state_mutex);
		return CHIAKI_ERR_SUCCESS;
	}

	feedback_sender->controller_state_raw = *state;
	feedback_sender->controller_state_changed = true;

	chiaki_mutex_unlock(&feedback_sender->state_mutex);
	chiaki_cond_signal(&feedback_sender->state_cond);

	return CHIAKI_ERR_SUCCESS;
}

CHIAKI_EXPORT ChiakiErrorCode chiaki_feedback_sender_set_pad_state(ChiakiFeedbackSender *feedback_sender, uint8_t pad, ChiakiControllerState *state)
{
	if(pad == 0)
		return chiaki_feedback_sender_set_controller_state(feedback_sender, state);
	if(pad > 3 || !state)
		return CHIAKI_ERR_INVALID_DATA;
	ChiakiErrorCode err = chiaki_mutex_lock(&feedback_sender->state_mutex);
	if(err != CHIAKI_ERR_SUCCESS)
		return err;
	if(chiaki_controller_state_equals(&feedback_sender->extra_raw[pad], state))
	{
		chiaki_mutex_unlock(&feedback_sender->state_mutex);
		return CHIAKI_ERR_SUCCESS;
	}
	feedback_sender->extra_raw[pad] = *state;
	feedback_sender->controller_state_changed = true;
	chiaki_mutex_unlock(&feedback_sender->state_mutex);
	chiaki_cond_signal(&feedback_sender->state_cond);
	return CHIAKI_ERR_SUCCESS;
}

CHIAKI_EXPORT ChiakiErrorCode chiaki_feedback_sender_set_pad_enabled(ChiakiFeedbackSender *feedback_sender, uint8_t pad, bool enabled)
{
	if(pad == 0)
		return CHIAKI_ERR_SUCCESS;
	if(pad > 3)
		return CHIAKI_ERR_INVALID_DATA;
	ChiakiErrorCode err = chiaki_mutex_lock(&feedback_sender->state_mutex);
	if(err != CHIAKI_ERR_SUCCESS)
		return err;
	if(feedback_sender->extra_enabled[pad] != enabled)
	{
		feedback_sender->extra_enabled[pad] = enabled;
		if(!enabled)
			chiaki_controller_state_set_idle(&feedback_sender->extra_raw[pad]);
		feedback_sender->extra_presence_pending[pad] = true;
		feedback_sender->extra_presence_on[pad] = enabled;
		feedback_sender->controller_state_changed = true;
	}
	chiaki_mutex_unlock(&feedback_sender->state_mutex);
	chiaki_cond_signal(&feedback_sender->state_cond);
	return CHIAKI_ERR_SUCCESS;
}

CHIAKI_EXPORT void chiaki_feedback_sender_set_ps_chord(ChiakiFeedbackSender *feedback_sender, bool enabled, uint32_t hold_ms)
{
	if(chiaki_mutex_lock(&feedback_sender->state_mutex) != CHIAKI_ERR_SUCCESS)
		return;
	feedback_sender->ps_chord.enabled = enabled;
	if(hold_ms)
		feedback_sender->ps_chord.hold_ms = hold_ms;
	feedback_sender->ps_chord.chord_start_ms = 0;
	feedback_sender->ps_chord.pulse_until_ms = 0;
	feedback_sender->ps_chord.fired = false;
	feedback_sender->ps_chord.releasing = false;
	chiaki_mutex_unlock(&feedback_sender->state_mutex);
	chiaki_cond_signal(&feedback_sender->state_cond);
}

CHIAKI_EXPORT void chiaki_feedback_sender_set_ps_chord_fired_cb(ChiakiFeedbackSender *feedback_sender, void (*cb)(void *user), void *user)
{
	if(chiaki_mutex_lock(&feedback_sender->state_mutex) != CHIAKI_ERR_SUCCESS)
		return;
	feedback_sender->ps_chord_fired_cb = cb;
	feedback_sender->ps_chord_fired_user = user;
	chiaki_mutex_unlock(&feedback_sender->state_mutex);
}

CHIAKI_EXPORT void chiaki_ps_chord_apply(ChiakiPsChord *chord, ChiakiControllerState *state, uint64_t now_ms)
{
	if(!chord->enabled)
		return;
	const uint32_t both = CHIAKI_CONTROLLER_BUTTON_OPTIONS | CHIAKI_CONTROLLER_BUTTON_SHARE;
	bool held = (state->buttons & both) == both;
	if(held)
	{
		state->buttons &= ~both;
		if(chord->chord_start_ms == 0 || chord->releasing)
		{
			chord->chord_start_ms = now_ms;
			chord->releasing = false;
		}
		else if(!chord->fired && now_ms - chord->chord_start_ms >= chord->hold_ms)
		{
			chord->fired = true;
			chord->pulse_until_ms = now_ms + CHIAKI_PS_CHORD_PULSE_MS;
		}
	}
	else if(chord->chord_start_ms != 0 && (state->buttons & both))
	{
		state->buttons &= ~both;
		chord->releasing = true;
	}
	else
	{
		chord->chord_start_ms = 0;
		chord->fired = false;
		chord->releasing = false;
	}
	if(chord->pulse_until_ms)
	{
		if(now_ms < chord->pulse_until_ms)
			state->buttons |= CHIAKI_CONTROLLER_BUTTON_PS;
		else
			chord->pulse_until_ms = 0;
	}
}

static bool controller_state_equals_for_feedback_state(ChiakiControllerState *a, ChiakiControllerState *b)
{
	if(!(a->left_x == b->left_x
		&& a->left_y == b->left_y
		&& a->right_x == b->right_x
		&& a->right_y == b->right_y))
		return false;
#define CHECKF(n) if(a->n < b->n - 0.0000001f || a->n > b->n + 0.0000001f) return false
	CHECKF(gyro_x);
	CHECKF(gyro_y);
	CHECKF(gyro_z);
	CHECKF(accel_x);
	CHECKF(accel_y);
	CHECKF(accel_z);
	CHECKF(orient_x);
	CHECKF(orient_y);
	CHECKF(orient_z);
	CHECKF(orient_w);
#undef CHECKF
	return true;
}

static void feedback_sender_send_state(ChiakiFeedbackSender *feedback_sender, ChiakiControllerState *cs, uint8_t pad)
{
	ChiakiFeedbackState state;
	state.left_x = cs->left_x;
	state.left_y = cs->left_y;
	state.right_x = cs->right_x;
	state.right_y = cs->right_y;
	state.gyro_x = cs->gyro_x;
	state.gyro_y = cs->gyro_y;
	state.gyro_z = cs->gyro_z;
	state.accel_x = cs->accel_x;
	state.accel_y = cs->accel_y;
	state.accel_z = cs->accel_z;
	state.orient_x = cs->orient_x;
	state.orient_y = cs->orient_y;
	state.orient_z = cs->orient_z;
	state.orient_w = cs->orient_w;

	ChiakiErrorCode err = chiaki_takion_send_feedback_state(feedback_sender->takion, feedback_sender->state_seq_num++, &state, pad);
	if(err != CHIAKI_ERR_SUCCESS)
		CHIAKI_LOGE(feedback_sender->log, "FeedbackSender failed to send Feedback State");
}

static bool controller_state_equals_for_feedback_history(ChiakiControllerState *a, ChiakiControllerState *b)
{
	if(!(a->buttons == b->buttons
		&& a->l2_state == b->l2_state
		&& a->r2_state == b->r2_state))
		return false;

	for(size_t i=0; i<CHIAKI_CONTROLLER_TOUCHES_MAX; i++)
	{
		if(a->touches[i].id != b->touches[i].id)
			return false;
		if(a->touches[i].id >= 0 && (a->touches[i].x != b->touches[i].x || a->touches[i].y != b->touches[i].y))
			return false;
	}
	return true;
}

static void feedback_sender_send_history_packet(ChiakiFeedbackSender *feedback_sender, ChiakiFeedbackHistoryBuffer *hist)
{
	uint8_t buf[0x300];
	size_t buf_size = sizeof(buf);
	ChiakiErrorCode err = chiaki_feedback_history_buffer_format(hist, buf, &buf_size);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		CHIAKI_LOGE(feedback_sender->log, "Feedback Sender failed to format history buffer");
		return;
	}

	chiaki_takion_send_feedback_history(feedback_sender->takion, feedback_sender->history_seq_num++, buf, buf_size);
}

static void feedback_sender_push_event(ChiakiFeedbackSender *feedback_sender, ChiakiFeedbackHistoryBuffer *hist, ChiakiFeedbackHistoryEvent *event, uint8_t pad)
{
	if(event->len)
		chiaki_feedback_stamp_pad_index(event->buf, pad);
	chiaki_feedback_history_buffer_push(hist, event);
	feedback_sender_send_history_packet(feedback_sender, hist);
}

static void feedback_sender_send_history(ChiakiFeedbackSender *feedback_sender, ChiakiControllerState *state_prev, ChiakiControllerState *state_now, ChiakiFeedbackHistoryBuffer *hist, uint8_t pad)
{
	uint64_t buttons_prev = state_prev->buttons;
	uint64_t buttons_now = state_now->buttons;
	for(uint8_t i=0; i<CHIAKI_CONTROLLER_BUTTONS_COUNT; i++)
	{
		uint64_t button_id = 1 << i;
		bool prev = buttons_prev & button_id;
		bool now = buttons_now & button_id;
		if(prev != now)
		{
			ChiakiFeedbackHistoryEvent event;
			ChiakiErrorCode err = chiaki_feedback_history_event_set_button(&event, button_id, now ? 0xff : 0);
			if(err != CHIAKI_ERR_SUCCESS)
			{
				CHIAKI_LOGE(feedback_sender->log, "Feedback Sender failed to format button history event for button id %llu", (unsigned long long)button_id);
				continue;
			}
			feedback_sender_push_event(feedback_sender, hist, &event, pad);
		}
	}

	if(state_prev->l2_state != state_now->l2_state)
	{
		ChiakiFeedbackHistoryEvent event;
		ChiakiErrorCode err = chiaki_feedback_history_event_set_button(&event, CHIAKI_CONTROLLER_ANALOG_BUTTON_L2, state_now->l2_state);
		if(err == CHIAKI_ERR_SUCCESS)
			feedback_sender_push_event(feedback_sender, hist, &event, pad);
		else
			CHIAKI_LOGE(feedback_sender->log, "Feedback Sender failed to format button history event for L2");
	}

	if(state_prev->r2_state != state_now->r2_state)
	{
		ChiakiFeedbackHistoryEvent event;
		ChiakiErrorCode err = chiaki_feedback_history_event_set_button(&event, CHIAKI_CONTROLLER_ANALOG_BUTTON_R2, state_now->r2_state);
		if(err == CHIAKI_ERR_SUCCESS)
			feedback_sender_push_event(feedback_sender, hist, &event, pad);
		else
			CHIAKI_LOGE(feedback_sender->log, "Feedback Sender failed to format button history event for R2");
	}

	for(size_t i=0; i<CHIAKI_CONTROLLER_TOUCHES_MAX; i++)
	{
		if(state_prev->touches[i].id != state_now->touches[i].id && state_prev->touches[i].id >= 0)
		{
			ChiakiFeedbackHistoryEvent event;
			chiaki_feedback_history_event_set_touchpad(&event, false, (uint8_t)state_prev->touches[i].id,
					state_prev->touches[i].x, state_prev->touches[i].y);
			feedback_sender_push_event(feedback_sender, hist, &event, pad);
		}
		else if(state_now->touches[i].id >= 0
				&& (state_prev->touches[i].id != state_now->touches[i].id
					|| state_prev->touches[i].x != state_now->touches[i].x
					|| state_prev->touches[i].y != state_now->touches[i].y))
		{
			ChiakiFeedbackHistoryEvent event;
			chiaki_feedback_history_event_set_touchpad(&event, true, (uint8_t)state_now->touches[i].id,
					state_now->touches[i].x, state_now->touches[i].y);
			feedback_sender_push_event(feedback_sender, hist, &event, pad);
		}
	}
}

static bool state_cond_check(void *user)
{
	ChiakiFeedbackSender *feedback_sender = user;
	return feedback_sender->should_stop || feedback_sender->controller_state_changed;
}

static void *feedback_sender_thread_func(void *user)
{
	ChiakiFeedbackSender *feedback_sender = user;

	ChiakiErrorCode err = chiaki_mutex_lock(&feedback_sender->state_mutex);
	if(err != CHIAKI_ERR_SUCCESS)
		return NULL;

	uint64_t next_timeout = FEEDBACK_STATE_TIMEOUT_MAX_MS;
	while(true)
	{
		err = chiaki_cond_timedwait_pred(&feedback_sender->state_cond, &feedback_sender->state_mutex, next_timeout, state_cond_check, feedback_sender);
		if(err != CHIAKI_ERR_SUCCESS && err != CHIAKI_ERR_TIMEOUT)
			break;

		if(feedback_sender->should_stop)
			break;

		bool timeout_wake = !feedback_sender->controller_state_changed;
		feedback_sender->controller_state_changed = false;

		feedback_sender->controller_state = feedback_sender->controller_state_raw;
		bool chord_fired_before = feedback_sender->ps_chord.fired;
		chiaki_ps_chord_apply(&feedback_sender->ps_chord, &feedback_sender->controller_state,
			chiaki_time_now_monotonic_ms());
		bool chord_just_fired = !chord_fired_before && feedback_sender->ps_chord.fired;

		bool send_feedback_state = timeout_wake
			|| !controller_state_equals_for_feedback_state(&feedback_sender->controller_state, &feedback_sender->controller_state_prev);

		bool send_feedback_history = !controller_state_equals_for_feedback_history(&feedback_sender->controller_state, &feedback_sender->controller_state_prev);

		if(send_feedback_state)
			feedback_sender_send_state(feedback_sender, &feedback_sender->controller_state, 0);

		if(send_feedback_history)
			feedback_sender_send_history(feedback_sender, &feedback_sender->controller_state_prev, &feedback_sender->controller_state, &feedback_sender->history_buf, 0);

		feedback_sender->controller_state_prev = feedback_sender->controller_state;

		for(uint8_t pad = 1; pad < 4; pad++)
		{
			if(feedback_sender->extra_presence_pending[pad])
			{
				ChiakiFeedbackHistoryEvent presence;
				presence.len = 2;
				presence.buf[0] = (uint8_t)(0x80 | pad);
				presence.buf[1] = (uint8_t)(0x1f | 0x80 | (feedback_sender->extra_presence_on[pad] ? 0x20 : 0));
				feedback_sender_push_event(feedback_sender, &feedback_sender->extra_history[pad], &presence, pad);
				feedback_sender->extra_presence_pending[pad] = false;
			}
			if(!feedback_sender->extra_enabled[pad])
				continue;
			feedback_sender->extra_state[pad] = feedback_sender->extra_raw[pad];
			bool send_state = timeout_wake
				|| !controller_state_equals_for_feedback_state(&feedback_sender->extra_state[pad], &feedback_sender->extra_prev[pad]);
			bool send_hist = !controller_state_equals_for_feedback_history(&feedback_sender->extra_state[pad], &feedback_sender->extra_prev[pad]);
			if(send_state)
				feedback_sender_send_state(feedback_sender, &feedback_sender->extra_state[pad], pad);
			if(send_hist)
				feedback_sender_send_history(feedback_sender, &feedback_sender->extra_prev[pad], &feedback_sender->extra_state[pad], &feedback_sender->extra_history[pad], pad);
			feedback_sender->extra_prev[pad] = feedback_sender->extra_state[pad];
		}

		if(chord_just_fired && feedback_sender->ps_chord_fired_cb)
		{
			void (*fired_cb)(void *user) = feedback_sender->ps_chord_fired_cb;
			void *fired_user = feedback_sender->ps_chord_fired_user;
			chiaki_mutex_unlock(&feedback_sender->state_mutex);
			fired_cb(fired_user);
			err = chiaki_mutex_lock(&feedback_sender->state_mutex);
			if(err != CHIAKI_ERR_SUCCESS)
				return NULL;
		}
	}

	chiaki_mutex_unlock(&feedback_sender->state_mutex);

	return NULL;
}
