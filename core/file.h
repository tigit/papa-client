#ifndef __PD_FILE__
#define __PD_FILE__

#include <stddef.h>
#include <time.h>

enum {
	LOG_DEBUG = 0,
	LOG_INFO = 1,
	LOG_WARN = 2,
	LOG_ERROR = 3,
	LOG_FATAL = 4,
	LOG_MAX
};
#define LOG_SPLIT ' '
#define PATH_SIZE  256
#define LOGD(...)  file_clog(LOG_DEBUG, __VA_ARGS__)
#define LOGI(...)  file_clog(LOG_INFO, __VA_ARGS__)
#define LOGW(...)  file_clog(LOG_WARN, __VA_ARGS__)
#define LOGE(...)  file_clog(LOG_ERROR, __VA_ARGS__)
#define LOGF(...)  file_clog(LOG_FATAL, __VA_ARGS__)

struct lua_State;

void
file_init(lua_State *L, const char *home, size_t hlen);

void
file_fini(lua_State *L);

void*
file_open(char *path, size_t plen, const char *mode);

int
file_utime(char *path, size_t plen, time_t mtime, time_t atime);

int
file_clog(int lv, const char *fmt, ...);

#endif // __PD_FILE__
