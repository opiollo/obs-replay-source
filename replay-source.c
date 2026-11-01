#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>
#include <media-io/video-io.h>
#include <media-io/video-frame.h>
#include <media-io/audio-resampler.h>
#include "replay.h"

static const char *replay_source_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("ReplaySource");
}

static void replay_source_update(void *data, obs_data_t *settings)
{
	struct replay_filter *filter = data;
	UNUSED_PARAMETER(filter);
	UNUSED_PARAMETER(settings);
}

static void *replay_source_create(obs_data_t *settings, obs_source_t *source)
{
	struct replay_filter *context = bzalloc(sizeof(struct replay_filter));
	context->src = source;
	pthread_mutex_init(&context->mutex, NULL);
	replay_source_update(context, settings);
	return context;
}

static void replay_source_destroy(void *data)
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

static void replay_source_video_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct replay_filter *filter = data;
	pthread_mutex_lock(&filter->mutex);
	if (filter->video_frames.size) {
		struct obs_source_frame *frame;
		circlebuf_peek_front(&filter->video_frames, &frame, sizeof(struct obs_source_frame *));
		if (frame && frame->data[0]) {
			gs_texture_t *tex = gs_texture_create(frame->width, frame->height, GS_BGRA, 1, (const uint8_t **)&frame->data[0], GS_DYNAMIC);
			if (tex) {
				obs_source_draw(tex, 0, 0, 0, 0, false);
				gs_texture_destroy(tex);
			}
		}
	}
	pthread_mutex_unlock(&filter->mutex);
}

static obs_properties_t *replay_source_properties(void *unused)
{
	UNUSED_PARAMETER(unused);
	obs_properties_t *props = obs_properties_create();
	obs_property_t *prop = obs_properties_add_text(props, "video_source", obs_module_text("VideoSource"), OBS_TEXT_DEFAULT);
	return props;
}

struct obs_source_info replay_source_info = {
	.id = REPLAY_SOURCE_ID,
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_CUSTOM_DRAW,
	.create = replay_source_create,
	.destroy = replay_source_destroy,
	.update = replay_source_update,
	.get_name = replay_source_get_name,
	.get_properties = replay_source_properties,
	.video_render = replay_source_video_render,
};
