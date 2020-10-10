/*
    SDL - Simple DirectMedia Layer
    Copyright (C) 1997-2012 Sam Lantinga

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA

    Sam Lantinga
    slouken@libsdl.org
*/
#include "SDL_config.h"
#include "SDL_stdinc.h"

#include <sys/types.h>
#include <termios.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <libdrm/drm.h>
#include <libdrm/drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#ifndef _SDL_kmsdrmvideo_h
#define _SDL_kmsdrmvideo_h

#ifdef ENABLE_KMSDRM_DEBUG
#define kmsdrm_dbg_printf(fmt, ...) printf(fmt, ##__VA_ARGS__)
#else
#define kmsdrm_dbg_printf(fmt, ...)
#endif

#include "../SDL_sysvideo.h"

/* Default refresh rate. Can be set with the environment variable SDL_VIDEO_REFRESHRATE */
#define KMSDRM_DEFAULT_REFRESHRATE 60

/* Hidden "this" pointer for the video functions */
#define _THIS	SDL_VideoDevice *this

/* Private display data */
typedef struct drm_prop_storage {
    drmModeObjectProperties *props;
	drmModePropertyRes **props_info;
    Uint32 obj_id;
    Uint32 obj_type;
    struct drm_prop_storage *next;
} drm_prop_storage;

typedef struct drm_pipe {
    /* fb -> plane -> crtc -> encoder -> connector */
    Uint32 framebuffer;
    Uint32 plane;
    Uint32 crtc;
    Uint32 encoder;
    Uint32 connector;
    drmModeModeInfo *modes;
    Uint32 mode_count;
    Uint32 factor_w, factor_h;
    struct drm_pipe *next;
} drm_pipe;

typedef struct drm_prop_arg {
	Uint32 obj_id;
	Uint32 obj_type;
	char name[DRM_PROP_NAME_LEN+1];
	Uint32 prop_id;
	Uint32 prop_drm_id;
	Uint64 value;
	int optional;
} drm_prop_arg;

typedef struct drm_buffer {
    struct drm_mode_destroy_dumb req_destroy_dumb;
    struct drm_mode_create_dumb req_create;
    struct drm_mode_map_dumb req_map;
    Uint32 buf_id;
    void *map;
} drm_buffer;

typedef struct drm_input_dev {
	char *path;
	int fd;
	struct drm_input_dev *next;
} drm_input_dev;

typedef enum {
	DRM_SCALING_MODE_FULLSCREEN,
	DRM_SCALING_MODE_ASPECT_RATIO,
	DRM_SCALING_MODE_INTEGER_SCALED,
	DRM_SCALING_MODE_END,
} drm_scaling_mode;

struct SDL_PrivateVideoData {
    SDL_Rect **vid_modes;
    int vid_mode_count;

    int fd;
	Uint32 size;
	Uint32 handle;
	void *map;

    drm_pipe *first_pipe;
    drm_pipe *active_pipe;
    drm_prop_storage *first_prop_store;
    drmModeAtomicReqPtr drm_req;
    drm_buffer buffers[3];
    Uint32 mode_blob_id;
    Uint32 front_buffer;
    Uint32 back_buffer;
    Uint32 queued_buffer;
    struct drm_color_lut palette[256];
    Uint32 drm_gamma_lut_blob_id;

	SDL_mutex *triplebuf_mutex;
	SDL_cond *triplebuf_cond;
	SDL_Thread *triplebuf_thread;
	int triplebuf_thread_stop;

    drm_input_dev *keyboards, *mice;
    drm_scaling_mode scaling_mode;

    int w, h, crtc_w, crtc_h;
    int bpp;
    int has_damage_clips;
};

#define drm_vid_modes        (this->hidden->vid_modes)
#define drm_vid_mode_count   (this->hidden->vid_mode_count)
#define drm_fd               (this->hidden->fd)
#define drm_size             (this->hidden->size)
#define drm_handle           (this->hidden->handle)
#define drm_map              (this->hidden->map)
#define drm_first_pipe       (this->hidden->first_pipe)
#define drm_first_prop_store (this->hidden->first_prop_store)
#define drm_buffers          (this->hidden->buffers)
#define drm_mode_blob_id     (this->hidden->mode_blob_id)
#define drm_front_buffer     (this->hidden->front_buffer)
#define drm_back_buffer      (this->hidden->back_buffer)
#define drm_queued_buffer    (this->hidden->queued_buffer)
#define drm_palette          (this->hidden->palette)
#define drm_palette_blob_id  (this->hidden->drm_gamma_lut_blob_id)
#define drm_active_pipe      (this->hidden->active_pipe)
#define drm_req_destroy_dumb (this->hidden->req_destroy_dumb)
#define drm_req_create       (this->hidden->req_create)
#define drm_req_map          (this->hidden->req_map)
#define drm_triplebuf_mutex  (this->hidden->triplebuf_mutex)
#define drm_triplebuf_cond   (this->hidden->triplebuf_cond)
#define drm_triplebuf_thread (this->hidden->triplebuf_thread)
#define drm_triplebuf_thread_stop (this->hidden->triplebuf_thread_stop)

#endif /* _SDL_kmsdrmvideo_h */
