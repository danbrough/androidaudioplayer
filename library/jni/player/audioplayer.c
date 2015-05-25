/*
 * avplay : Simple Media Player based on the Libav libraries
 * Copyright (c) 2003 Fabrice Bellard
 *
 * This file is part of Libav.
 *
 * Libav is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * Libav is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with Libav; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */
#define _GNU_SOURCE
#include <inttypes.h>
#include <math.h>
#include <limits.h>
#include <stdint.h>
#include <fcntl.h>
#include <libavutil/avstring.h>
#include <libavutil/dict.h>
#include <libavutil/samplefmt.h>

#include "audioplayer.h"

const char * ap_get_state_name(audio_state_t state) {
	switch (state) {
	case STATE_IDLE:
		return "STATE_IDLE";
	case STATE_INITIALIZED:
		return "STATE_INITIALIZED";
	case STATE_STOPPED:
		return "STATE_STOPPED";
	case STATE_PAUSED:
		return "STATE_PAUSED";
	case STATE_PREPARING:
		return "STATE_PREPARING";
	case STATE_PREPARED:
		return "STATE_PREPARED";
	case STATE_STARTED:
		return "STATE_STARTED";
	case STATE_COMPLETED:
		return "STATE_COMPLETED";

	default:
		return "STATE_INVALID";
	}
}

const char* ap_get_cmd_name(audio_cmd_t cmd) {
	switch (cmd) {
	case CMD_PREPARE:
		return "CMD_PREPARE";
	case CMD_START:
		return "CMD_START";
	case CMD_PAUSE:
		return "CMD_PAUSE";
	case CMD_STOP:
		return "CMD_STOP";
	case CMD_SEEK:
		return "CMD_SEEK";
	case CMD_RESET:
		return "CMD_RESET";
	case CMD_EXIT:
		return "CMD_EXIT";
	case CMD_SET_DATASOURCE:
		return "CMD_SET_DATASOURCE";
	}
	return "CMD_UNKNOWN";
}

int ap_send_cmd(player_t *player, audio_cmd_t cmd) {
	log_trace("ap_send_cmd::%s", ap_get_cmd_name(cmd));
	return write(player->pipe[1], &cmd, sizeof(cmd));
}

void ap_print_error(const char* msg, int err) {
	char buf[128];
	if (av_strerror(err, buf, sizeof(buf)) == 0) {
		log_error("%s: code:%d \"%s\"", msg, err, buf);
	} else {
		log_error("%s unknown error: %d", msg, err);
	}
}

/* get the current audio output buffer size, in samples. With SDL, we
 cannot have a precise information */
static int audio_write_get_buf_size(player_t *player) {
	return player->audio_buf_size - player->audio_buf_index;
}

/* get the current audio clock value */
double ap_get_audio_clock(player_t *player) {
	double pts = 0;
	int hw_buf_size, bytes_per_sec;
	pts = player->audio_clock;

	hw_buf_size = audio_write_get_buf_size(player);

	bytes_per_sec = 0;
	if (player->audio_st) {
		bytes_per_sec = player->sdl_sample_rate * player->sdl_channels
				* av_get_bytes_per_sample(player->sdl_sample_fmt);
	}

	if (bytes_per_sec)
		pts -= (double) hw_buf_size / bytes_per_sec;
	return pts;
}

/* seek in the stream */
static void stream_seek(player_t *player, int64_t pos, int64_t rel) {
	BEGIN_LOCK(player);
	log_trace("stream_seek %"PRIu64" : %"PRIu64, pos, rel);
	if (!player->seek_req) {
		player->seek_pos = pos;
		player->seek_rel = rel;
		player->seek_flags = AVSEEK_FLAG_FRAME;
		/*	player->seek_flags &= ~AVSEEK_FLAG_BYTE;
		 if (seek_by_bytes)
		 player->seek_flags |= AVSEEK_FLAG_BYTE;*/
		player->seek_req = 1;
	}
	END_LOCK(player);
}

/* pause or resume the video */
int ap_pause(player_t *player) {
	log_trace("ap_pause()");
	return ap_send_cmd(player,
			player->state == STATE_STARTED ? CMD_PAUSE : CMD_START);
}

static void log_callback_help(void *ptr, int level, const char *fmt, va_list vl) {
	vfprintf(stdout, fmt, vl);
}

int ap_init() {
	log_debug("ap_init()");

	av_log_set_flags(AV_LOG_SKIP_REPEATED);
	av_log_set_callback(log_callback_help);
	avcodec_register_all();
	av_register_all();
	avformat_network_init();

	return 0;
}

void ap_uninit() {
	log_debug("ap_uninit()");
	avformat_network_deinit();
}

extern int player_thread(player_t *player);

static int start_thread(player_t *player) {
	int ret = 0;
	BEGIN_LOCK(player);
	log_info("start_thread()");

	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	if ((ret = pthread_create(&player->player_thread, &attr,
			(void*) player_thread, player)) != SUCCESS) {
		log_error("failed to start decode thread: %s", strerror(errno));
	}
	pthread_attr_destroy(&attr);
	END_LOCK(player);
	return ret;
}

player_t* ap_create(player_callbacks_t callbacks) {
	log_info("ap_create()");
	player_t *player = malloc(sizeof(player_t));
	if (!player)
		return NULL;
	memset(player, 0, sizeof(player_t));
	player->callbacks = callbacks;
	pthread_mutexattr_t attr;
	pthread_mutexattr_init(&attr);
	pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&player->mutex, &attr);
	pthread_mutexattr_destroy(&attr);

	if (pipe2(player->pipe, O_NONBLOCK) != 0) {
		log_error("pipe failed");
		ap_delete(player);
		return NULL;
	}

	if (start_thread(player) != SUCCESS) {
		log_error("ap_create::failed to start thread");
		ap_delete(player);
		return NULL;
	}

	return player;
}

void ap_delete(player_t* player) {
	log_info("ap_delete()");
	if (!player)
		return;

	ap_reset(player);
	ap_send_cmd(player, CMD_EXIT);

}

int ap_reset(player_t *player) {
	log_debug("ap_reset()");
	return ap_send_cmd(player, CMD_RESET);
}

int ap_set_datasource(player_t *player, const char *url) {
	log_info("ap_set_datasource() %s", url);
	int ret = -1;
	if (player->state == STATE_IDLE) {
		av_strlcpy(player->url, url, sizeof(player->url));
		ret = ap_send_cmd(player, CMD_SET_DATASOURCE);
	} else {
		log_error("ap_set_datasource::invalid state: %s",
				ap_get_state_name(player->state));
		ret = FAILURE;

	}
	return ret;
}

int ap_prepare_async(player_t *player) {
	log_error("ap_prepare_async()");
	ap_send_cmd(player, CMD_PREPARE);
	return SUCCESS;
}

void ap_seek(player_t *player, int64_t incr, int relative) {
	log_debug("ap_seek() :%"PRIi64" relative: %s", incr,
			relative ? "true" : "false");

	BEGIN_LOCK(player);
	if (!player->audio_st)
		goto end;

	int64_t pos;
	if (relative) {
		pos = ap_get_audio_clock(player) * AV_TIME_BASE;
		pos += incr;
	} else {
		pos = incr;
	}
	stream_seek(player, pos, incr);

	end:
	END_LOCK(player);
}

int ap_start(player_t *player) {
	log_info("ap_start()");
	return ap_send_cmd(player, CMD_START);
}

int ap_stop(player_t *player) {
	log_info("ap_stop()");

	return ap_send_cmd(player, CMD_STOP);
}

void ap_print_status(player_t *player) {
	int hours, mins, secs;

	BEGIN_LOCK(player);

	if (!player->audio_st) {
		log_trace("ap_print_status():: not playing");
		goto end;
	}

	int64_t duration = player->ic->duration;
	if (duration == AV_NOPTS_VALUE) {
		duration = -1;
		hours = mins = secs = 0;
	} else {
		duration = duration / AV_TIME_BASE;
		secs = duration % 60;
		hours = (duration / (60 * 60));
		mins = (duration - hours * 60 * 60) / 60;
	}
	log_trace(
			"ap_print_status(): state:%s pos:%.2f duration:%"PRIi64" %02d:%02d:%02d",
			ap_get_state_name(player->state), ap_get_audio_clock(player),
			player->ic->duration == AV_NOPTS_VALUE ? 0 : player->ic->duration,
			hours, mins, secs);
	end:
	END_LOCK(player);
}

void ap_print_metadata(player_t *player) {
	if (player->ic) {
		AVDictionaryEntry *entry = NULL;
		while ((entry = av_dict_get(player->ic->metadata, "", entry,
		AV_DICT_IGNORE_SUFFIX)))
			log_trace("metadata:\t%s:%s", entry->key, entry->value);
	}
}

//duration of current track in ms
int32_t ap_get_duration(player_t *player) {

	if (player->state == STATE_PREPARED || player->state == STATE_STARTED
			|| player->state == STATE_PAUSED || player->state == STATE_STOPPED
			|| player->state == STATE_COMPLETED) {
		if (player && player->ic
				&& player->ic->duration
						> 0&& player->ic->duration != AV_NOPTS_VALUE)
			return (int32_t) (player->ic->duration / 1000);
	} else {
		log_error("ap_get_duration() called in illegal state: %s",
				ap_get_state_name(player->state));
	}
	return -1;
}

//position in current track in ms
int32_t ap_get_position(player_t *player) {
	return (int32_t) (ap_get_audio_clock(player) * 1000);
}

int ap_is_playing(player_t *player) {
	return player->state == STATE_STARTED;
}

int ap_is_looping(player_t *player) {
	return player->looping;
}

void ap_set_looping(player_t *player, int looping) {
	player->looping = looping;
}
