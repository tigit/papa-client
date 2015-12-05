#ifndef __PD_HTTP__
#define __PD_HTTP__

#include <stddef.h>

#define URL_SIZE 512
#define REQ_MAX 256

struct lua_State;

void
http_init(lua_State *L);

size_t
http_loop(lua_State *L);

int
http_push(char *url, size_t ulen, char *log, size_t llen);

void
http_fini(lua_State *L);

#endif // __PD_HTTP__
