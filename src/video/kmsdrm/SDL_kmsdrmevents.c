// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * Copyright (C) 2020 Paul Cercueil <paul@crapouillou.net>
 *
 * libudev/libevdev input code for SDL1
 */

#include "SDL_config.h"
#include "SDL_keysym.h"

#include "SDL.h"
#include "../../events/SDL_sysevents.h"
#include "../../events/SDL_events_c.h"

#include "SDL_kmsdrmvideo.h"
#include "SDL_kmsdrmevents_c.h"

#include <errno.h>
#include <libudev.h>
#include <linux/input.h>
#include <linux/input-event-codes.h>
#include <unistd.h>

static drm_input_dev * KMSDRM_GetInputDevice(const char *property)
{
	drm_input_dev *new_dev, *input_devs = NULL;
	struct udev_enumerate *enumerate;
	struct udev_list_entry *devices, *entry;
	struct udev_device *dev;
	struct udev *udev;
	const char *path;

	udev = udev_new();
	if (!udev) {
		SDL_SetError("Could not create libudev instance.\n");
		return(NULL);
	}

	enumerate = udev_enumerate_new(udev);
	if (!enumerate) {
		SDL_SetError("Could not create libudev enumerate instance.\n");
		goto out_unref;
	}

	udev_enumerate_add_match_subsystem(enumerate, "input");
	udev_enumerate_add_match_property(enumerate, property, "1");
	udev_enumerate_scan_devices(enumerate);

	devices = udev_enumerate_get_list_entry(enumerate);
	if (!devices) {
		SDL_SetError("Failed to get device list.\n");
		goto out_unref_enumerate;
	}

	udev_list_entry_foreach(entry, devices) {
		dev = udev_device_new_from_syspath(udev, udev_list_entry_get_name(entry));
		if (!dev) {
			SDL_SetError("Failed to create device.\n");
			goto out_unref_enumerate;
		}

		path = udev_device_get_devnode(dev);
		if (!path) {
			udev_device_unref(dev);
			continue;
		}

		new_dev = SDL_malloc(sizeof(*new_dev));
		if (!new_dev) {
			udev_device_unref(dev);
			goto out_unref_enumerate;
		}

		new_dev->fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (new_dev->fd < 0) {
			SDL_SetError("Could not open device '%s'\n", path);
			SDL_free(new_dev);
			udev_device_unref(dev);
			goto out_unref_enumerate;
		}

		new_dev->path = SDL_strdup(path);
		if (!new_dev->path) {
			close(new_dev->fd);
			SDL_free(new_dev);
			udev_device_unref(dev);
			goto out_unref_enumerate;
		}

		new_dev->next = input_devs;
		input_devs = new_dev;

		udev_device_unref(dev);
	}

out_unref_enumerate:
	udev_enumerate_unref(enumerate);
out_unref:
	udev_unref(udev);
	return(input_devs);
}

void KMSDRM_InitInput(_THIS)
{
	drm_input_dev *devs;

	devs = KMSDRM_GetInputDevice("ID_INPUT_KEY");
	this->hidden->keyboards = devs;

	for (; devs; devs = devs->next)
		kmsdrm_dbg_printf("Found keyboard: %s\n", devs->path);

	devs = KMSDRM_GetInputDevice("ID_INPUT_MOUSE");
	this->hidden->mice = devs;

	for (; devs; devs = devs->next)
		kmsdrm_dbg_printf("Found mouse: %s\n", devs->path);
}

void KMSDRM_ExitInput(_THIS)
{
	drm_input_dev *devs, *next;

	for (devs = this->hidden->keyboards; devs; devs = next) {
		next = devs->next;

		SDL_free(devs->path);
		close(devs->fd);
		SDL_free(devs);
	}
}

static const SDLKey keymap[] = {
	[KEY_ESC] = SDLK_ESCAPE,
	[KEY_1] = SDLK_1,
	[KEY_2] = SDLK_2,
	[KEY_3] = SDLK_3,
	[KEY_4] = SDLK_4,
	[KEY_5] = SDLK_5,
	[KEY_6] = SDLK_6,
	[KEY_7] = SDLK_7,
	[KEY_8] = SDLK_8,
	[KEY_9] = SDLK_9,
	[KEY_0] = SDLK_0,
	[KEY_MINUS] = SDLK_MINUS,

	[KEY_EQUAL] = SDLK_EQUALS,
	[KEY_BACKSPACE] = SDLK_BACKSPACE,
	[KEY_TAB] = SDLK_TAB,
	[KEY_Q] = SDLK_q,
	[KEY_W] = SDLK_w,
	[KEY_E] = SDLK_e,
	[KEY_R] = SDLK_r,
	[KEY_T] = SDLK_t,
	[KEY_Y] = SDLK_y,
	[KEY_U] = SDLK_u,
	[KEY_I] = SDLK_i,
	[KEY_O] = SDLK_o,
	[KEY_P] = SDLK_p,
	[KEY_LEFTBRACE] = SDLK_LEFTBRACKET,
	[KEY_RIGHTBRACE] = SDLK_RIGHTBRACKET,
	[KEY_ENTER] = SDLK_RETURN,
	[KEY_LEFTCTRL] = SDLK_LCTRL,
	[KEY_A] = SDLK_a,
	[KEY_S] = SDLK_s,
	[KEY_D] = SDLK_d,
	[KEY_F] = SDLK_f,
	[KEY_G] = SDLK_g,
	[KEY_H] = SDLK_h,
	[KEY_J] = SDLK_j,
	[KEY_K] = SDLK_k,
	[KEY_L] = SDLK_l,
	[KEY_SEMICOLON] = SDLK_SEMICOLON,
	[KEY_APOSTROPHE] = SDLK_QUOTE,
	/* [KEY_GRAVE] = SDLK_GRAVE, ??? */
	[KEY_LEFTSHIFT] = SDLK_LSHIFT,
	[KEY_BACKSLASH] = SDLK_BACKSLASH,
	[KEY_Z] = SDLK_z,
	[KEY_X] = SDLK_x,
	[KEY_C] = SDLK_c,
	[KEY_V] = SDLK_v,
	[KEY_B] = SDLK_b,
	[KEY_N] = SDLK_n,
	[KEY_M] = SDLK_m,
	[KEY_COMMA] = SDLK_COMMA,
	[KEY_DOT] = SDLK_PERIOD,
	[KEY_SLASH] = SDLK_SLASH,
	[KEY_RIGHTSHIFT] = SDLK_RSHIFT,
	[KEY_KPASTERISK] = SDLK_KP_MULTIPLY,
	[KEY_LEFTALT] = SDLK_LALT,
	[KEY_SPACE] = SDLK_SPACE,
	[KEY_CAPSLOCK] = SDLK_CAPSLOCK,
	[KEY_F1] = SDLK_F1,
	[KEY_F2] = SDLK_F2,
	[KEY_F3] = SDLK_F3,
	[KEY_F4] = SDLK_F4,
	[KEY_F5] = SDLK_F5,
	[KEY_F6] = SDLK_F6,
	[KEY_F7] = SDLK_F7,
	[KEY_F8] = SDLK_F8,
	[KEY_F9] = SDLK_F9,
	[KEY_F10] = SDLK_F10,
	[KEY_NUMLOCK] = SDLK_NUMLOCK,
	[KEY_SCROLLLOCK] = SDLK_SCROLLOCK,
	[KEY_KP7] = SDLK_KP7,
	[KEY_KP8] = SDLK_KP8,
	[KEY_KP9] = SDLK_KP9,
	[KEY_KPMINUS] = SDLK_KP_MINUS,
	[KEY_KP4] = SDLK_KP4,
	[KEY_KP5] = SDLK_KP5,
	[KEY_KP6] = SDLK_KP6,
	[KEY_KPPLUS] = SDLK_KP_PLUS,
	[KEY_KP1] = SDLK_KP1,
	[KEY_KP2] = SDLK_KP2,
	[KEY_KP3] = SDLK_KP3,
	[KEY_KP0] = SDLK_KP0,
	[KEY_KPDOT] = SDLK_KP_PERIOD,

	[KEY_F11] = SDLK_F11,
	[KEY_F12] = SDLK_F12,
	[KEY_KPENTER] = SDLK_KP_ENTER,
	[KEY_RIGHTCTRL] = SDLK_RCTRL,
	[KEY_KPSLASH] = SDLK_KP_DIVIDE,
	[KEY_SYSRQ] = SDLK_SYSREQ,
	[KEY_RIGHTALT] = SDLK_RALT,
	/*[KEY_LINEFEED] = SDLK_LINEFEED, ??? */
	[KEY_HOME] = SDLK_HOME,
	[KEY_UP] = SDLK_UP,
	[KEY_PAGEUP] = SDLK_PAGEUP,
	[KEY_LEFT] = SDLK_LEFT,
	[KEY_RIGHT] = SDLK_RIGHT,
	[KEY_END] = SDLK_END,
	[KEY_DOWN] = SDLK_DOWN,
	[KEY_PAGEDOWN] = SDLK_PAGEDOWN,
	[KEY_INSERT] = SDLK_INSERT,
	[KEY_DELETE] = SDLK_DELETE,

	[KEY_POWER] = SDLK_POWER,
	[KEY_KPEQUAL] = SDLK_KP_EQUALS,
	[KEY_PAUSE] = SDLK_PAUSE,

	[KEY_LEFTMETA] = SDLK_LMETA,
	[KEY_RIGHTMETA] = SDLK_RMETA,
	[KEY_COMPOSE] = SDLK_COMPOSE,

	[KEY_UNDO] = SDLK_UNDO,
	[KEY_HELP] = SDLK_HELP,
	[KEY_MENU] = SDLK_MENU,

	[KEY_F13] = SDLK_F13,
	[KEY_F14] = SDLK_F14,
	[KEY_F15] = SDLK_F15,

	[KEY_PRINT] = SDLK_PRINT,

	[BTN_LEFT] = SDL_BUTTON_LEFT,
	[BTN_RIGHT] = SDL_BUTTON_RIGHT,
	[BTN_MIDDLE] = SDL_BUTTON_MIDDLE,
};

void KMSDRM_InitOSKeymap(_THIS)
{
}

static void KMSDRM_HandleScaling(_THIS)
{
	this->hidden->scaling_mode++;

	if (this->hidden->scaling_mode == DRM_SCALING_MODE_END)
		this->hidden->scaling_mode = 0;
}

static void KMSDRM_PumpInputDev(_THIS, int fd, const char *path)
{
	struct input_event events[32];
	ssize_t bytes_read;
	SDL_keysym keysym;
	unsigned int i;
	int pressed;

	for (;;) {
		bytes_read = read(fd, events, sizeof(events));
		if (bytes_read < 0) {
			if (errno != EAGAIN)
				SDL_SetError("Unable to read from %s\n", path);
			break;
		}

		for (i = 0; i < bytes_read / sizeof(*events); i++) {
			if (events[i].type == EV_KEY) {
				const char *scaling_key = getenv("SDL_VIDEO_KMSDRM_SCALING_KEY");

				if (scaling_key && events[i].code == atoi(scaling_key)) {
					if (events[i].value)
						KMSDRM_HandleScaling(this);
					continue;
				}

				pressed = events[i].value ? SDL_PRESSED : SDL_RELEASED;
				keysym.sym = keymap[events[i].code];

				if (events[i].code >= BTN_LEFT && events[i].code <= BTN_TASK) {
					/* Mouse button event */
					SDL_PrivateMouseButton(pressed, keysym.sym, 0, 0);
				} else {
					/* Keyboard event */
					SDL_PrivateKeyboard(pressed, &keysym);
				}
			} else if (events[i].type == EV_REL) {
				switch (events[i].code) {
				case REL_X:
					SDL_PrivateMouseMotion(0, SDL_TRUE, events[i].value, 0);
					break;
				case REL_Y:
					SDL_PrivateMouseMotion(0, SDL_TRUE, 0, events[i].value);
					break;
				case REL_WHEEL:
					if (events[i].value < 0) {
						SDL_PrivateMouseButton(SDL_TRUE, SDL_BUTTON_WHEELDOWN, 0, 0);
						SDL_PrivateMouseButton(SDL_FALSE, SDL_BUTTON_WHEELDOWN, 0, 0);
					} else {
						SDL_PrivateMouseButton(SDL_TRUE, SDL_BUTTON_WHEELUP, 0, 0);
						SDL_PrivateMouseButton(SDL_FALSE, SDL_BUTTON_WHEELUP, 0, 0);
					}
					break;
				default:
					break;
				}
			}
		}
	}
}

void KMSDRM_PumpEvents(_THIS)
{
	drm_input_dev *devs;

	for (devs = this->hidden->keyboards; devs; devs = devs->next)
		KMSDRM_PumpInputDev(this, devs->fd, devs->path);

	for (devs = this->hidden->mice; devs; devs = devs->next)
		KMSDRM_PumpInputDev(this, devs->fd, devs->path);
}
