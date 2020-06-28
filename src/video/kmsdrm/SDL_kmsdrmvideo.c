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

#define KMSDRM_DRIVER_NAME "kmsdrm"

static void KMSDRM_DeleteDevice(SDL_VideoDevice *device)
{
	SDL_free(device->hidden);
	SDL_free(device);
}

static int KMSDRM_Available(void)
{
	const char *envr = SDL_getenv("SDL_VIDEODRIVER");
	if ((envr) && (SDL_strcmp(envr, KMSDRM_DRIVER_NAME) == 0)) {
		return(1);
	}

	return(0);
}

static int KMSDRM_HasDBufCaps(int fd)
{
	Uint64 has_dumb;
	return (drmGetCap(fd, DRM_CAP_DUMB_BUFFER, &has_dumb) >= 0 && has_dumb);
}

int KMSDRM_VideoInit(_THIS, SDL_PixelFormat *vformat)
{
	char *node = chooseDefault("SDL_VIDEO_KMSDRM_NODE", "/dev/dri/card0");
	if ( (drm_fd = open(node, O_RDWR | O_CLOEXEC)) < 0 ) {
		SDL_SetError("Could not open device '%s'.\n", node);
		goto vidinit_fail;
	}

	// Is our DRM device capable of dumb buffers?
	if ( !KMSDRM_HasDBufCaps(drm_fd) ) {
		SDL_SetError("Device '%s' does not have dumb buffer capabilities.\n", node);
		goto vidinit_fail_fd;
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

	// Check if we did not succeeded
	if (drm_first_pipe == NULL) {
		SDL_SetError("Unable to initialize device, no suitable pipes.\n");
		goto vidinit_fail_fd; /* resources already cleaned up */
	}

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
	/** TODO:: Unstub. **/
   	return (SDL_Rect **) -1;
}

uint32_t pixelFormatToFourCC(SDL_PixelFormat *fmt)
{
	/** TODO:: Unstub. **/
	return 0;
}

SDL_Surface *KMSDRM_SetVideoMode(_THIS, SDL_Surface *current,
				int width, int height, int bpp, Uint32 flags)
{
	// Lock the event thread, in multi-threading environments
	SDL_Lock_EventThread();

	// Request a dumb buffer from the DRM device
	struct drm_mode_create_dumb req_create = {
			.width = width,
			.height = height,
			.bpp = (bpp == 24) ? 32 : bpp
	};

	if ( drmIoctl(drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &req_create ) < 0) {
			SDL_SetError("Dumb framebuffer request failed, %s.\n", strerror(errno));
			goto setvidmode_fail;
	}

	struct drm_mode_destroy_dumb req_destroy = {
		.handle = req_create.handle
	};

	/* Create the framebuffer object */
	if ( drmModeAddFB(drm_fd, width, height,
			(bpp == 32) ? 24 : 16,
			(bpp == 24) ? 32 : bpp, req_create.pitch, req_create.handle, &drm_fb) ) {
			SDL_SetError("Unable to add framebuffer, %s.\n", strerror(errno));
			goto setvidmode_fail_ddumb;
	}

	// Request to map our current framebuffer
	struct drm_mode_map_dumb req_map = {
			.handle = req_create.handle
	};

	if ( drmIoctl(drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &req_map) < 0 ) {
			SDL_SetError("Map data request failed, %s.\n", strerror(errno));
			goto setvidmode_fail_ddumb;
	}

	// Map the framebuffer
	drm_map = mmap(0, req_create.size, PROT_READ | PROT_WRITE, MAP_SHARED, drm_fd, req_map.offset);
	if ( drm_map == MAP_FAILED ) {
			SDL_SetError("Failed to map framebuffer, %s.\n", strerror(errno));
			goto setvidmode_fail_ddumb;
	}

	struct drm_mode_destroy_dumb req_ddumb = {
		.handle = req_create.handle
	};

	// Save all the modeset request structures
	memcpy(&drm_req_destroy_dumb, &req_ddumb, sizeof(req_ddumb));
	memcpy(&drm_req_create, &req_create, sizeof(req_create));
	memcpy(&drm_req_map, &req_map, sizeof(req_map));

	#define attempt_add_prop(t, re, id, name, opt, val) \
		if (!add_property(t, re, id, name, opt, val)) { \
			drmModeAtomicFree(re); \
			goto setvidmode_fail_req; \
		}
	
	// Gets checked by setvidmode_fail_req cleanup routine, don't reorder.
	Uint32 blob_id = -1;


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
		printf("Attempting plane: %d crtc: %d mode: ", pipe->plane, pipe->crtc);

		// Use the connector's preferred mode first.
		drmModeCreatePropertyBlob(drm_fd, &pipe->preferred_mode, sizeof(pipe->preferred_mode), &blob_id);
		dump_mode(&pipe->preferred_mode);

		// Start a new atomic modeset request
		drmModeAtomicReq *req = drmModeAtomicAlloc();

		// Setup crtc->connector pipe
		attempt_add_prop(this, req, pipe->connector, "CRTC_ID", 0, pipe->crtc);
		attempt_add_prop(this, req, pipe->crtc, "MODE_ID", 0, blob_id);
		attempt_add_prop(this, req, pipe->crtc, "ACTIVE", 0, 1);

		// Setup plane->crtc pipe
		attempt_add_prop(this, req, pipe->plane, "FB_ID", 0, drm_fb);
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
			drm_prev_crtc = drmModeGetCrtc(drm_fd, pipe->crtc);
			drm_active_pipe = pipe;
			break;
		}

		// Modeset failed, clean up request and related objects
		drmModeDestroyPropertyBlob(drm_fd, blob_id);
		blob_id = -1;
	}

	// If we've got no active pipe, then modeset failed. Bail out.
	if ( !drm_active_pipe ) {
		SDL_SetError("Unable to set video mode.\n");
		goto setvidmode_fail_munmap;
	}

	/** TODO:: Investigate color masks from requested modes **/
	// Let SDL know about the created framebuffer
	int rbits = req_create.bpp == 16 ? 5 : 8;
	int gbits = req_create.bpp == 16 ? 6 : 8;
	int bbits = req_create.bpp == 16 ? 5 : 8;
	#define MASK(bits_per_pixel) (0xFF >> (8-bits_per_pixel))
	if ( ! SDL_ReallocFormat(current, req_create.bpp, MASK(rbits) << 24,
					MASK(gbits) << 16, MASK(bbits) << 8, 0) ) {
			SDL_SetError("Unable to recreate surface format structure!\n");
			goto setvidmode_fail_realloc;
	}

	current->w = req_create.width;
	current->h = req_create.height;
	current->pitch = req_create.pitch;
	current->pixels = drm_map;
	current->flags = 0; /** TODO:: Add double, triple buffering and vsync! **/

	// Unlock the event thread, in multi-threading environments
	SDL_Unlock_EventThread();
	return current;

setvidmode_fail_req:
	drmModeDestroyPropertyBlob(drm_fd, blob_id);
setvidmode_fail_realloc:
	if (drm_prev_crtc) {
		drmModeFreeCrtc(drm_prev_crtc);
		drm_prev_crtc = NULL;
	}
setvidmode_fail_munmap:
	munmap(drm_map, req_create.size);
setvidmode_fail_ddumb:
	drmIoctl(drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &req_destroy);
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

static void KMSDRM_UpdateRects(_THIS, int numrects, SDL_Rect *rects)
{
	/* do nothing. */
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
		while (free_drm_prop_storage(this));
		while (free_drm_pipe(this));
		/** 
		 * TODO:: Maybe a test application to see how this behaves and if we need
		 * to really use this to undo the modeset, and how to.
		 **/
		if (drm_prev_crtc) drmModeFreeCrtc(drm_prev_crtc);
		drm_prev_crtc = NULL;
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
	device->FlipHWSurface = NULL;
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