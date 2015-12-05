#ifndef __PD_UTIL__
#define __PD_UTIL__

#include <stddef.h>

struct lua_State;

void
util_init(lua_State *L);

char*
util_url_encode(const char *src, size_t slen, size_t *dlen);

size_t
util_url_decode(char *src, size_t slen);

void
util_fini(lua_State *L);

#endif // __PD_UTIL__
