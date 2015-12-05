#ifndef __PD_BIND_H__
#define __PD_BIND_H__

#include <stddef.h>

struct lua_State;

int 
bind_call(const char *type, const char *data, const char *sign, lua_State *L = nullptr);

int
bind_read(const char *path, size_t plen, lua_State *L = nullptr);

#endif //__PD_BIND_H__
