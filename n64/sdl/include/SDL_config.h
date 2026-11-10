#ifndef _SDL_config_h
#define _SDL_config_h
/* SDL 1.2 config para N64 (libdragon/newlib) - solo para medir */
#include "SDL_platform.h"
#define SDL_HAS_64BIT_TYPE 1
#define SDL_BYTEORDER 4321
#define HAVE_LIBC 1
#define HAVE_STDIO_H 1
#define STDC_HEADERS 1
#define HAVE_STDLIB_H 1
#define HAVE_STDARG_H 1
#define HAVE_STRING_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_STDINT_H 1
#define HAVE_CTYPE_H 1
#define HAVE_MATH_H 1
#define HAVE_MALLOC 1
#define HAVE_CALLOC 1
#define HAVE_REALLOC 1
#define HAVE_FREE 1
#define HAVE_MEMSET 1
#define HAVE_MEMCPY 1
#define HAVE_MEMMOVE 1
#define HAVE_MEMCMP 1
#define HAVE_STRLEN 1
#define HAVE_STRDUP 1
#define HAVE_STRCHR 1
#define HAVE_STRRCHR 1
#define HAVE_STRSTR 1
#define HAVE_STRTOL 1
#define HAVE_STRTOD 1
#define HAVE_ATOI 1
#define HAVE_STRCMP 1
#define HAVE_STRNCMP 1
#define HAVE_SNPRINTF 1
#define HAVE_VSNPRINTF 1
#define SDL_CDROM_DISABLED 1
#define SDL_LOADSO_DISABLED 1
#define SDL_THREADS_DISABLED 1
#define SDL_TIMERS_DISABLED 1
#define SDL_AUDIO_DRIVER_DUMMY 1
#define SDL_VIDEO_DRIVER_DUMMY 1
#define SDL_JOYSTICK_DISABLED 0
#endif
