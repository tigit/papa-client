#ifndef __PD_LOOP_H__
#define __PD_LOOP_H__

struct lua_State;

int
loop_start(const char *home);

int
loop_update(void);

void
loop_event(const char *type, const char *data, const char *sign);

void
loop_call(lua_State *L, int n, int r);

void
loop_stop(void);

#endif //__PD_LOOP_H__
