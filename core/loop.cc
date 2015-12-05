#include <loop.h>
#include <bind.h>
#include <boot.h>
#include <link.h>
#include <http.h>
#include <file.h>
#include <util.h>

#ifdef __cplusplus
extern "C" {
#endif
#include <lua/lauxlib.h>
#include <lua/lua.h>
#include <lua/lualib.h>
#ifdef __cplusplus
} //extern "C"
#endif //__cplusplus

#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define RESIDENT_LUA_ERROR  1
#define RESIDENT_TOP  1

struct loop_data 
{
    lua_State *_L;

    int _c2l_loop;
    int _c2l_stop;
    int _c2l_event;
	int _err_flag;

    loop_data() : _L(nullptr),
		_c2l_loop(LUA_NOREF), _c2l_stop(LUA_NOREF), _c2l_event(LUA_NOREF), _err_flag(0)
    {
    }
    ~loop_data()
    {
        if (nullptr != _L) {
            lua_close(_L);
        }
    }
};
static loop_data *_D = nullptr;

static int
__lua_panic(lua_State *L) 
{
    const char * err = lua_tostring(L,-1);
    LOGF("%s", err);
	_D->_err_flag++;
    return 0;
}

static int
__lua_error(lua_State *L) {
    const char *msg = lua_tostring(L, 1);
    if (nullptr != msg) {
        luaL_traceback(L, L, msg, 1);
    } else if (!lua_isnoneornil(L, 1) && !luaL_callmeta(L, 1, "__tostring")) {
        lua_pushliteral(L, "(no error message)");
    }

    return 1;
}

static int
__lua_print(lua_State *L)
{
    int n = lua_gettop(L);
    for (int i = 1; i <= n; ++i) {
        const char *z = lua_tostring(L, i);
        if (nullptr != z) {
            LOGD("lua-print\t%s", z);
        } else {
            LOGD("lua-print\t%s", lua_typename(L, lua_type(L, i)));
        }
    }

    return 0;
}

static void
__lua_call(lua_State *L, int n, int r) 
{
    //assert((RESIDENT_TOP + 1 + n) == lua_gettop(L));
	_D->_err_flag++;

    int err = lua_pcall(L, n, r, 1);
    switch(err) {
    case LUA_OK:
		_D->_err_flag--;
        break;
    case LUA_ERRRUN:
        LOGE("lua-call\tLUA_ERRRUN\t%s", lua_tostring(L, -1));
        break;
    case LUA_ERRMEM:
        LOGE("lua-call\tLUA_ERRMEM\t%s", lua_tostring(L, -1));
        break;
    case LUA_ERRERR:
        LOGE("lua-call\tLUA_ERRERR\t%s", lua_tostring(L, -1));
        break;
    case LUA_ERRGCMM:
        LOGE("lua-call\tLUA_ERRGCMM\t%s", lua_tostring(L, -1));
        break;
    default:
        LOGE("lua-call\tLUA_UNKNOWN");
        break;
    }
}

static void
__lua_boot(void)
{
    assert(nullptr != _D);

    int err = luaL_loadbufferx(_D->_L, __LUA_BOOTSTRAP, sizeof(__LUA_BOOTSTRAP) - 1, "lua_boot", "bt");
    if (LUA_OK == err) {
        __lua_call(_D->_L, 0, 0);
    } else {
        LOGF("lua-boot\t%s %d", lua_tostring(_D->_L, -1), err);
    }
}

static int
__l2c_bind(lua_State *L)
{
    assert(1 == lua_gettop(L));
    assert(lua_istable(L, -1));

    if (LUA_NOREF != _D->_c2l_loop) {
        luaL_unref(L, LUA_REGISTRYINDEX, _D->_c2l_loop);
		_D->_c2l_loop = LUA_NOREF;
    }
	if (LUA_NOREF != _D->_c2l_stop) {
        luaL_unref(L, LUA_REGISTRYINDEX, _D->_c2l_stop);
		_D->_c2l_stop = LUA_NOREF;
    }
	if (LUA_NOREF != _D->_c2l_event) {
        luaL_unref(L, LUA_REGISTRYINDEX, _D->_c2l_event);
		_D->_c2l_event = LUA_NOREF;
    }

    lua_getfield(L, 1, "loop");
    if (lua_isfunction(L, -1)) {
        _D->_c2l_loop = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    lua_getfield(L, 1, "stop");
	if (lua_isfunction(L, -1)) {
		_D->_c2l_stop = luaL_ref(L, LUA_REGISTRYINDEX); 
	}
    lua_getfield(L, 1, "event");
    if (lua_isfunction(L, -1)) {
		_D->_c2l_event = luaL_ref(L, LUA_REGISTRYINDEX);
	}

    return 0;
}

static int
__l2c_call(lua_State *L)
{
    const char *type = lua_tostring(L, 1);
    const char *data = lua_tostring(L, 2);
    const char *sign = lua_tostring(L, 3);

    return bind_call(type, data, sign, L);
}

static int
__l2c_read(lua_State *L)
{
	int ret = 0;

	size_t plen = 0;
	const char *path = lua_tolstring(L, 1, &plen);
	if (nullptr != path && plen > 0) {
		ret = bind_read(path, plen, L);
	}

	return ret;
}

static int
__luaopen_bind(lua_State *L)
{
    luaL_Reg r[] = {
        { "bind", __l2c_bind },
        { "call", __l2c_call },
        { "read", __l2c_read },
        { nullptr, nullptr },
    };
    luaL_newlib(L, r);
    return 1;
}

int
loop_start(const char *home)
{
    if (nullptr != _D) {
        return 0;
    }

    _D = new loop_data();
    _D->_L = luaL_newstate();
    lua_atpanic(_D->_L, __lua_panic);
    luaL_openlibs(_D->_L);

    lua_pushcfunction(_D->_L, __lua_error);
    luaL_Reg g[] = {
        { "print", __lua_print },
        { nullptr, nullptr },
    };
    lua_pushglobaltable(_D->_L);
    luaL_setfuncs(_D->_L, g, 0);

	size_t hlen = 0;
	if (nullptr == home) {
		home = "./";
		hlen = sizeof("./") - 1;
	} else {
		hlen = strnlen(home, PATH_SIZE);
	}
	lua_pushlstring(_D->_L, "HOME", sizeof("HOME") - 1);
	lua_pushlstring(_D->_L, home, hlen);
	lua_rawset(_D->_L, -3);
    //assert(RESIDENT_LUA_ERROR == lua_gettop(_D->_L));

    util_init(_D->_L);
    file_init(_D->_L, home, hlen);
    http_init(_D->_L);
    link_init(_D->_L);
    luaL_requiref(_D->_L, "cbind", __luaopen_bind, 0);
    //assert(RESIDENT_LUA_ERROR == lua_gettop(_D->_L));

    __lua_boot();

    lua_settop(_D->_L, RESIDENT_TOP);
    return 1;
}

int
loop_update(void)
{
	if (nullptr == _D) {
		return 0;
	}
	LOGI("loop-update");

	link_loop(_D->_L);

	size_t n = http_loop(_D->_L);

	if (0 == _D->_err_flag) {
		if (LUA_NOREF != _D->_c2l_loop) {
			lua_rawgeti(_D->_L, LUA_REGISTRYINDEX, _D->_c2l_loop);
			__lua_call(_D->_L, 0, 1);
		}
	} else {
		if (0 == n) {
			bind_call("loop.restart", "5000", "");
		}
	}

	lua_settop(_D->_L, RESIDENT_TOP);

	return 0;
}

void
loop_event(const char *type, const char *data, const char *sign)
{
    if (nullptr != _D && LUA_NOREF != _D->_c2l_event) {

        lua_rawgeti(_D->_L, LUA_REGISTRYINDEX, _D->_c2l_event);
        lua_pushstring(_D->_L, type);
        lua_pushstring(_D->_L, data);
        lua_pushstring(_D->_L, sign);
        __lua_call(_D->_L, 3, 0);

        lua_settop(_D->_L, RESIDENT_TOP);
    }
}

void
loop_call(lua_State *L, int n, int r)
{
	__lua_call(L, n, r);
}

void
loop_stop(void)
{
    if (nullptr != _D) {
        if (LUA_NOREF != _D->_c2l_stop) {
            lua_rawgeti(_D->_L, LUA_REGISTRYINDEX, _D->_c2l_stop);
            __lua_call(_D->_L, 0, 1);
            lua_settop(_D->_L, RESIDENT_TOP);
        }

		link_fini(_D->_L);
        http_fini(_D->_L);
        file_fini(_D->_L);
        util_fini(_D->_L);

        delete _D; _D = nullptr;
    }
}
