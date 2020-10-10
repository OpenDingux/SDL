// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * Copyright (C) 2020 João H. Spies <johnnyonflame@hotmail.com>
 *
 * KMS/DRM video backend code for SDL1
 */

#include "SDL_config.h"

#include <stdio.h>
#include <unistd.h>
#include <math.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <fcntl.h>

#include "SDL_endian.h"
#include "SDL_timer.h"
#include "SDL_thread.h"
#include "SDL_video.h"
#include "SDL_mouse.h"
#include "../SDL_sysvideo.h"
#include "../SDL_pixels_c.h"
#include "../../events/SDL_events_c.h"

#include "SDL_kmsdrmvideo.h"
#include "SDL_kmsdrmevents_c.h"
#include "SDL_kmsdrmmisc_c.h"
#include "SDL_kmsdrmcolordef.h"

#define KMSDRM_DRIVER_NAME "kmsdrm"

static int KMSDRM_TripleBufferingThread(void *d);
static void KMSDRM_TripleBufferInit(_THIS);
static void KMSDRM_TripleBufferStop(_THIS);
static void KMSDRM_TripleBufferQuit(_THIS);

static void KMSDRM_DeleteDevice(SDL_VideoDevice *device)
{
	SDL_free(device->hidden);
	SDL_free(device);
}

static int KMSDRM_HasDBufCaps(int fd)
{
	Uint64 has_dumb;
	return (drmGetCap(fd, DRM_CAP_DUMB_BUFFER, &has_dumb) >= 0 && has_dumb);
}

static int KMSDRM_OpenDevice()
{
	int fd;

	// First, see if we have a default node set.
	const char *env_node = getenv("SDL_VIDEO_KMSDRM_NODE");
	if (env_node) {
		fd = open(env_node, O_RDWR | O_CLOEXEC);
		// Is our DRM device capable of dumb buffers?
		if ( fd >= 0 && !KMSDRM_HasDBufCaps(fd) ) {
			fprintf(stderr, "Default node '%s' has no dumb buffer capability.\n", env_node);
			close(fd);
			return(-1);
		}
	} else {
		char node_path[] = "/dev/dri/cardxxx";
		for (int i = 0; i < 128; i++) {
			snprintf(node_path, sizeof(node_path), "/dev/dri/card%d", i);
			fd = open(node_path, O_RDWR | O_CLOEXEC);

			// If device node does not exist, stop searching
			if (fd == -ENOENT) {
				break;
			}

			// For any other error code, let's try the next
			if ( fd < 0 ) {
				continue;
			}

			// Is our DRM device capable of dumb buffers? If not, skip it
			if ( !KMSDRM_HasDBufCaps(fd) ) {
				close(fd);
				fd = -1;
			} else {
				break;
			}
		}
	}

	return(fd);
}

static int KMSDRM_Available(void)
{
	const char *envr = SDL_getenv("SDL_VIDEODRIVER");
	if ((envr) && (SDL_strcmp(envr, KMSDRM_DRIVER_NAME) == 0)) {
		return(1);
	}

	int fd = KMSDRM_OpenDevice();
	if ( fd < 0 ) {
		return(0);
	}

	close(fd);
	return(1);
}

int KMSDRM_VideoInit(_THIS, SDL_PixelFormat *vformat)
{
	if ( (drm_fd = KMSDRM_OpenDevice()) < 0 ) {
		SDL_SetError("Could not find any (capable) DRM device.\n");
		goto vidinit_fail;
	}

	if (drmSetClientCap(drm_fd, DRM_CLIENT_CAP_ATOMIC, 1)) {
		fprintf(stderr, "Failed to set client atomic cap, %s.\n", strerror(errno));
		goto vidinit_fail_fd;
	}

	// Enable universal planes (necessary for the atomic api)
	if (drmSetClientCap(drm_fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1)) {
		fprintf(stderr, "Failed to set universal planes cap, %s.\n", strerror(errno));
		goto vidinit_fail_fd;
	}

	// Get available resources.
	drmModeRes *res = drmModeGetResources(drm_fd);
	if (!res) {
		SDL_SetError("Unable to get resources for device.\n");
		goto vidinit_fail_fd;
	}

	drmModePlaneRes *pres = drmModeGetPlaneResources(drm_fd);
	if (!res) {
		SDL_SetError("Unable to get resources for device.\n");
		goto vidinit_fail_res;
	}

	#define acquire_props_for(_res, type, TYPE) \
		for (int i = 0; i < _res->count_##type##s; i++) { \
			kmsdrm_dbg_printf("PROPS FOR %s %d.\n", #TYPE, _res->type##s[i]); \
			acquire_properties(this, _res->type##s[i], DRM_MODE_OBJECT_##TYPE); \
		}

	// Acquire props for all objects
	acquire_props_for(pres, plane, PLANE);
	acquire_props_for(res, crtc, CRTC);
	acquire_props_for(res, connector, CONNECTOR);
	acquire_props_for(res, encoder, ENCODER);

	// Initialize vid_mode listing
	drm_vid_mode_count = 0;
	drm_vid_modes = SDL_realloc(drm_vid_modes, sizeof(*drm_vid_modes) * (drm_vid_mode_count+1));
	drm_vid_modes[0] = NULL;

	for (int plane_idx = 0; plane_idx < pres->count_planes; plane_idx++) {
		drmModePlane *plane = drmModeGetPlane(drm_fd, pres->planes[plane_idx]);
		if ( !plane ) {
			continue;
		}

		for (int crtc_idx = 0; crtc_idx < res->count_crtcs; crtc_idx++)
		for (int encoder_idx = 0; encoder_idx < res->count_encoders; encoder_idx++)
		for (int connector_idx = 0; connector_idx < res->count_connectors; connector_idx++) {
			drmModeCrtc *crtc = drmModeGetCrtc(drm_fd, res->crtcs[crtc_idx]);
			drmModeEncoder *enc = drmModeGetEncoder(drm_fd, res->encoders[encoder_idx]);
			drmModeConnector *conn = drmModeGetConnector(drm_fd, res->connectors[connector_idx]);
			if ( crtc && enc && conn &&
			     (plane->possible_crtcs & (1 << crtc_idx)) &&
			     (enc->possible_crtcs & (1 << crtc_idx)) &&
			     conn->encoder_id == enc->encoder_id &&
			     conn->connection == DRM_MODE_CONNECTED &&
			     conn->count_modes > 0 ) {
				// This is a complete, suitable pathway. save it.
				save_drm_pipe(this, plane->plane_id, crtc->crtc_id,
					      enc->encoder_id, conn);
			}

			if (crtc) drmModeFreeCrtc(crtc);
			if (enc)  drmModeFreeEncoder(enc);
			if (conn) drmModeFreeConnector(conn);
		}

		drmModeFreePlane(plane);
	}

	// Setup attempt finished, free resources
	drmModeFreeResources(res);
	drmModeFreePlaneResources(pres);

	// Setup video information
	this->info.hw_available = 1;
	if (drm_vid_modes[0]) {
		this->info.current_w = drm_vid_modes[0]->w;
		this->info.current_h = drm_vid_modes[0]->h;
		vformat->BitsPerPixel = KMSDRM_DEFAULT_COLOR_DEPTH;
	}

	// Check if we did not succeeded
	if (drm_first_pipe == NULL) {
		SDL_SetError("Unable to initialize device, no suitable pipes.\n");
		goto vidinit_fail_fd; /* resources already cleaned up */
	}

	// These values shouldn't be zero-initialized, so, set them appropriately
	drm_mode_blob_id = -1;
	for (int i = 0; i < sizeof(drm_buffers) / sizeof(drm_buffers[0]); i++) {
		drm_buffers[i].map = (void*)-1;
	}

	if (drmModeCreatePropertyBlob(drm_fd, drm_palette,
				      sizeof(drm_palette), &drm_palette_blob_id)) {
		SDL_SetError("Unable to create gamma LUT blob.\n");
		goto vidinit_fail_fd;
	}

	KMSDRM_TripleBufferInit(this);

	KMSDRM_InitInput(this);

	return 0;
vidinit_fail_res:
	while (free_drm_prop_storage(this));
	while (free_drm_pipe(this));
	drmModeFreeResources(res);
vidinit_fail_fd:
	close(drm_fd);
	drm_fd = -1;
vidinit_fail:
	return -1;
}

SDL_Rect **KMSDRM_ListModes(_THIS, SDL_PixelFormat *format, Uint32 flags)
{
	return drm_vid_modes;
}

static int KMSDRM_CreateFramebuffer(_THIS, int idx, Uint32 width, Uint32 height, const drm_color_def *color_def)
{
// Request a dumb buffer from the DRM device
	struct drm_mode_create_dumb *req_create = &drm_buffers[idx].req_create;
	struct drm_mode_map_dumb *req_map = &drm_buffers[idx].req_map;
	struct drm_mode_destroy_dumb *req_destroy_dumb = &drm_buffers[idx].req_destroy_dumb;

	/**
	 * Reserve a handle for framebuffer creation.
	 * A multi planar dumb buffers' height is a multiple of the requested height,
	 * and varies depending on the color format used.
	 **/
	req_create->width = width;
	req_create->height = height * color_def->h_factor;
	req_create->bpp = color_def->bpp;
	if ( drmIoctl(drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, req_create ) < 0) {
			SDL_SetError("Dumb framebuffer request failed, %s.\n", strerror(errno));
			return 0;
	}

	// Remember it in case we need to destroy it in the future
	req_destroy_dumb->handle = req_create->handle;
	kmsdrm_dbg_printf("Creating framebuffer %dx%dx%d (%c%c%c%c)\n", 
			 width, height, color_def->bpp,
			 color_def->four_cc        & 0xFF,
			(color_def->four_cc >>  8) & 0xFF,
			(color_def->four_cc >> 16) & 0xFF,
			 color_def->four_cc >> 24);

	// Calculate the necessary information to create the framebuffer
	Uint32 handles[4] = {};
	Uint32 pitches[4] = {};
	Uint32 offsets[4] = {};
	get_framebuffer_args(color_def, req_create->handle, req_create->pitch, height,
		&handles[0], &pitches[0], &offsets[0]);

	// Create the framebuffer object
	if ( drmModeAddFB2(drm_fd, width, height, color_def->four_cc, &handles[0], 
			&pitches[0], &offsets[0], &drm_buffers[idx].buf_id, 0) ) {
		SDL_SetError("Unable to create framebuffer, %s.\n", strerror(errno));
		goto createfb_fail_ddumb;
	}

	// Request information on how to map our framebuffer
	req_map->handle = req_create->handle;
	if ( drmIoctl(drm_fd, DRM_IOCTL_MODE_MAP_DUMB, req_map) < 0 ) {
		SDL_SetError("Map data request failed, %s.\n", strerror(errno));
		goto createfb_fail_rmfb;
	}

	// Map the framebuffer
	drm_buffers[idx].map = mmap(0, req_create->size, PROT_READ | PROT_WRITE, MAP_SHARED, drm_fd, req_map->offset);
	if ( drm_map == MAP_FAILED ) {
		SDL_SetError("Failed to map framebuffer, %s.\n", strerror(errno));
		goto createfb_fail_rmfb;
	}

	return 1;

createfb_fail_rmfb:
	drmModeRmFB(drm_fd, drm_buffers[idx].buf_id);
createfb_fail_ddumb:
	drmIoctl(drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, req_destroy_dumb);
	drm_buffers[idx].req_create.pitch = 0;
	return 0;
}

static void KMSDRM_ClearFramebuffers(_THIS)
{
	for (int i = 0; i < sizeof(drm_buffers) / sizeof(drm_buffers[0]); i++) {
		if (drm_buffers[i].req_create.handle != -1) {
			drmUnmap(drm_buffers[i].map, drm_buffers[i].req_create.size);
			drmModeRmFB(drm_fd, drm_buffers[i].buf_id);
			drmIoctl(drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &drm_buffers[i].req_destroy_dumb);
			drm_buffers[i].map = (void*)-1;
		}
	}
}

static int KMSDRM_VideoModeOK(_THIS, int width, int height, int bpp, Uint32 flags)
{
	return (bpp); /* TODO: check that the resolution is really OK */
}

static int KMSDRM_SetCrtcParams(_THIS, drmModeAtomicReqPtr req, Uint32 plane_id,
				Uint32 crtc_id, int width, int height,
				int mode_width, int mode_height, int bpp)
{
	unsigned int crtc_w, crtc_h;

	switch (this->hidden->scaling_mode) {
	case DRM_SCALING_MODE_ASPECT_RATIO:
		if (width * mode_height * drm_active_pipe->factor_w >
		    height * mode_width * drm_active_pipe->factor_h) {
			crtc_w = mode_width;
			crtc_h = drm_active_pipe->factor_h * crtc_w * height /
				(width * drm_active_pipe->factor_w);
		} else {
			crtc_h = mode_height;
			crtc_w = drm_active_pipe->factor_w * crtc_h * width /
				(height * drm_active_pipe->factor_h);
		}
		break;
	case DRM_SCALING_MODE_INTEGER_SCALED:
		if (width < mode_width / drm_active_pipe->factor_w &&
		    height < mode_height / drm_active_pipe->factor_h) {
			crtc_w = width * (mode_width / (width * drm_active_pipe->factor_w));
			crtc_h = height * (mode_height / (height * drm_active_pipe->factor_h));
			break;
		}
		/* fall-through */
	case DRM_SCALING_MODE_FULLSCREEN:
		crtc_w = mode_width;
		crtc_h = mode_height;
		break;
	case DRM_SCALING_MODE_END:
		fprintf(stderr, "Invalid mode %d\n", this->hidden->scaling_mode);
		return 1;
	}

	if (!add_property(this, req, plane_id, "CRTC_X", 0, (mode_width - crtc_w) / 2))
		return 1;

	if (!add_property(this, req, plane_id, "CRTC_Y", 0, (mode_height - crtc_h) / 2))
		return 1;

	if (!add_property(this, req, plane_id, "CRTC_W", 0, crtc_w))
		return 1;

	if (!add_property(this, req, plane_id, "CRTC_H", 0, crtc_h))
		return 1;

	if (bpp == 8 &&
	    !add_property(this, req, crtc_id, "GAMMA_LUT", 0, drm_palette_blob_id))
		return 1;

	return 0;
}

SDL_Surface *KMSDRM_SetVideoMode(_THIS, SDL_Surface *current,
				int width, int height, int bpp, Uint32 flags)
{
	// Lock the event thread, in multi-threading environments
	SDL_Lock_EventThread();

	// If we have set a video mode previously, now we need to clean up.
	if ( drm_active_pipe ) {
		if ( drm_triplebuf_thread ) {
			KMSDRM_TripleBufferStop(this);
		}

		drm_active_pipe = NULL;
		KMSDRM_ClearFramebuffers(this);
		drmModeDestroyPropertyBlob(drm_fd, drm_mode_blob_id);
		drmModeAtomicFree(this->hidden->drm_req);
		this->hidden->drm_req = NULL;
	}

	// Select the desired refresh rate.
	int refresh_rate = KMSDRM_DEFAULT_REFRESHRATE;
	char *r_end, *refresh_env = getenv("SDL_VIDEO_REFRESHRATE");
	if ( refresh_env ) {
		long rr = strtol(refresh_env, &r_end, 10);
		if (*r_end == '\0') {
			refresh_rate = rr;
		}
	}

	// Set all buffer indexes
	drm_back_buffer = 1;
	drm_front_buffer = 0;
	drm_queued_buffer = 2;	

	// Get rounded bpp number for drm_mode_create_dumb.
	const drm_color_def *color_def = get_drm_color_def(bpp, flags);
	if ( !color_def ) {
		SDL_SetError("Bad pixel format (%dbpp).\n", bpp);
		goto setvidmode_fail;
	}

	// Determine how many buffers do we need
	int n_buf = ((flags & SDL_TRIPLEBUF) == SDL_TRIPLEBUF) ? 3 : 
				((flags & SDL_TRIPLEBUF) == SDL_DOUBLEBUF) ? 2 : 1;

	// Initialize how many framebuffers were requested
	kmsdrm_dbg_printf("Creating %d framebuffers!\n", n_buf);
	for (int i = 0; i < n_buf; i++) {
		if ( !KMSDRM_CreateFramebuffer(this, i, width, height, color_def)) {
			goto setvidmode_fail_fbs;
		}
	}

	#define attempt_add_prop(t, req, id, name, opt, val) \
		if (!add_property(t, req, id, name, opt, val)) \
			goto setvidmode_fail_req;

	#define attempt_add_prop2(t, req, id, name, opt, val) \
		if (!add_property(t, req, id, name, opt, val)) \
			goto setvidmode_fail_req2;

	drmModeAtomicReqPtr req;

	for (drm_pipe *pipe = drm_first_pipe; pipe; pipe = pipe->next) {
		drmModeModeInfo *closest_mode = find_pipe_closest_refresh(pipe, refresh_rate);

		// Use the connector's preferred mode first.
		drmModeCreatePropertyBlob(drm_fd, closest_mode, sizeof(*closest_mode), &drm_mode_blob_id);

		// Start a new atomic modeset request
		req = drmModeAtomicAlloc();

		kmsdrm_dbg_printf("Attempting plane: %d crtc: %d mode: #%02d ", pipe->plane, pipe->crtc, drm_mode_blob_id);
		dump_mode(closest_mode);

		// Disable the other primary planes of this CRTC
		for (drm_pipe *other = drm_first_pipe; other; other = other->next) {
			if (other != pipe && other->crtc == pipe->crtc) {
				attempt_add_prop(this, req, other->plane, "FB_ID", 0, 0);
				attempt_add_prop(this, req, other->plane, "CRTC_ID", 0, 0);
			}
		}

		// Setup crtc->connector pipe
		attempt_add_prop(this, req, pipe->connector, "CRTC_ID", 0, pipe->crtc);
		attempt_add_prop(this, req, pipe->crtc, "MODE_ID", 0, drm_mode_blob_id);
		attempt_add_prop(this, req, pipe->crtc, "ACTIVE", 0, 1);

		this->hidden->drm_req = req;
		req = drmModeAtomicDuplicate(req);

		// Setup plane->crtc pipe
		attempt_add_prop2(this, req, pipe->plane, "FB_ID", 0, drm_buffers[drm_front_buffer].buf_id);
		attempt_add_prop2(this, req, pipe->plane, "CRTC_ID", 0, pipe->crtc);

		// Setup plane details
		attempt_add_prop2(this, req, pipe->plane, "SRC_X", 0, 0);
		attempt_add_prop2(this, req, pipe->plane, "SRC_Y", 0, 0);
		attempt_add_prop2(this, req, pipe->plane, "SRC_W", 0, width << 16);
		attempt_add_prop2(this, req, pipe->plane, "SRC_H", 0, height << 16);

		drm_active_pipe = pipe;

		if (KMSDRM_SetCrtcParams(this, req, pipe->plane, pipe->crtc, width, height,
					 closest_mode->hdisplay, closest_mode->vdisplay, bpp)) {
			fprintf(stderr, "Unable to set CRTC params: %s\n", strerror(errno));
			goto setvidmode_fail_req2;
		}

		int rc = drmModeAtomicCommit(drm_fd, req,
					     DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
		drmModeAtomicFree(req);

		// Modeset successful, remember necessary data
		if ( !rc ) {
			this->hidden->w = width;
			this->hidden->h = height;
			this->hidden->crtc_w = closest_mode->hdisplay;
			this->hidden->crtc_h = closest_mode->vdisplay;
			this->hidden->bpp = bpp;
			break;
		} else {
			kmsdrm_dbg_printf("SetVideoMode failed: %s, retrying.\n", strerror(errno));
			drmModeAtomicFree(this->hidden->drm_req);
			this->hidden->drm_req = NULL;
			drm_active_pipe = NULL;
		}

		// Modeset failed, clean up request and related objects
		drmModeDestroyPropertyBlob(drm_fd, drm_mode_blob_id);
		drm_mode_blob_id = -1;
	}

	// If we've got no active pipe, then modeset failed. Bail out.
	if ( !drm_active_pipe ) {
		SDL_SetError("Unable to set video mode.\n");
		goto setvidmode_fail_fbs;
	}

	// Let SDL know about the created framebuffer
	if ( ! SDL_ReallocFormat(current, bpp, color_def->r_mask, color_def->g_mask,
	        color_def->b_mask, color_def->a_mask) ) {
		SDL_SetError("Unable to recreate surface format structure!\n");
		goto setvidmode_fail_fbs;
	}

	if ( flags & (SDL_DOUBLEBUF | SDL_TRIPLEBUF) ) {
		current->pixels = drm_buffers[drm_back_buffer].map;
	} else {
		current->pixels = drm_buffers[drm_front_buffer].map;
	}

	current->w = width;
	current->h = height;
	current->pitch = drm_buffers[0].req_create.pitch;

	this->hidden->has_damage_clips = find_property(this, drm_active_pipe->plane,
						       "FB_DAMAGE_CLIPS");

	// Let SDL know what type of surface this is. In case the user asks for a
	// SDL_SWSURFACE video mode, SDL will silently create a shadow buffer
	// as an intermediary.
	current->flags =
		         SDL_HWSURFACE  |
		(flags & SDL_HWPALETTE) |
		(flags & SDL_TRIPLEBUF); /* SDL_TRIPLEBUF implies SDL_DOUBLEBUF */

	if ( (flags & SDL_TRIPLEBUF) == SDL_TRIPLEBUF ) {
		SDL_LockMutex(drm_triplebuf_mutex);
		drm_triplebuf_thread_stop = 0;
		drm_triplebuf_thread = SDL_CreateThread(KMSDRM_TripleBufferingThread, this);

		/* Wait until the triplebuf thread is ready */
		SDL_CondWait(drm_triplebuf_cond, drm_triplebuf_mutex);
		SDL_UnlockMutex(drm_triplebuf_mutex);
	}

	// Unlock the event thread, in multi-threading environments
	SDL_Unlock_EventThread();
	return current;

setvidmode_fail_req2:
	drmModeAtomicFree(this->hidden->drm_req);
	this->hidden->drm_req = NULL;
setvidmode_fail_req:
	drmModeAtomicFree(req);
	drmModeDestroyPropertyBlob(drm_fd, drm_mode_blob_id);
	drm_mode_blob_id = -1;
setvidmode_fail_fbs:
	KMSDRM_ClearFramebuffers(this);
setvidmode_fail:
	SDL_Unlock_EventThread();
	return NULL;
}

/* We don't actually allow hardware surfaces other than the main one */
static int KMSDRM_AllocHWSurface(_THIS, SDL_Surface *surface)
{
	return(-1);
}
static void KMSDRM_FreeHWSurface(_THIS, SDL_Surface *surface)
{
	return;
}

/* We need to wait for vertical retrace on page flipped displays */
static int KMSDRM_LockHWSurface(_THIS, SDL_Surface *surface)
{
	return(0);
}

static void KMSDRM_UnlockHWSurface(_THIS, SDL_Surface *surface)
{
	return;
}

static int KMSDRM_TripleBufferingThread(void *d)
{
	SDL_VideoDevice *this = d;

	SDL_LockMutex(drm_triplebuf_mutex);
	SDL_CondSignal(drm_triplebuf_cond);

	for (;;) {
		int page;

		SDL_CondWait(drm_triplebuf_cond, drm_triplebuf_mutex);
		if (drm_triplebuf_thread_stop)
			break;

		/* Flip the most recent back buffer with the front buffer */
		page = drm_queued_buffer;
		drm_queued_buffer = drm_front_buffer;
		drm_front_buffer = page;

		drmModeAtomicReqPtr req = drmModeAtomicDuplicate(this->hidden->drm_req);

		if (KMSDRM_SetCrtcParams(this, req, drm_active_pipe->plane,
					 drm_active_pipe->crtc,
					 this->hidden->w, this->hidden->h,
					 this->hidden->crtc_w, this->hidden->crtc_h,
					 this->hidden->bpp))
			fprintf(stderr, "Unable to set CRTC params: %s\n", strerror(errno));

		/* flip display */
		if (!add_property(this, req, drm_active_pipe->plane,
				  "FB_ID", 0, drm_buffers[drm_queued_buffer].buf_id))
			fprintf(stderr, "Unable to set FB_ID property: %s\n", strerror(errno));

		int rc = drmModeAtomicCommit(drm_fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
		if (rc)
			fprintf(stderr, "Unable to flip buffers: %s\n", strerror(errno));

		drmModeAtomicFree(req);
	}

	SDL_UnlockMutex(drm_triplebuf_mutex);
	return 0;
}

static void KMSDRM_TripleBufferInit(_THIS)
{
	drm_triplebuf_mutex = SDL_CreateMutex();
	drm_triplebuf_cond = SDL_CreateCond();
	drm_triplebuf_thread = NULL;
}

static void KMSDRM_TripleBufferStop(_THIS)
{
	SDL_LockMutex(drm_triplebuf_mutex);
	drm_triplebuf_thread_stop = 1;
	SDL_CondSignal(drm_triplebuf_cond);
	SDL_UnlockMutex(drm_triplebuf_mutex);

	SDL_WaitThread(drm_triplebuf_thread, NULL);
	drm_triplebuf_thread = NULL;
}

static void KMSDRM_TripleBufferQuit(_THIS)
{
	if (drm_triplebuf_thread)
		KMSDRM_TripleBufferStop(this);
	SDL_DestroyMutex(drm_triplebuf_mutex);
	SDL_DestroyCond(drm_triplebuf_cond);
}

static int KMSDRM_FlipHWSurface(_THIS, SDL_Surface *surface)
{
	if ( !drm_active_pipe )
		return -2;

	// Either wait for VSync or for buffer acquire
	if ( (surface->flags & SDL_TRIPLEBUF) == SDL_DOUBLEBUF ) {
		drmModeAtomicReqPtr req = drmModeAtomicDuplicate(this->hidden->drm_req);

		if (KMSDRM_SetCrtcParams(this, req, drm_active_pipe->plane,
					 drm_active_pipe->crtc,
					 this->hidden->w, this->hidden->h,
					 this->hidden->crtc_w, this->hidden->crtc_h,
					 this->hidden->bpp))
			fprintf(stderr, "Unable to set CRTC params: %s\n", strerror(errno));

		if (!add_property(this, req, drm_active_pipe->plane,
				  "FB_ID", 0, drm_buffers[drm_back_buffer].buf_id))
			fprintf(stderr, "Unable to set FB_ID property: %s\n", strerror(errno));

		int rc = drmModeAtomicCommit(drm_fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
		if (rc)
			fprintf(stderr, "Unable to flip buffers: %s\n", strerror(errno));

		drmModeAtomicFree(req);
	} else {
		SDL_LockMutex(drm_triplebuf_mutex);
	}

	// Swap between the two available buffers
	int prev_buffer = drm_front_buffer;
	drm_front_buffer = drm_back_buffer;
	drm_back_buffer = prev_buffer;

	surface->pixels = drm_buffers[drm_back_buffer].map;

	if ( (surface->flags & SDL_TRIPLEBUF) == SDL_TRIPLEBUF ) {
		SDL_CondSignal(drm_triplebuf_cond);
		SDL_UnlockMutex(drm_triplebuf_mutex);
	}

	return 1;
}

static void KMSDRM_UpdateRects(_THIS, int numrects, SDL_Rect *rects)
{
	struct drm_mode_rect *drm_rects;
	drmModeAtomicReqPtr req;
	unsigned int i;
	Uint32 blob_id;
	int ret;

	/**
	 * this->hidden->drm_req is NULL - SDL_SetVideoMode must be called
	 * first.
	 **/
	if (!this->hidden->drm_req)
		return;

	/* No FB_DAMAGE_CLIPS property - no need to go further */
	if (!this->hidden->has_damage_clips)
		return;

	req = drmModeAtomicDuplicate(this->hidden->drm_req);

	drm_rects = alloca(numrects * sizeof(*drm_rects));

	for (i = 0; i < numrects; i++) {
		drm_rects[i].x1 = rects[i].x;
		drm_rects[i].y1 = rects[i].y;
		drm_rects[i].x2 = rects[i].x + rects[i].w;
		drm_rects[i].y2 = rects[i].y + rects[i].h;
	}

	ret = drmModeCreatePropertyBlob(drm_fd, drm_rects,
					sizeof(*drm_rects) * numrects, &blob_id);
	if (ret != 0) {
		fprintf(stderr, "Unable to create damage clips blob\n");
		return;
	}

	if (KMSDRM_SetCrtcParams(this, req, drm_active_pipe->plane,
				 drm_active_pipe->crtc,
				 this->hidden->w, this->hidden->h,
				 this->hidden->crtc_w, this->hidden->crtc_h,
				 this->hidden->bpp))
		fprintf(stderr, "Unable to set CRTC params: %s\n", strerror(errno));

	if (!add_property(this, req, drm_active_pipe->plane,
			  "FB_DAMAGE_CLIPS", 0, blob_id))
		fprintf(stderr, "Unable to set FB_DAMAGE_CLIPS property: %s\n", strerror(errno));

	if (!add_property(this, req, drm_active_pipe->plane,
			  "FB_ID", 0, drm_buffers[drm_front_buffer].buf_id))
		fprintf(stderr, "Unable to set FB_ID property: %s\n", strerror(errno));

	int rc = drmModeAtomicCommit(drm_fd, req, DRM_MODE_ATOMIC_NONBLOCK, NULL);
	if (rc && errno != EBUSY)
		fprintf(stderr, "Unable to update rects: %s\n", strerror(errno));

	drmModeAtomicFree(req);
}

int KMSDRM_SetColors(_THIS, int firstcolor, int ncolors, SDL_Color *colors)
{
	unsigned int i;
	Uint32 blob_id, old_palette_id;

	for (i = firstcolor; i < firstcolor + ncolors; i++) {
		drm_palette[i] = (struct drm_color_lut){
			.red = colors[i].r << 8,
			.green = colors[i].g << 8,
			.blue = colors[i].b << 8,
		};
	}

	if (drmModeCreatePropertyBlob(drm_fd, drm_palette,
				      sizeof(drm_palette), &blob_id)) {
		fprintf(stderr, "Unable to create gamma LUT blob\n");
		return 1;
	}

	old_palette_id = drm_palette_blob_id;
	drm_palette_blob_id = blob_id;

	drmModeDestroyPropertyBlob(drm_fd, old_palette_id);

	return(0);
}

/* Note:  If we are terminated, this could be called in the middle of
   another SDL video routine -- notably UpdateRects.
*/
void KMSDRM_VideoQuit(_THIS)
{
	if (this->screen->pixels != NULL)
	{
		KMSDRM_TripleBufferQuit(this);
		KMSDRM_ClearFramebuffers(this);
		drmModeDestroyPropertyBlob(drm_fd, drm_palette_blob_id);
		while (free_drm_prop_storage(this));
		while (free_drm_pipe(this));

		this->screen->pixels = NULL;
	}

	KMSDRM_ExitInput(this);
}

static SDL_VideoDevice *KMSDRM_CreateDevice(int devindex)
{
	SDL_VideoDevice *device;

	/* Initialize all variables that we clean on shutdown */
	device = (SDL_VideoDevice *)SDL_malloc(sizeof(SDL_VideoDevice));
	if ( device ) {
		SDL_memset(device, 0, (sizeof *device));
		device->hidden = (struct SDL_PrivateVideoData *)
				SDL_malloc((sizeof *device->hidden));
	}
	if ( (device == NULL) || (device->hidden == NULL) ) {
		SDL_OutOfMemory();
		if ( device ) {
			SDL_free(device);
		}
		return(0);
	}
	SDL_memset(device->hidden, 0, (sizeof *device->hidden));

	/* Set the function pointers */
	device->VideoInit = KMSDRM_VideoInit;
	device->ListModes = KMSDRM_ListModes;
	device->VideoModeOK = KMSDRM_VideoModeOK;
	device->SetVideoMode = KMSDRM_SetVideoMode;
	device->CreateYUVOverlay = NULL;
	device->SetColors = KMSDRM_SetColors;
	device->UpdateRects = KMSDRM_UpdateRects;
	device->VideoQuit = KMSDRM_VideoQuit;
	device->AllocHWSurface = KMSDRM_AllocHWSurface;
	device->CheckHWBlit = NULL;
	device->FillHWRect = NULL;
	device->SetHWColorKey = NULL;
	device->SetHWAlpha = NULL;
	device->LockHWSurface = KMSDRM_LockHWSurface;
	device->UnlockHWSurface = KMSDRM_UnlockHWSurface;
	device->FlipHWSurface = KMSDRM_FlipHWSurface;
	device->FreeHWSurface = KMSDRM_FreeHWSurface; // TODO:: Obvious
	device->SetCaption = NULL;
	device->SetIcon = NULL;
	device->IconifyWindow = NULL;
	device->GrabInput = NULL;
	device->GetWMInfo = NULL;
	device->InitOSKeymap = KMSDRM_InitOSKeymap;
	device->PumpEvents = KMSDRM_PumpEvents;

	device->free = KMSDRM_DeleteDevice;

	return device;
}

VideoBootStrap KMSDRM_bootstrap = {
	KMSDRM_DRIVER_NAME, "SDL kmsdrm video driver",
	KMSDRM_Available, KMSDRM_CreateDevice
};
