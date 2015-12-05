#include <bind.h>
#include <loop.h>
#include <file.h>

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
#include <assert.h>
#include <windows.h>
#include <map>

struct bind_data
{
	unsigned int _tick;
	unsigned int _quit;
	unsigned int _restart;

	bind_data() : _tick(10), _quit(0), _restart(0)
	{
	}
};

static bind_data *_B = nullptr;

int
bind_call(const char *type, const char *data, const char *sign, lua_State *L)
{
	static std::map<std::string, std::string> tmp_values;

	int ret = 0;
	if (0 == strcmp(type, "data.get_devinfo")) {
		lua_pushstring(L, "{\"id\":\"pc-test\"}");
		ret = 1;
	} else if (0 == strcmp(type, "data.get_pkginfo")) {
		lua_pushstring(L, "{\"version\":1, \"config\":{\"server_url\":\"http://server.thedawens.net:8080/do\", \"static_url\":\"http://static.thedawens.net:8080/\", \"auth_token\":\"auth_token.txt\"}}");
		ret = 1;
	} else if (0 == strcmp(type, "data.get_tmpvalue")) {
		std::string val = tmp_values[std::string(data)];
		lua_pushstring(L, val.data());
		ret = 1;
	} else if (0 == strcmp(type, "data.set_tmpvalue")) {
		tmp_values[std::string(data)] = std::string(sign);
	} else if (0 == strcmp(type, "loop.set_tick")) {
		_B->_tick = atoi(data);
	} else if (0 == strcmp(type, "loop.restart")) {
		_B->_restart = (nullptr == data ? 1 : atoi(data));
	} else if (0 == strcmp(type, "loop.exit")) {
		_B->_quit = 1;
	}

	return ret;
}

int
bind_read(const char *path, size_t plen, lua_State *L)
{
	char temp[512];
	_snprintf(temp, sizeof(temp), "../assets/%s", path);

	FILE *asset = ::fopen(temp, "rb");
	if (nullptr == asset) {
		int err = errno;
		return 0;
	}

	char *data = nullptr;
	::fseek(asset, 0, SEEK_END);
	size_t size = ::ftell(asset);
	::fseek(asset, 0, SEEK_SET);
	if (size > 0) {
		data = new char[size];
		if (size != ::fread(data, 1, size, asset)) {
			size = 0;
		}
	}

	lua_pushlstring(L, data, size);

	::fclose(asset);
	if (nullptr != data) {
		delete[] data;
	}

	return 1;
}

int
main(void)
{
	while (true) {
		//_CrtSetBreakAlloc(1553);
		_CrtSetDbgFlag(_CrtSetDbgFlag(_CRTDBG_REPORT_FLAG) | _CRTDBG_LEAK_CHECK_DF);
		_B = new bind_data();

		loop_start("./tmp/data/");

		while (0 == _B->_quit && 0 == _B->_restart) {
			loop_update();
			::Sleep(_B->_tick);
		}

		loop_stop();

		if (_B->_quit > 0) {
			break;
		}
		if (_B->_restart > 0) {
			unsigned int w = _B->_restart;
			delete _B; _B = nullptr;
			::Sleep(w);
		}
		_CrtDumpMemoryLeaks();
	}

	if (nullptr != _B) {
		delete _B; _B = nullptr;
	}

	return 0;
}