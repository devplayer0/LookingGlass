#include "lg-renderer.h"
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <malloc.h>

#include "debug.h"
#include "utils.h"
#include "lg-decoders.h"

struct Inst {
	LG_RendererParams	params;

	LG_Lock				format_lock;
	LG_RendererFormat	format;
	bool				configured;
	bool				reconfigure;
	const LG_Decoder	*decoder;
	void				*decoder_data;
	size_t				tex_size;

	LG_Lock				sync_lock;
	bool				frame_update;
};

static void deconfigure(struct Inst *this) {
	if (!this->configured) {
		return;
	}

	if (this->decoder_data) {
		this->decoder->destroy(this->decoder_data);
		this->decoder_data = NULL;
	}

	this->configured = false;
}
static bool configure(struct Inst *this, SDL_Window *window) {
	LG_LOCK(this->format_lock);
	if (!this->reconfigure) {
		LG_UNLOCK(this->format_lock);
		return this->configured;
	}

	if (this->configured) {
		deconfigure(this);
	}

	switch(this->format.type) {
		case FRAME_TYPE_ARGB:
			this->decoder = &LGD_NULL;
			break;
		case FRAME_TYPE_YUV420:
			this->decoder = &LGD_YUV420;
			break;
		default:
			DEBUG_ERROR("unknown/unsupported compression type");
			return false;
	}

	DEBUG_INFO("using decoder: %s", this->decoder->name);

	if (!this->decoder->create(&this->decoder_data)) {
		DEBUG_ERROR("failed to create the decoder");
		return false;
	}

	if (!this->decoder->initialize(this->decoder_data, this->format, window)) {
		DEBUG_ERROR("failed to initialize decoder");
		return false;
	}

	switch(this->decoder->get_out_format(this->decoder_data)) {
	case LG_OUTPUT_BGRA:
		//this->intFormat = GL_RGBA8;
		//this->vboFormat = GL_BGRA;
		break;
	case LG_OUTPUT_YUV420:
		// fixme
		//this->intFormat = GL_RGBA8;
		//this->vboFormat = GL_BGRA;
		break;
	default:
		DEBUG_ERROR("format not supported");
		LG_UNLOCK(this->format_lock);
		return false;
	}

	// calculate the texture size in bytes
	this->tex_size = this->format.height * this->decoder->get_frame_pitch(this->decoder_data);

	this->configured = true;
	this->reconfigure = false;

	LG_UNLOCK(this->format_lock);
	return true;
}

const char *stdout_get_name() {
	return "stdout";
}
bool stdout_create(void **opaque, const LG_RendererParams params) {
	// create our local storage
	*opaque = malloc(sizeof(struct Inst));
	if (!*opaque) {
		DEBUG_INFO("Failed to allocate %lu bytes", sizeof(struct Inst));
		return false;
	}
	memset(*opaque, 0, sizeof(struct Inst));

    struct Inst * this = (struct Inst *)*opaque;
    memcpy(&this->params, &params, sizeof(LG_RendererParams));

	LG_LOCK_INIT(this->format_lock);
	LG_LOCK_INIT(this->sync_lock);

	return true;
}
bool stdout_initialize(void *opaque, Uint32 *sdl_flags) {
	struct Inst *this = (struct Inst *)opaque;
	if (!this) {
		return false;
	}

	return true;
}
void stdout_deinitialize(void *opaque) {
	struct Inst *this = (struct Inst *)opaque;
	if (!this) {
		return;
	}

	LG_LOCK_FREE(this->sync_lock);
	LG_LOCK_FREE(this->format_lock);

	deconfigure(this);
	free(this);
}
void stdout_on_resize(void *opaque, const int width, const int height, const LG_RendererRect dest_rect) {
	DEBUG_INFO("new render window size: %dx%d", width, height);
}
bool stdout_on_mouse_shape(void *opaque, const LG_RendererCursor cursor, const int width, const int height, const int pitch, const uint8_t * data) {
	return true;
}
bool stdout_on_mouse_event(void *opaque, const bool visible, const int x, const int y) {
	return true;
}
bool stdout_on_frame_event(void *opaque, const LG_RendererFormat format, const uint8_t *data) {
	struct Inst *this = (struct Inst *)opaque;
	if (!this) {
		DEBUG_ERROR("invalid opaque pointer");
		return false;
	}

	LG_LOCK(this->format_lock);
	if (this->reconfigure) {
		LG_UNLOCK(this->format_lock);
		return true;
	}

	if (!this->configured ||
			this->format.type	!= format.type		||
			this->format.width	!= format.width		||
			this->format.height	!= format.height	||
			this->format.stride	!= format.stride	||
			this->format.bpp	!= format.bpp) {
		memcpy(&this->format, &format, sizeof(LG_RendererFormat));
		this->reconfigure = true;
		LG_UNLOCK(this->format_lock);
		return true;
	}
	LG_UNLOCK(this->format_lock);

	LG_LOCK(this->sync_lock);
	if (!this->decoder->decode(this->decoder_data, data, format.pitch)) {
		DEBUG_ERROR("decode returned failure");
		LG_UNLOCK(this->sync_lock);
		return false;
	}
	this->frame_update = true;
	LG_UNLOCK(this->sync_lock);

	return true;
}
void stdout_on_alert(void *opaque, const LG_RendererAlert alert, const char *message, bool **close_flag) {
	switch(alert) {
	case LG_ALERT_INFO:
	case LG_ALERT_SUCCESS:
		DEBUG_INFO("alert: %s", message);
		break;
	case LG_ALERT_WARNING:
		DEBUG_WARN("alert: %s", message);
		break;
	case LG_ALERT_ERROR:
		DEBUG_ERROR("alert: %s", message);
		break;
	}
}
bool stdout_render_startup(void * opaque, SDL_Window * window) {
	// we won't be drawing anything!
	SDL_HideWindow(window);
	return true;
}
bool stdout_render(void *opaque, SDL_Window *window) {
	struct Inst * this = (struct Inst *)opaque;
	if (!this) {
		return false;
	}

	configure(this, window);

	LG_LOCK(this->sync_lock);
	if (!this->frame_update) {
		LG_UNLOCK(this->sync_lock);
		return true;
	}
	this->frame_update = false;
	LG_UNLOCK(this->sync_lock);

	LG_LOCK(this->format_lock);
    const uint8_t *data = this->decoder->get_buffer(this->decoder_data);
    if (!data) {
		LG_UNLOCK(this->format_lock);
		DEBUG_ERROR("failed to get the buffer from the decoder");
		return false;
    }

	write(fileno(stdout), data, this->tex_size);
	LG_UNLOCK(this->format_lock);

	return true;
}


const LG_Renderer LGR_stdout = {
  .get_name       = stdout_get_name,
//  .options        = opengl_options,
//  .option_count   = LGR_OPTION_COUNT(opengl_options),
  .create         = stdout_create,
  .initialize     = stdout_initialize,
  .deinitialize   = stdout_deinitialize,
  .on_resize      = stdout_on_resize,
  .on_mouse_shape = stdout_on_mouse_shape,
  .on_mouse_event = stdout_on_mouse_event,
  .on_frame_event = stdout_on_frame_event,
  .on_alert       = stdout_on_alert,
  .render_startup = stdout_render_startup,
  .render         = stdout_render
};
