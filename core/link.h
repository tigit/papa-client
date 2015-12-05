#ifndef __PD_LINK__
#define __PD_LINK__

#define LINK_PORT 9527

struct lua_State;

void
link_init(lua_State *L);

void
link_loop(lua_State *L);

void
link_fini(lua_State *L);

#endif//__PD_LINK__
