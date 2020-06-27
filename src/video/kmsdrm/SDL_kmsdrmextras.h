
#include <stdio.h>
#include "SDL_kmsdrmvideo.h"

static char *chooseDefault(char *str_env, char *str2)
{
	char *str1 = getenv(str_env);
	return str1 ? str1 : str2;
}

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define bit_name_fn(res)					\
const char * res##_str(int type) {				\
	unsigned int i;						\
	const char *sep = "";					\
	for (i = 0; i < ARRAY_SIZE(res##_names); i++) {		\
		if (type & (1 << i)) {				\
			printf("%s%s", sep, res##_names[i]);	\
			sep = ", ";				\
		}						\
	}							\
	return NULL;						\
}


static const char *mode_type_names[] = {
	"builtin",
	"clock_c",
	"crtc_c",
	"preferred",
	"default",
	"userdef",
	"driver",
};

static bit_name_fn(mode_type)

static const char *mode_flag_names[] = {
	"phsync",
	"nhsync",
	"pvsync",
	"nvsync",
	"interlace",
	"dblscan",
	"csync",
	"pcsync",
	"ncsync",
	"hskew",
	"bcast",
	"pixmux",
	"dblclk",
	"clkdiv2"
};

static bit_name_fn(mode_flag)

static float mode_vrefresh(drmModeModeInfo *mode)
{
	return  mode->clock * 1000.00
			/ (mode->htotal * mode->vtotal);
}

static void dump_mode(drmModeModeInfo *mode, int index)
{
	printf("  #%02i %s %.2f %d %d %d %d %d %d %d %d %d",
	       index,
	       mode->name,
	       mode_vrefresh(mode),
	       mode->hdisplay,
	       mode->hsync_start,
	       mode->hsync_end,
	       mode->htotal,
	       mode->vdisplay,
	       mode->vsync_start,
	       mode->vsync_end,
	       mode->vtotal,
	       mode->clock);

	printf(" flags: ");
	mode_flag_str(mode->flags);
	printf("; type: ");
	mode_type_str(mode->type);
	printf("\n");
}

static const char *pretty[] = {
	"Connector",
	"CRTC",
	"Plane",
	"bad"
};

static const char *from_mode_object_type(Uint32 type)
{
	switch (type) {
#define CASE(_type, pretty) case DRM_MODE_OBJECT_##_type: return pretty
		CASE(CONNECTOR, pretty[0]);
		CASE(CRTC, pretty[1]);
		CASE(PLANE, pretty[2]);
		default: return pretty[3];
	}
}

static int save_drm_pipe(_THIS, Uint32 plane, Uint32 crtc, Uint32 enc, Uint32 conn)
{
	drm_pipe *pipe = calloc(1, sizeof(*pipe));
	if ( !pipe ) {
		SDL_SetError("Unable to allocate drm_pipe.\n");
		return 0;
	}

	pipe->plane = plane;
	pipe->crtc = crtc;
	pipe->encoder = enc;
	pipe->connector = conn;

    // We want to remember the pipe order, so save to last.
	if (drm_first_pipe) {
        drm_pipe *pipe_f = drm_first_pipe;
        while (pipe_f->next) {
            pipe_f = pipe_f->next;
        }

        pipe_f->next = pipe;
    }
    else {
        drm_first_pipe = pipe;
    }
    
	printf("Annotating pipe p: %d cr: %d e: %d con: %d\n", plane, crtc, enc, conn);
	return 1;
}

static Uint32 get_prop_ptrs(_THIS, drmModeObjectProperties **props, 
	drmModePropertyRes ***props_info, struct drm_prop_arg *p)
{
	drm_prop_storage *cur;
	for (cur = drm_first_prop_store; cur; cur = cur->next) {
		if (cur->obj_id == p->obj_id) {
			*props = cur->props;
			*props_info = cur->props_info;
			return cur->obj_type;
		}
	}

	return 0;
}

static int find_prop_info_idx(drmModeObjectProperties *props, 
	drmModePropertyRes **props_info, struct drm_prop_arg *p)
{
	for (int i = 0; i < props->count_props; i++) {
		if (!props_info[i])
			continue;
		if (strcmp(props_info[i]->name, p->name) == 0) {
			p->prop_id = props_info[i]->prop_id;
			return i;
		}
	}

	return -1;
}

static int set_property(_THIS, drmModeAtomicReq *req, struct drm_prop_arg *p)
{
	drmModeObjectProperties *props = NULL;
	drmModePropertyRes **props_info = NULL;
	
	// Try to acquire object
	if ( (p->obj_type = get_prop_ptrs(this, &props, &props_info, p)) == 0 ) {
		SDL_SetError("No known properties for object %d.\n", p->obj_id);
		return 0;
	}

	// If the specified object has no property, raise error.
	if ( !props ) {
		SDL_SetError("%s has no properties.\n", from_mode_object_type(p->obj_type));
		return 0;
	}

	Uint32 idx;
	if ( (idx = find_prop_info_idx(props, props_info, p)) < 0 ) {
		// If this is optional, don't raise an error on not finding the property.
		if (p->optional)
			return 1;

		// If this isn't optional, do raise error.
		SDL_SetError("%s has no property %s.\n", from_mode_object_type(p->obj_type),
			p->name);
		return 0;
	}

	// Finally try adding/setting the property
	printf("setting %lld to %s (%s, %d, %d).\n", p->value, p->name, from_mode_object_type(p->obj_type), p->obj_id, p->prop_id);
	if ( drmModeAtomicAddProperty(req, p->obj_id, p->prop_id, p->value) < 0 ) {
		printf("Failed to set %s property for %s, %s.\n", p->name, 
		    from_mode_object_type(p->obj_type), strerror(errno));
	}

	return 1;
}

static int _get_property(_THIS, struct drm_prop_arg *p, Uint64 *val)
{
	drmModeObjectProperties *props = NULL;
	drmModePropertyRes **props_info = NULL;
	
	// Try to acquire object
	if ( (p->obj_type = get_prop_ptrs(this, &props, &props_info, p)) == 0 ) {
		SDL_SetError("Could not find object.\n");
		return 0;
	}

	// If the specified object has no property, raise error.
	if ( !props || !props_info ) {
		SDL_SetError("%s has no properties.\n", from_mode_object_type(p->obj_type));
		return 0;
	}

	if ( (p->prop_id = find_prop_info_idx(props, props_info, p)) < 0 ) {
		SDL_SetError("%s has no property %s.\n", from_mode_object_type(p->obj_type),
			p->name);
		return 0;
	}

	// Finally try getting the property
	*val = props->prop_values[p->prop_id];
	return 1;
}

static int acquire_properties(_THIS, Uint32 id, Uint32 type)
{
	struct drm_prop_storage *store = calloc(1, sizeof(drm_prop_storage));
	store->props = drmModeObjectGetProperties(drm_fd, id, type); 
	if ( !store->props || store->props->count_props == 0 ) { 
		free(store);
		return 0;
	} 

	store->obj_id = id;
	store->obj_type = type;
	store->next = drm_first_prop_store;
	store->props_info = calloc(store->props->count_props, sizeof(*store->props_info)); 
	if ( !store->props_info ) {
		free(store);
		return 1;
	}

	for (int i = 0; i < store->props->count_props; i++) {
		store->props_info[i] = drmModeGetProperty(drm_fd, store->props->props[i]);
		if ( store->props_info[i]->count_values > 0 )
			printf(" * \"%s\": %lld\n", store->props_info[i]->name, store->props_info[i]->values[0]);
		else
			printf(" * \"%s\": %s\n", store->props_info[i]->name, "??");
	}

	drm_first_prop_store = store;
	return 1;
}

static int add_property(_THIS, drmModeAtomicReq *req, uint32_t obj_id,
			       const char *name, int opt, uint64_t value)
{
	struct drm_prop_arg p = {};

	p.obj_id = obj_id;
	strncpy(p.name, name, sizeof(p.name)-1);
	p.value = value;
	p.optional = opt;

	return set_property(this, req, &p);
}

static int get_property(_THIS, uint32_t obj_id, const char *name, uint64_t *value)
{
	struct drm_prop_arg p;

	p.obj_id = obj_id;
	strncpy(p.name, name, sizeof(p.name)-1);
	
	return _get_property(this, &p, value);
}

static int free_drm_prop_storage(_THIS)
{
	if ( !drm_first_prop_store )
		return 0;
	
	for (int i = 0; i < drm_first_prop_store->props->count_props; i++) {
		free(drm_first_prop_store->props_info[i]);
	}

	drmModeFreeObjectProperties(drm_first_prop_store->props);
	drm_prop_storage *next = drm_first_prop_store->next;
	free(drm_first_prop_store);
	drm_first_prop_store = next;

	return 1;
}

static int free_drm_pipe(_THIS)
{
	if ( !drm_first_pipe )
		return 0;

	drm_pipe *next = drm_first_pipe->next;
	free(drm_first_pipe);
	drm_first_pipe = next;
	return 1;
}