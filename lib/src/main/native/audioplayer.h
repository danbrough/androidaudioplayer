#ifndef _AUDIOPLAYER_H_
#define _AUDIOPLAYER_H_
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavresample/avresample.h>




#define TRUE 1
#define FALSE 0
#define SUCCESS 0
#define FAILURE -1

#define OUTPUT_SAMPLE_FMT AV_SAMPLE_FMT_S16


#define MAX_QUEUE_SIZE (15  * 1024)
#define MIN_AUDIOQ_SIZE (20 * 16 * 1024)
#define SDL_AUDIO_BUFFER_SIZE 1024

/* NOTE: the size must be big enough to compensate the hardware audio buffersize size */
#define SAMPLE_ARRAY_SIZE (2 * 65536)

typedef enum {
	STATE_IDLE,
	STATE_INITIALIZED,
	STATE_PREPARING,
	STATE_PREPARED,
	STATE_STARTED,
	STATE_PAUSED,
	STATE_COMPLETED,
	STATE_STOPPED,
	STATE_ERROR,
	STATE_END
} audio_state_t;

typedef enum {
	EVENT_THREAD_START = 1, EVENT_STATE_CHANGE, EVENT_SEEK_COMPLETE
} audio_event_t;

typedef enum {
	CMD_SET_DATASOURCE,
	CMD_PREPARE,
	CMD_START,
	CMD_PAUSE,
	CMD_STOP,
	CMD_SEEK,
	CMD_RESET,
	CMD_EXIT
} audio_cmd_t;

const char* ap_get_state_name(audio_state_t state);

typedef struct player_t {
	int looping;
	int abort_call;
	audio_state_t state;
	pthread_mutex_t mutex;
	pthread_t player_thread;
	int seek_req;
	int seek_flags;

	int64_t seek_pos;
	int64_t seek_rel;
	AVFormatContext *ic;

	int audio_stream;
	int pipe[2];

	double audio_clock;

	AVStream *audio_st;

	//int audio_hw_buf_size;
	uint8_t silence_buf[SDL_AUDIO_BUFFER_SIZE];

	unsigned int audio_buf_size; /* in bytes */
	int audio_buf_index; /* in bytes */

	enum AVSampleFormat sdl_sample_fmt;

	uint64_t sdl_channel_layout;
	int sdl_channels;
	int sdl_sample_rate;

	enum AVSampleFormat resample_sample_fmt;
	uint64_t resample_channel_layout;
	int resample_sample_rate;

	int16_t *sample_array[SAMPLE_ARRAY_SIZE];
	int sample_array_index;

	char url[1024];

	struct _player_callbacks_t {

		void (*on_event)(struct player_t *player, audio_event_t event, int arg1,
				int arg2);

		void (*on_play)(struct player_t *player, char *data, int len);

		int (*on_prepare)(struct player_t *player, int sampleFormat,
				int sampleRate, int channelFormat);

	} callbacks;

	void *extra;

} player_t;

typedef struct _player_callbacks_t player_callbacks_t;

typedef void (*on_state_change_t)(player_t *player, audio_state_t old_state,
		audio_state_t new_state);

typedef void (*on_play_t)(player_t *player, char *data, int len);

typedef void (*callback_t)(player_t *player);

typedef int (*on_prepare_t)(struct player_t *player, int sampleFormat,
		int sampleRate, int channelFormat);

//one off initialization of the library
int ap_init();

//one off de-initialization of the library
void ap_uninit();

player_t* ap_create(player_callbacks_t callbacks);

void ap_delete(player_t* player);

double ap_get_audio_clock(player_t *player);

int ap_start(player_t *player);

int ap_stop(player_t *player);

int ap_pause(player_t *player);

int ap_set_datasource(player_t *player, const char* url);

int ap_prepare_async(player_t *player);

int ap_reset(player_t *player);

void ap_seek(player_t *player, int64_t pos, int relative);

void ap_print_metadata(player_t *player);

//duration of current track in ms
int32_t ap_get_duration(player_t *player);

//position in current track in ms
int32_t ap_get_position(player_t *player);

int ap_is_playing(player_t *player);

int ap_is_looping(player_t *player);

void ap_set_looping(player_t *player, int looping);

void ap_print_error(const char* msg, int err);

#define BEGIN_LOCK(player) pthread_mutex_lock(&player->mutex)

#define END_LOCK(player) pthread_mutex_unlock(&player->mutex)

#define AP_EVENT(player,event,arg1,arg2) if (player->callbacks.on_event)\
		player->callbacks.on_event(player,event,arg1,arg2)

#endif //_AUDIOPLAYER_H_
