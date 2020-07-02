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

/* Dummy SDL video driver implementation; this is just enough to make an
 *  SDL-based application THINK it's got a working video driver, for
 *  applications that call SDL_Init(SDL_INIT_VIDEO) when they don't need it,
 *  and also for use as a collection of stubs when porting SDL to a new
 *  platform for which you haven't yet written a valid video driver.
 *
 * This is also a great way to determine bottlenecks: if you think that SDL
 *  is a performance problem for a given platform, enable this driver, and
 *  then see if your application runs faster without video overhead.
 *
 * Initial work by Ryan C. Gordon (icculus@icculus.org). A good portion
 *  of this was cut-and-pasted from Stephane Peter's work in the AAlib
 *  SDL video driver.  Renamed to "DUMMY" by Sam Lantinga.
 */

#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/fcntl.h>

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
#include "SDL_kmsdrmextras.h"
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

int KMSDRM_LookupVidMode(_THIS, int width, int height)
{
	for (int i = 0; i < drm_vid_mode_count; i++) {
		if (drm_vid_modes[i]->w == width && drm_vid_modes[i]->h == height) {
			return i;
		}
	}

	return -1;
}

void KMSDRM_RegisterVidMode(_THIS, int width, int height)
{
	if (KMSDRM_LookupVidMode(this, width, height) >= 0) {
		return;
	}

	drm_vid_mode_count++;
	drm_vid_modes = SDL_realloc(drm_vid_modes, sizeof(*drm_vid_modes) * (drm_vid_mode_count));
	drm_vid_modes[drm_vid_mode_count] = NULL;
	drm_vid_modes[drm_vid_mode_count-1] = SDL_calloc(1, sizeof(**drm_vid_modes));
	drm_vid_modes[drm_vid_mode_count-1]->x = 0;
	drm_vid_modes[drm_vid_mode_count-1]->y = 0;
	drm_vid_modes[drm_vid_mode_count-1]->w = width;
	drm_vid_modes[drm_vid_mode_count-1]->h = height;
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
			printf("PROPS FOR %s %d.\n", #TYPE, _res->type##s[i]); \
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
		Uint64 p_type;
		drmModePlane *plane = drmModeGetPlane(drm_fd, pres->planes[plane_idx]);
		if ( !plane ) {
			continue;
		}

		if ( !get_property(this, plane->plane_id, "type", &p_type) ) {
			SDL_SetError("Unable to query plane %d type.\n", plane->plane_id);
			drmModeFreePlane(plane);
			goto vidinit_fail_res;
		}

		if (p_type == DRM_PLANE_TYPE_OVERLAY) {
			printf("Skipping overlay plane %d.\n", plane->plane_id);
			drmModeFreePlane(plane);
			continue;
		}

		for (int crtc_idx = 0; crtc_idx < res->count_crtcs; crtc_idx++)
		for (int encoder_idx = 0; encoder_idx < res->count_encoders; encoder_idx++)
		for (int connector_idx = 0; connector_idx < res->count_connectors; connector_idx++) {
			drmModeCrtc *crtc = drmModeGetCrtc(drm_fd, res->crtcs[crtc_idx]);
			drmModeEncoder *enc = drmModeGetEncoder(drm_fd, res->encoders[encoder_idx]);
			drmModeConnector *conn = drmModeGetConnector(drm_fd, res->connectors[connector_idx]);
			if ( crtc && enc && conn ) {				
				// This is a suitable pathway, continue
				if (plane->possible_crtcs & (1 << crtc_idx)) {
					// This is a suitable pathway, continue
					if (enc->possible_crtcs & (1 << crtc_idx)) {
						// This is a suitable pathway, continue
						if (conn->encoder_id == enc->encoder_id && 
							conn->connection == DRM_MODE_CONNECTED && 
							conn->count_modes > 0) {
							// This is a complete, suitable pathway. save it.
							save_drm_pipe(this, plane->plane_id, crtc->crtc_id, 
								enc->encoder_id, conn->connector_id, &conn->modes[0]);
							printf("Supported modes:\n");
							for (int i = 0; i < conn->count_modes; i++) {
								KMSDRM_RegisterVidMode(this, conn->modes[i].hdisplay, conn->modes[i].vdisplay);

								printf(" * ");
								dump_mode(&conn->modes[i]);
							}
							printf("Supported formats:\n");
							for (int i = 0; i < plane->count_formats; i++) {
								printf(" * %c%c%c%c\n", 
									 plane->formats[i]        & 0xFF, 
									(plane->formats[i] >> 8)  & 0xFF, 
									(plane->formats[i] >> 16) & 0xFF, 
									 plane->formats[i] >> 24);
							}
						}
					}
				}
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

	KMSDRM_TripleBufferInit(this);
	
	if (KMSDRM_GetKeyboards(this) != 0)
		goto vidinit_fail_fd;

	if (KMSDRM_GetMice(this) != 0)
		goto vidinit_fail_fd;

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

	// Reserve a handle for framebuffer creation
	req_create->width = width;
	req_create->height = height;
	req_create->bpp = color_def->bpp;
	if ( drmIoctl(drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, req_create ) < 0) {
			SDL_SetError("Dumb framebuffer request failed, %s.\n", strerror(errno));
			return 0;
	}

	// Remember it in case we need to destroy it in the future
	req_destroy_dumb->handle = req_create->handle;
	printf("Creating framebuffer %dx%dx%d (%c%c%c%c)\n", width, height, color_def->bpp,
			 color_def->four_cc        & 0xFF,
			(color_def->four_cc >>  8) & 0xFF,
			(color_def->four_cc >> 16) & 0xFF,
			 color_def->four_cc >> 24);

	// Calculate the necessary information to create the framebuffer
	Uint32 handles[4] = {};
	Uint32 pitches[4] = {};
	Uint32 offsets[4] = {};
	get_framebuffer_args(color_def, req_create->handle, req_create->pitch, 
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
	}

	// Set all buffer indexes
	drm_back_buffer = 1;
	drm_front_buffer = 0;
	drm_queued_buffer = 2;	

	// Get rounded bpp number for drm_mode_create_dumb.
	const drm_color_def *color_def = get_drm_color_def(&bpp, 0, flags);
	if ( !color_def ) {
		SDL_SetError("Bad pixel format (%dbpp).\n", bpp);
		goto setvidmode_fail;
	}

	// Determine how many buffers do we need
	int n_buf = ((flags & SDL_TRIPLEBUF) == SDL_TRIPLEBUF) ? 3 : 
				((flags & SDL_TRIPLEBUF) == SDL_DOUBLEBUF) ? 2 : 1;

	// Initialize how many framebuffers were requested
	printf("Creating %d framebuffers!\n", n_buf);
	for (int i = 0; i < n_buf; i++) {
		if ( !KMSDRM_CreateFramebuffer(this, i, width, height, color_def)) {
			goto setvidmode_fail_fbs;
		}
	}

	#define attempt_add_prop(t, re, id, name, opt, val) \
		if (!add_property(t, re, id, name, opt, val)) { \
			drmModeAtomicFree(re); \
			goto setvidmode_fail_req; \
		}

	// Disable pipes before attempting to modeset
	for (drm_pipe *pipe = drm_first_pipe; pipe; pipe = pipe->next) {
		drmModeAtomicReq *req = drmModeAtomicAlloc();

		// Disconnect plane->connector pipe
		attempt_add_prop(this, req, pipe->plane, "FB_ID", 0, 0);
		attempt_add_prop(this, req, pipe->plane, "CRTC_ID", 0, 0);
		attempt_add_prop(this, req, pipe->connector, "CRTC_ID", 0, 0);
		attempt_add_prop(this, req, pipe->crtc, "MODE_ID", 0, 0);
		attempt_add_prop(this, req, pipe->crtc, "ACTIVE", 0, 0);

		int rc = drmModeAtomicCommit(drm_fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
		drmModeAtomicFree(req);
		if ( rc ) {
			printf("Failed to deactivate one or more pipes.\n");
			goto setvidmode_fail_req;
		}
	}


	for (drm_pipe *pipe = drm_first_pipe; pipe; pipe = pipe->next) {
		// Use the connector's preferred mode first.
		drmModeCreatePropertyBlob(drm_fd, &pipe->preferred_mode, sizeof(pipe->preferred_mode), &drm_mode_blob_id);

		// Start a new atomic modeset request
		drmModeAtomicReq *req = drmModeAtomicAlloc();
		printf("Attempting plane: %d crtc: %d mode: #%02d ", pipe->plane, pipe->crtc, drm_mode_blob_id);
		dump_mode(&pipe->preferred_mode);

		// Setup crtc->connector pipe
		attempt_add_prop(this, req, pipe->connector, "CRTC_ID", 0, pipe->crtc);
		attempt_add_prop(this, req, pipe->crtc, "MODE_ID", 0, drm_mode_blob_id);
		attempt_add_prop(this, req, pipe->crtc, "ACTIVE", 0, 1);

		// Setup plane->crtc pipe
		attempt_add_prop(this, req, pipe->plane, "FB_ID", 0, drm_buffers[drm_front_buffer].buf_id);
		attempt_add_prop(this, req, pipe->plane, "CRTC_ID", 0, pipe->crtc);

		// Setup plane details
		attempt_add_prop(this, req, pipe->plane, "SRC_X", 0, 0);
		attempt_add_prop(this, req, pipe->plane, "SRC_Y", 0, 0);
		attempt_add_prop(this, req, pipe->plane, "SRC_W", 0, width << 16);
		attempt_add_prop(this, req, pipe->plane, "SRC_H", 0, height << 16);
		attempt_add_prop(this, req, pipe->plane, "CRTC_X", 0, 0);
		attempt_add_prop(this, req, pipe->plane, "CRTC_Y", 0, 0);
		attempt_add_prop(this, req, pipe->plane, "CRTC_W", 0, pipe->preferred_mode.hdisplay);
		attempt_add_prop(this, req, pipe->plane, "CRTC_H", 0, pipe->preferred_mode.vdisplay);

		int rc = drmModeAtomicCommit(drm_fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
		drmModeAtomicFree(req);
		// Modeset successful, remember necessary data
		if ( !rc ) {
			drm_active_pipe = pipe;
			break;
		} else {
			printf("SetVideoMode failed: %s, retrying.\n", strerror(errno));
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

	// Acquire the prop_id necessary for flipping buffers
	if ( (drm_fb_id_prop = get_prop_id(this, drm_active_pipe->plane, "FB_ID")) == -1 ) {
		goto setvidmode_fail_fbs;
	}

	/** TODO:: Investigate color masks from requested modes **/
	// Let SDL know about the created framebuffer
	if ( ! SDL_ReallocFormat(current, bpp, color_def->r_mask, color_def->g_mask,
	        color_def->b_mask, color_def->a_mask) ) {
		SDL_SetError("Unable to recreate surface format structure!\n");
		goto setvidmode_fail_fbs;
	}

	current->w = width;
	current->h = height;
	current->pitch = drm_buffers[0].req_create.pitch;
	if ( flags & (SDL_DOUBLEBUF | SDL_TRIPLEBUF) ) {
		current->pixels = drm_buffers[drm_back_buffer].map;
	} else {
		current->pixels = drm_buffers[drm_front_buffer].map;
	}
	
	// Let SDL know what type of surface this is. In case the user asks for a
	// SDL_SWSURFACE video mode, SDL will silently create a shadow buffer
	// as an intermediary.
	current->flags = SDL_HWSURFACE | (flags & SDL_DOUBLEBUF) | (flags & SDL_TRIPLEBUF);

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

setvidmode_fail_req:
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

		/* flip display */
		drmModeObjectSetProperty(drm_fd, drm_active_pipe->plane, 
			DRM_MODE_OBJECT_PLANE, drm_fb_id_prop, drm_buffers[drm_queued_buffer].buf_id);
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
		drmModeObjectSetProperty(drm_fd, drm_active_pipe->plane, 
			DRM_MODE_OBJECT_PLANE, drm_fb_id_prop, drm_buffers[drm_back_buffer].buf_id);
	} else {
		SDL_LockMutex(drm_triplebuf_mutex);
	}

	// Swap between the two available buffers
	int prev_buffer = drm_front_buffer;
	drm_front_buffer = drm_back_buffer;
	drm_back_buffer = prev_buffer;

	// Expose the new buffer
	surface->pixels = drm_buffers[drm_back_buffer].map;

	if ( (surface->flags & SDL_TRIPLEBUF) == SDL_TRIPLEBUF ) {
		SDL_CondSignal(drm_triplebuf_cond);
		SDL_UnlockMutex(drm_triplebuf_mutex);
	}

	return 1;
}

static void KMSDRM_UpdateRects(_THIS, int numrects, SDL_Rect *rects)
{
	// Do nothing.
}

int KMSDRM_SetColors(_THIS, int firstcolor, int ncolors, SDL_Color *colors)
{
	/* do nothing of note. */
	return(1);
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
		while (free_drm_prop_storage(this));
		while (free_drm_pipe(this));

		this->screen->pixels = NULL;
	}
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
