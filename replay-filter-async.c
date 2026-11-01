#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>
#include <media-io/video-io.h>
#include <media-io/video-frame.h>
#include <media-io/audio-resampler.h>
#include <math.h>
#include "replay.h"

#ifndef SETTING_THRESHOLD
#define SETTING_THRESHOLD "threshold"
#endif

#ifndef SETTING_THRESHOLD_MIN
#define SETTING_THRESHOLD_MIN -60.0
#endif

#ifndef SETTING_INTERNAL_FRAMES
#define SETTING_INTERNAL_FRAMES "internal_frames"
#endif

static const char *replay_filter_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("ReplayFilterAsync");
}

static void replay_filter_update(void *data, obs_data_t *settings)
{
	struct replay_filter *filter = data;
	uint64_t new_duration = (uint64_t)obs_data_get_int(settings, SETTING_DURATION) * MSEC_TO_NSEC;
	if (new_duration < filter->duration) {
		pthread_mutex_lock(&filter->mutex);
		free_video_data(filter);
		pthread_mutex_unlock(&filter->mutex);
	}
	filter->duration = new_duration;
	filter->internal_frames = false;
	
	double threshold_db = obs_data_get_double(settings, SETTING_THRESHOLD);
	filter->threshold = powf(10.0f, (float)threshold_db / 20.0f);
	
	replay_filter_check(filter);
	obs_source_t *s = obs_get_source_by_name(obs_source_get_name(filter->src));
	if (s) {
		if (obs_data_get_bool(settings, SETTING_SOUND_TRIGGER) && !filter->trigger_threshold) {
			filter->threshold_data = obs_obj_get_data(s);
			filter->trigger_threshold = replay_trigger_threshold;
		}
		obs_source_release(s);
	} else {
		obs_source_t *parent = obs_filter_get_parent(filter->src);
		if (parent)
			obs_source_filter_remove(parent, filter->src);
	}
}

static void *replay_filter_create(obs_data_t *settings, obs_source_t *source)
{
	struct replay_filter *context = bzalloc(sizeof(struct replay_filter));
	context->src = source;
	pthread_mutex_init(&context->mutex, NULL);
	obs_get_video_info(&context->ovi);
	context->last_check = obs_get_video_frame_time();

	replay_filter_update(context, settings);

	return context;
}

static void replay_filter_destroy(void *data)
{
	struct replay_filter *filter = data;

	pthread_mutex_lock(&filter->mutex);
	free_video_data(filter);
	free_audio_data(filter);
	pthread_mutex_unlock(&filter->mutex);
	circlebuf_free(&filter->video_frames);
	circlebuf_free(&filter->audio_frames);
	pthread_mutex_destroy(&filter->mutex);

	bfree(data);
}

static void StoreFrame(struct replay_filter *filter, struct obs_source_frame *frame)
{
	if (!frame || !frame->width || !frame->height)
		return;

	struct obs_source_frame *output;
	const uint64_t timestamp = frame->timestamp;
	uint64_t adjusted_time = timestamp + filter->timing_adjust;
	const uint64_t os_time = os_gettime_ns();
	uint64_t diff = (os_time < timestamp) ? (timestamp - os_time) : (os_time - timestamp);
	uint64_t adj_diff = (os_time < adjusted_time) ? (adjusted_time - os_time) : (os_time - adjusted_time);

	if (filter->timing_adjust && diff < MAX_TS_VAR) {
		adjusted_time = timestamp;
		filter->timing_adjust = 0;
	} else if (adj_diff > MAX_TS_VAR) {
		filter->timing_adjust = os_time - timestamp;
		adjusted_time = os_time;
	}

	struct obs_source_frame *new_frame = obs_source_frame_create(frame->format, frame->width, frame->height);
	obs_source_frame_copy(new_frame, frame);
	new_frame->timestamp = adjusted_time;
	new_frame->refs = 1;

	pthread_mutex_lock(&filter->mutex);
	circlebuf_push_back(&filter->video_frames, &new_frame, sizeof(struct obs_source_frame *));
	circlebuf_peek_front(&filter->video_frames, &output, sizeof(struct obs_source_frame *));
	uint64_t cur_duration = adjusted_time - output->timestamp;
	while (cur_duration > 0 && cur_duration > filter->duration) {
		circlebuf_pop_front(&filter->video_frames, NULL, sizeof(struct obs_source_frame *));
		if (os_atomic_dec_long(&output->refs) <= 0) {
			obs_source_frame_destroy(output);
			output = NULL;
		}
		if (filter->video_frames.size) {
			circlebuf_peek_front(&filter->video_frames, &output, sizeof(struct obs_source_frame *));
			cur_duration = adjusted_time - output->timestamp;
		} else {
			cur_duration = 0;
		}
	}
	pthread_mutex_unlock(&filter->mutex);
}

static struct obs_source_frame *replay_filter_video(void *data, struct obs_source_frame *frame)
{
	struct replay_filter *filter = data;
	if (frame) {
		StoreFrame(filter, frame);
	}
	replay_filter_check(data);
	return frame;
}

static obs_properties_t *replay_filter_properties(void *unused)
{
	UNUSED_PARAMETER(unused);
	obs_properties_t *props = obs_properties_create();
	obs_property_t *prop = obs_properties_add_int(props, SETTING_DURATION, obs_module_text("Duration"),
						      SETTING_DURATION_MIN, SETTING_DURATION_MAX, 1000);
	obs_property_int_set_suffix(prop, "ms");
	obs_properties_add_bool(props, SETTING_INTERNAL_FRAMES, obs_module_text("CaptureInternalFrames"));
	obs_properties_add_bool(props, SETTING_SOUND_TRIGGER, obs_module_text("SoundTrigger"));
	prop = obs_properties_add_float_slider(props, SETTING_THRESHOLD, obs_module_text("ThresholdDb"),
					       SETTING_THRESHOLD_MIN, 0.0, 0.1);
	obs_property_float_set_suffix(prop, "db");
	return props;
}

static void replay_filter_remove(void *data, obs_source_t *parent)
{
	UNUSED_PARAMETER(parent);
	struct replay_filter *filter = data;
	pthread_mutex_lock(&filter->mutex);
	free_video_data(filter);
	free_audio_data(filter);
	pthread_mutex_unlock(&filter->mutex);
}

static void replay_filter_tick(void *data, float seconds)
{
	UNUSED_PARAMETER(seconds);
	replay_filter_check(data);
}

struct obs_source_info replay_filter_async_info = {
	.id = REPLAY_FILTER_ASYNC_ID,
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_ASYNC,
	.create = replay_filter_create,
	.destroy = replay_filter_destroy,
	.update = replay_filter_update,
	.load = replay_filter_update,
	.get_name = replay_filter_get_name,
	.get_properties = replay_filter_properties,
	.filter_video = replay_filter_video,
	.filter_audio = replay_filter_audio,
	.filter_remove = replay_filter_remove,
	.video_tick = replay_filter_tick,
};
