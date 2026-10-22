/* A stand-in for libcanberra's API (not built: it needs a sound server or ALSA at run time and this machine's sound goes another way):
   the Notifications module asks it to preview a sound, every call fails with "not supported" and the module logs a warning. */
#pragma once
#include <stdarg.h>
#define CA_SUCCESS 0
#define CA_ERROR_NOTSUPPORTED (-1)
#define CA_PROP_APPLICATION_NAME "application.name"
#define CA_PROP_APPLICATION_ID "application.id"
#define CA_PROP_APPLICATION_ICON_NAME "application.icon_name"
#define CA_PROP_EVENT_ID "event.id"
#define CA_PROP_MEDIA_FILENAME "media.filename"
#define CA_PROP_CANBERRA_XDG_THEME_NAME "canberra.xdg-theme.name"
#define CA_PROP_CANBERRA_CACHE_CONTROL "canberra.cache-control"
typedef struct ca_context ca_context;
typedef struct ca_proplist ca_proplist;
typedef void (*ca_finish_callback_t)(ca_context *, unsigned, int, void *);
static inline int ca_context_create(ca_context **c) { *c = 0; return CA_ERROR_NOTSUPPORTED; }
static inline int ca_context_change_props(ca_context *, ...) { return CA_ERROR_NOTSUPPORTED; }
static inline int ca_context_play_full(ca_context *, unsigned, ca_proplist *, ca_finish_callback_t, void *) { return CA_ERROR_NOTSUPPORTED; }
static inline int ca_context_cancel(ca_context *, unsigned) { return CA_SUCCESS; }
static inline int ca_context_destroy(ca_context *) { return CA_SUCCESS; }
static inline int ca_proplist_create(ca_proplist **p) { *p = 0; return CA_SUCCESS; }
static inline int ca_proplist_sets(ca_proplist *, const char *, const char *) { return CA_SUCCESS; }
static inline int ca_proplist_destroy(ca_proplist *) { return CA_SUCCESS; }
static inline const char *ca_strerror(int) { return "libcanberra is not built here"; }
