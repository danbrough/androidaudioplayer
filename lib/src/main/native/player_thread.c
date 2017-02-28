#include <assert.h>

#include "audioplayer.h"
#include <libavutil/opt.h>
#include <sys/epoll.h>
#include "logging.h"
static int decode_interrupt_cb(player_t *player) {
	//log_trace("decode_interrupt returning %d", player && player->abort_call);
	return player && player->abort_call;
}

//see audiostream.c
// "seek by bytes 0=off 1=on -1=auto"
//int seek_by_bytes = -1;

static int workaround_bugs = 1;
//"non spec compliant optimizations"
static int fast = 0;
const int genpts = 0;
static int idct = FF_IDCT_AUTO;
static enum AVDiscard skip_idct = AVDISCARD_DEFAULT;
static enum AVDiscard skip_loop_filter = AVDISCARD_DEFAULT;
static int error_concealment = 3;
//  "don't limit the input buffer size (useful with realtime streams)"

static int st_index[AVMEDIA_TYPE_NB] = { 0 };
//wait indefinitely
static int epoll_timeout = -1;

static int eof = 0;
static AVAudioResampleContext *avr = NULL;
static AVFrame *frame = NULL;
static AVPacket pkt;
static uint8_t *audio_buf = NULL;

static int wanted_stream[AVMEDIA_TYPE_NB] = { [AVMEDIA_TYPE_AUDIO] = -1,
		[AVMEDIA_TYPE_VIDEO] = -1, [AVMEDIA_TYPE_SUBTITLE] = -1, };

extern const char* ap_get_cmd_name(audio_cmd_t cmd);

static int change_state(player_t *player, audio_state_t state) {
	int ret = -1;
	log_trace("[%" PRIXPTR "] change_state() %s", (intptr_t )pthread_self(),
			ap_get_state_name(state));
	BEGIN_LOCK(player);
	audio_state_t old_state = player->state;

	if ((state == STATE_IDLE || state == STATE_ERROR || state == STATE_END)
			|| (old_state == STATE_IDLE && state == STATE_INITIALIZED)

			|| (old_state == STATE_INITIALIZED
					&& (state == STATE_PREPARING || state == STATE_PREPARED))

			|| (old_state == STATE_PREPARING && state == STATE_PREPARED)

			|| (old_state == STATE_PREPARED
					&& (state == STATE_STOPPED || state == STATE_STARTED))

			|| (old_state == STATE_STARTED
					&& (state == STATE_PAUSED || state == STATE_STOPPED
							|| state == STATE_COMPLETED))

			|| (old_state == STATE_PAUSED
					&& (state == STATE_STOPPED || state == STATE_STARTED))

			|| (old_state == STATE_COMPLETED
					&& (state == STATE_STOPPED || state == STATE_STARTED))

			|| (old_state == STATE_STOPPED
					&& (state == STATE_PREPARING || state == STATE_PREPARED))

			) {
		ret = SUCCESS;
	}

	if (ret == SUCCESS) {
		player->state = state;
		log_trace("[%" PRIXPTR "] change_state::signaling state change to %s",
				(intptr_t )pthread_self(), ap_get_state_name(state));

		if (player->callbacks.on_event) {
			log_trace(
					"[%" PRIXPTR "] change_state::calling state change callback",
					(intptr_t )pthread_self());
			player->callbacks.on_event(player, EVENT_STATE_CHANGE, old_state,
					state);
		}
	} else {
		log_error("invalid state change: %s -> %s",
				ap_get_state_name(old_state), ap_get_state_name(state));
	}

	END_LOCK(player);
	log_trace("change_state::finished with state: %s",
			ap_get_state_name(player->state));
	return ret;
}

/* open a given stream. Return 0 if OK */
static int stream_component_open(player_t *player, int stream_index) {
	AVFormatContext *ic = player->ic;
	AVCodecContext *avctx;
	AVCodec *codec;
	int ret = 0;

	log_info("stream_component_open()");

	if (stream_index < 0 || stream_index >= ic->nb_streams)
		return -1;
	avctx = ic->streams[stream_index]->codec;

	/*	opts = filter_codec_opts(codec_opts, avctx->codec_id, ic,
	 ic->streams[stream_index], NULL);*/

	codec = avcodec_find_decoder(avctx->codec_id);
	if (!codec)
		goto end;

	avctx->workaround_bugs = workaround_bugs;
	avctx->idct_algo = idct;
	avctx->skip_idct = skip_idct;
	avctx->skip_loop_filter = skip_loop_filter;
	avctx->error_concealment = error_concealment;

	if (fast)
		avctx->flags2 |= CODEC_FLAG2_FAST;

	if ((ret = avcodec_open2(avctx, codec, NULL)) < 0) {
		ap_print_error("avcodec_open2() failed", ret);
		goto end;
	}
	if (player->abort_call)
		return FAILURE;
	/* prepare audio output */
	if (avctx->codec_type == AVMEDIA_TYPE_AUDIO) {
		player->sdl_sample_rate = avctx->sample_rate;

		if (!avctx->channel_layout)
			avctx->channel_layout = av_get_default_channel_layout(
					avctx->channels);

		if (!avctx->channel_layout) {
			log_error("unable to guess channel layout");
			ret = AVERROR_INVALIDDATA;
			goto end;
		}

		if (avctx->channels == 1)
			player->sdl_channel_layout = AV_CH_LAYOUT_MONO;
		else
			player->sdl_channel_layout = AV_CH_LAYOUT_STEREO;

		player->sdl_channels = av_get_channel_layout_nb_channels(
				player->sdl_channel_layout);

		int sampleRate = player->sdl_sample_rate;
		int channelFormat = player->sdl_channels;

		if (player->callbacks.on_prepare(player, AV_SAMPLE_FMT_S16, sampleRate,
				channelFormat) < 0) {
			log_error("on_prepare() failed");
			ret = AVERROR_UNKNOWN;
			goto end;
		}

		player->sdl_sample_fmt = AV_SAMPLE_FMT_S16;
		player->resample_sample_fmt = player->sdl_sample_fmt;
		player->resample_channel_layout = avctx->channel_layout;
		player->resample_sample_rate = player->sdl_sample_rate;
		log_trace("stream_component_open::resample_sample_rate: %d",
				player->sdl_sample_rate);
	}

	ic->streams[stream_index]->discard = AVDISCARD_DEFAULT;

	player->audio_stream = stream_index;
	player->audio_st = ic->streams[stream_index];
	player->audio_buf_size = 0;
	player->audio_buf_index = 0;

	memset(&pkt, 0, sizeof(pkt));

	end:

	return ret;
}

void stream_component_close(player_t *player, int stream_index) {
	log_info("stream_component_close() index:%d", stream_index);
	AVFormatContext *ic = player->ic;

	BEGIN_LOCK(player);

	av_free_packet(&pkt);

	if (avr) {
		avresample_free(&avr);
	}

	if (audio_buf) {
		av_freep(&audio_buf);
	}

	av_frame_free(&frame);

	player->audio_st->discard = AVDISCARD_ALL;

	avcodec_close(player->audio_st->codec);
	avformat_close_input(&player->ic);

	END_LOCK(player);
	log_trace("stream_component_close::done");
}

/* decode one audio frame and returns its uncompressed size */
static int audio_decode_frame(player_t *player) {
	/*AVPacket *pkt_temp = &player->audio_pkt_temp;
	 */

//log_info("audio_decode_frame()");
	AVCodecContext *dec = player->audio_st->codec;
	int n, len1, data_size, got_frame;

	for (;;) {
		/* NOTE: the audio packet can contain several frames */

		//log_debug("top_loop");usleep(100000);
		while (pkt.size > 0) {
			int resample_changed, audio_resample;

			if (!frame) {
				if (!(frame = av_frame_alloc()))
					return AVERROR(ENOMEM);
			}
			if (player->abort_call)
				return FAILURE;
			len1 = avcodec_decode_audio4(dec, frame, &got_frame, &pkt);
			if (len1 < 0) {
				/* if error, we skip the frame */
				ap_print_error("avcodec_decode_audio4()", len1);
				pkt.size = 0;

			} else {
				//log_trace("avcodec_decode_audio4 returned %d",len1);
			}

      if (player->audio_st->event_flags)
			log_info("read something: %d",player->audio_st->event_flags);

			pkt.data += len1;
			pkt.size -= len1;

			if (!got_frame) {
				/* stop sending empty packets if the decoder is finished */
				/*	if (!pkt.data
				 && (dec->codec->capabilities & CODEC_CAP_DELAY)){
				 return 0;
				 }
				 */
				return 0;

			}
			data_size = av_samples_get_buffer_size(NULL, dec->channels,
					frame->nb_samples, frame->format, 1);

			audio_resample = frame->format != player->sdl_sample_fmt
					|| frame->channel_layout != player->sdl_channel_layout
					|| frame->sample_rate != player->sdl_sample_rate;

			resample_changed = frame->format != player->resample_sample_fmt
					|| frame->channel_layout != player->resample_channel_layout
					|| frame->sample_rate != player->resample_sample_rate;

			if ((!avr && audio_resample) || resample_changed) {
				int ret;
				if (avr)
					avresample_close(avr);
				else if (audio_resample) {
					avr = avresample_alloc_context();
					if (!avr) {
						fprintf(stderr,
								"error allocating AVAudioResampleContext\n");
						break;
					}
				}
				if (audio_resample) {
					av_opt_set_int(avr, "in_channel_layout",
							frame->channel_layout, 0);
					av_opt_set_int(avr, "in_sample_fmt", frame->format, 0);
					av_opt_set_int(avr, "in_sample_rate", frame->sample_rate,
							0);
					av_opt_set_int(avr, "out_channel_layout",
							player->sdl_channel_layout, 0);
					av_opt_set_int(avr, "out_sample_fmt",
							player->sdl_sample_fmt, 0);
					av_opt_set_int(avr, "out_sample_rate",
							player->sdl_sample_rate, 0);

					if ((ret = avresample_open(avr)) < 0) {
						fprintf(stderr, "error initializing libavresample\n");
						break;
					}
				}
				player->resample_sample_fmt = frame->format;
				player->resample_channel_layout = frame->channel_layout;
				player->resample_sample_rate = frame->sample_rate;
			}

			uint8_t *play_buf = NULL;

			if (audio_resample) {
				void *tmp_out;
				int out_samples, out_size, out_linesize;
				int osize = av_get_bytes_per_sample(player->sdl_sample_fmt);
				int nb_samples = frame->nb_samples;

				out_size = av_samples_get_buffer_size(&out_linesize,
						player->sdl_channels, nb_samples,
						player->sdl_sample_fmt, 0);
				tmp_out = av_realloc(audio_buf, out_size);
				if (!tmp_out)
					return AVERROR(ENOMEM);
				audio_buf = tmp_out;
				play_buf = audio_buf;
				out_samples = avresample_convert(avr, &play_buf, out_linesize,
						nb_samples, frame->data, frame->linesize[0],
						frame->nb_samples);
				if (out_samples < 0) {
					ap_print_error("avresample_convert() failed", out_samples);
					break;
				}
				data_size = out_samples * osize * player->sdl_channels;

			} else {
				play_buf = frame->data[0];
			}

			/* if no pts, then compute it */
			/*pts = player->audio_clock;
			 *pts_ptr = pts;*/
			n = player->sdl_channels
					* av_get_bytes_per_sample(player->sdl_sample_fmt);
			player->audio_clock += (double) data_size
					/ (double) (n * player->sdl_sample_rate);

			if (pkt.pts != AV_NOPTS_VALUE) {
				player->audio_clock = av_q2d(player->audio_st->time_base)
						* pkt.pts;
			}
			if (player->abort_call)
				return FAILURE;
			player->callbacks.on_play(player, (char*) play_buf, data_size);

#ifdef DEBUG
			{
				static double last_clock;
				printf("audio: delay=%0.3f clock=%0.3f pts=%0.3f\n",
						player->audio_clock - last_clock,
						player->audio_clock, pts);
				last_clock = player->audio_clock;
			}
#endif
			return data_size;
		}

		/* free the current packet */
		if (pkt.data)
			av_free_packet(&pkt);
		memset(&pkt, 0, sizeof(pkt));

		if (player->state == STATE_PAUSED) {
			log_trace("audio_decode_frame::exiting");
			return -1;
		}

		/* if update the audio clock with the pts */
		if (pkt.pts != AV_NOPTS_VALUE) {
			player->audio_clock = av_q2d(player->audio_st->time_base) * pkt.pts;
		}
	}

	return 0;
}

static int cmd_prepare(player_t *player) {
	log_info("cmd_prepare(): %s in state: %s", player->url,ap_get_state_name(player->state));
	int i, ret;
	if (change_state(player, STATE_PREPARING) != SUCCESS){
		log_error("cmd_prepare::failed to change to preparing");
		return FAILURE;
	}

	log_debug("cmd_prepare::1");

	if (player->ic) {
		log_trace("cmd_prepare::avformat_close_input(&player->ic);");
		avformat_close_input(&player->ic);
	}

	//AVDictionary *options = NULL;
	//av_dict_set(&options, "user-agent", "This is my user-agent!", 0);


	log_debug("cmd_prepare::avformat_open_input() %s", player->url);
	//ret = avformat_open_input(&player->ic, player->url, NULL, &options);
	ret = avformat_open_input(&player->ic, player->url, NULL, NULL);
	log_debug("cmd_prepare:avformat_open_input returned");

	//av_dict_free(&options);

	if (ret < 0) {
		ap_print_error("cmd_prepare::avformat_open_input failed: ", ret);
		return FAILURE;
	}



	player->ic->interrupt_callback.opaque = player;
	player->ic->interrupt_callback.callback = (void*) decode_interrupt_cb;

	if (genpts)
		player->ic->flags |= AVFMT_FLAG_GENPTS;
	log_debug("cmd_prepare::avformat_find_stream_info()");
	ret = avformat_find_stream_info(player->ic, NULL);
	log_debug("cmd_prepare:avformat_find_stream_info retured");

	if (ret < 0) {
		ap_print_error("cmd_prepare::avformat_find_stream_info failed", ret);
		return FAILURE;
	}

	if (player->ic->pb)
		player->ic->pb->eof_reached = 0;

	player->audio_stream = -1;

	for (i = 0; i < player->ic->nb_streams; i++)
		player->ic->streams[i]->discard = AVDISCARD_ALL;

	st_index[AVMEDIA_TYPE_AUDIO] = av_find_best_stream(player->ic,
			AVMEDIA_TYPE_AUDIO, wanted_stream[AVMEDIA_TYPE_AUDIO],
			st_index[AVMEDIA_TYPE_VIDEO],
			NULL, 0);

	/* open the streams */
	if (st_index[AVMEDIA_TYPE_AUDIO] >= 0) {
		stream_component_open(player, st_index[AVMEDIA_TYPE_AUDIO]);
	}

	if (player->audio_stream < 0) {
		log_error("cmd_prepare::%s: could not open codecs", player->url);
		return FAILURE;
	}

	log_debug("cmd_prepare:: stream opened .. reading metadata..");

	av_dump_format(player->ic, 0, player->url, 0);
	AVDictionaryEntry *entry = NULL;
	while ((entry = av_dict_get(player->ic->metadata, "", entry,
	AV_DICT_IGNORE_SUFFIX)))
		log_debug("metadata:\t%s:%s", entry->key, entry->value);

	return change_state(player, STATE_PREPARED);
}

static int cmd_reset(player_t *player) {
	log_info("cmd_reset(): %s", player->url);

	if (player->state == STATE_IDLE) {
		return SUCCESS;
	}

	if (player->state != STATE_END)
		change_state(player, STATE_IDLE);

	if (player->audio_st && player->audio_st->codec) {
		log_trace("avcodec_close(player->audio_st->codec)");
		avcodec_close(player->audio_st->codec);
	}

	if (avr) {
		log_trace("cmd_reset::avresample_free()");
		avresample_free(&avr);
		avr = NULL;
	}

	if (frame) {
		log_trace("cmd_reset::av_frame_free()");
		av_frame_free(&frame);
		frame = NULL;
	}

	if (player->ic) {
		log_trace("cmd_reset::avformat_close_input(&player->ic)");
		avformat_close_input(&player->ic);
	}

	player->audio_stream = -1;
	player->audio_st = NULL;
	epoll_timeout = -1;

	player->abort_call = 0;

	log_trace("cmd_reset::done");
	return SUCCESS;
}

static int cmd_pause(player_t *player) {
	log_info("cmd_pause(): %s", player->url);
	int ret = FAILURE;
	if (player->state == STATE_STARTED) {
		log_trace("ret = av_read_pause(player->ic)");
		ret = av_read_pause(player->ic);
		epoll_timeout = -1; //block when waiting for next event
		ret = change_state(player, STATE_PAUSED);
	}
	return ret;
}

static int cmd_start(player_t *player) {
	log_info("cmd_start(): %s", player->url);
	int ret = FAILURE;

	if (player->state == STATE_STARTED)
		return SUCCESS;

	if (player->state == STATE_STOPPED) {
		cmd_reset(player);
		return cmd_prepare(player);
	}

	if (player->state == STATE_PAUSED) {
		log_trace("av_read_play(player->ic)");
		ret = av_read_play(player->ic);
	}

	if ((ret = change_state(player, STATE_STARTED)) == SUCCESS) {
		epoll_timeout = 0; //dont block when waiting for events
	}
	return ret;
}

static int cmd_stop(player_t *player) {
	log_info("cmd_stop()");
	epoll_timeout = -1; //block when waiting for events
	return change_state(player, STATE_STOPPED);
}

static int cmd_seek(player_t *player) {
	log_trace("cmd_seek()");
	int ret = FAILURE;
	int64_t seek_target = player->seek_pos;
	int64_t seek_min =
			player->seek_rel > 0 ? seek_target - player->seek_rel + 2 :
			INT64_MIN;
	int64_t seek_max =
			player->seek_rel < 0 ? seek_target - player->seek_rel - 2 :
			INT64_MAX;
// FIXME the +-2 is due to rounding being not done in the correct direction in generation
//      of the seek_pos/seek_rel variables

	log_trace("cmd_seek::avformat_seek_file()");
	ret = avformat_seek_file(player->ic, -1, seek_min, seek_target, seek_max,
			player->seek_flags);
	player->seek_req = 0;

	if (player->abort_call)
		return -1;

	if (ret < 0) {
		ap_print_error("cmd_seek::error in seek", ret);
	} else if (player->callbacks.on_event) {
		player->callbacks.on_event(player, EVENT_SEEK_COMPLETE, 0, 0);
	}

	return ret;
}

static int cmd_set_datasource(player_t *player) {

	log_info("cmd_set_datasource(): %s", player->url);
	if (player->state != STATE_IDLE) {
		log_error("cmd_set_datasource::invalid state: %s",
				ap_get_state_name(player->state));
		return FAILURE;
	}
	return change_state(player, STATE_INITIALIZED);
}

static int cmd_test(player_t *player) {
	log_info("cmd_test()");

}
/* this thread gets the stream from the disk or the network */
int player_thread(player_t *player) {

	log_debug("[%" PRIXPTR "] player_thread()", (intptr_t )pthread_self());

	int ret, i;

	const int MAX_EVENTS = 8;
	struct epoll_event event;
	struct epoll_event events[MAX_EVENTS];
	int efd = 0;

	memset(st_index, -1, sizeof(st_index));
	memset(&events, 0, sizeof(events));
	player->audio_stream = -1;
	av_init_packet(&pkt);

	pkt.data = NULL;
	pkt.size = 0;

	AP_EVENT(player, EVENT_THREAD_START, 0, 0);

	log_trace("player_thread::avformat_alloc_context()");
	player->ic = avformat_alloc_context();
	if (!player->ic) {
		av_log(NULL, AV_LOG_FATAL, "Could not allocate context.\n");
		ret = AVERROR(ENOMEM);
		return -1;
	}

	player->ic->interrupt_callback.opaque = player;
	player->ic->interrupt_callback.callback = (void*) decode_interrupt_cb;

	efd = epoll_create(MAX_EVENTS);
	log_trace("efd: %d", efd);

	memset(&event, 0, sizeof(struct epoll_event));
	event.events = EPOLLIN;
	int pipe_fd = player->pipe[0];
	event.data.fd = pipe_fd;

	if ((ret = epoll_ctl(efd, EPOLL_CTL_ADD, pipe_fd, &event)) < 0) {
		log_error("epoll set insertion error: fd=%d0: %s", pipe_fd,
				strerror(errno));
		goto end;
	}

	log_trace("player_thread::starting loop");
	int quit = 0;

	while (!quit) {
		int nfds = epoll_wait(efd, events, MAX_EVENTS, epoll_timeout);

		if (nfds < 0) {
			log_error("nfds < 0");
			break;
		}

		for (i = 0; i < nfds; i++) {
			if (events[i].data.fd == pipe_fd) {
				int cmd = 0;
				ret = read(pipe_fd, &cmd, sizeof(cmd));
				log_trace("player_thread::received cmd: %s in state: %s",
						ap_get_cmd_name(cmd), ap_get_state_name(player->state));
				switch (cmd) {
				case CMD_PREPARE:
					eof = 0;
					cmd_prepare(player);
					break;
				case CMD_START:
					cmd_start(player);
					break;
				case CMD_PAUSE:
					cmd_pause(player);
					break;
				case CMD_STOP:
					cmd_stop(player);
					break;
				case CMD_SEEK:
					cmd_seek(player);
					break;
				case CMD_RESET:
					cmd_reset(player);
					break;
				case CMD_SET_DATASOURCE:
					cmd_set_datasource(player);
					break;
				case CMD_EXIT:
					quit = 1;
					continue;
				default:
					log_error("player_thread::invalid command: %d", cmd);
					break;
				}
			}
		}

		if (player->state != STATE_STARTED)
			continue;

		//log_trace("player_thread::av_read_frame()");
		ret = av_read_frame(player->ic, &pkt);

		if (ret < 0) {
			ap_print_error("player_thread::av_read_frame failed", ret);
			if (ret == AVERROR_EOF
					|| (player->ic->pb && player->ic->pb->eof_reached)) {
				log_trace("player_thread::eof == 1");
				eof = 1;

				if (player->looping) {
					eof = 0;
					ap_seek(player, 0, 0);
					continue;
				}
			} else {
				continue;
			}
		}

		if (eof) {
			log_trace("player_thread::eof");
			if (player->audio_stream >= 0
					&& (player->audio_st->codec->codec->capabilities
							& CODEC_CAP_DELAY)) {
				av_init_packet(&pkt);
				pkt.data = NULL;
				pkt.size = 0;
				pkt.stream_index = player->audio_stream;
			}
			change_state(player, STATE_COMPLETED);
			epoll_timeout = -1;
			continue;
		}

		if (pkt.stream_index == player->audio_stream) {
			audio_decode_frame(player);
		}

		av_free_packet(&pkt);

	}
	ret = SUCCESS;
	end:
	log_info("read_loop::finished  state: %s eof: %d  ret: %d looping: %d",
			ap_get_state_name(player->state), eof, ret, player->looping);

	change_state(player, STATE_END);

	if (player->audio_st && player->audio_st->codec) {
		avcodec_close(player->audio_st->codec);
	}

	if (audio_buf) {
		log_warn("read_loop::av_freep(&audio_buf);");
		av_freep(&audio_buf);
	}

	if (avr) {
		log_warn("read_loop::avresample_free()");
		avresample_free(&avr);
	}

	av_free_packet(&pkt);

	if (frame) {
		log_warn("read_loop::av_frame_free()");
		av_frame_free(&frame);
	}

	if (player->ic) {
		log_warn("avformat_close_input(&player->ic)");
		avformat_close_input(&player->ic);
	}

	pthread_mutex_destroy(&player->mutex);

	close(player->pipe[0]);
	close(player->pipe[1]);

	log_warn("read_loop::done");
	pthread_exit(0);
	return ret;
}

