#include <http.h>
#include <loop.h>
#include <file.h>

#ifdef __cplusplus
extern "C" {
#endif
#include <lua/lauxlib.h>
#include <lua/lua.h>
#include <lua/lualib.h>
#include <curl.h>
#ifdef __cplusplus
} //extern "C"
#endif //__cplusplus

#include <md5.h>
#include <string>
#include <list>

struct http_task;

struct http_data
{
	CURLM  *_M;
	std::list<http_task*> _tasks;

	http_data(void) : _M(nullptr)
	{
	}
};

static http_data *_H = nullptr;

struct http_task
{
	enum { _GET, _POST, _FGET, _FPUT, };

	int          _type;
	CURL        *_easy;
	char         _path[PATH_SIZE];
	int          _plen;
	time_t       _mtime;
	FILE        *_file;
	std::string *_data;
	int          _lfun;

	http_task(void) : _type(0), _easy(nullptr), _plen(0), _mtime(0), _file(nullptr), _data(nullptr), _lfun(LUA_NOREF)
	{
		this->_path[0] = '\0';
	}
};

static size_t 
__curl_write_function(char *ptr, size_t size, size_t nmemb, void *userdata)
{
	auto task = (http_task*)userdata;

	size_t ret = size * nmemb;

	if (nullptr != ptr && nullptr != task)
	{
		if (http_task::_FPUT != task->_type) {
			if (nullptr == task->_file && task->_plen > 0) {
				task->_file = (FILE*)file_open(task->_path, task->_plen, "wb");
				if (nullptr == task->_file) {
					ret = 0;
				}
			}

			if (nullptr != task->_file)
			{
				ret = ::fwrite(ptr, size, nmemb, task->_file);
			}
		}

		if (nullptr != task->_data)
		{
			task->_data->append(ptr, size * nmemb);
		}
	}

	return ret;
}

static size_t 
__curl_fput_function(char *buffer, size_t size, size_t nmemb, void *userdata)
{
	size_t ret = 0;
	auto task = (http_task*)userdata;

	if (nullptr != buffer && nullptr != task) {
		if (nullptr != task->_file) {
			ret = ::fread(buffer, size, nmemb, task->_file);
		}
	}

	return ret;
}

static inline http_task*
__curl_easy(void)
{
	if (_H->_tasks.size() > REQ_MAX) {
		LOGW("curl req limit %u", _H->_tasks.size());
		return nullptr;
	}

	http_task *task = new http_task();

	CURL * easy = curl_easy_init();
	curl_easy_setopt(easy, CURLOPT_FORBID_REUSE, 0L);
	curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(easy, CURLOPT_NOPROGRESS, 1L);
	curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, 10L);
	curl_easy_setopt(easy, CURLOPT_TIMEOUT, 30L);
	curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 0L);
	curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, 0L);
	curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(easy, CURLOPT_HEADER, 0L);

	curl_easy_setopt(easy, CURLOPT_READFUNCTION, NULL);
	curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, __curl_write_function);
	curl_easy_setopt(easy, CURLOPT_WRITEDATA, (void*)task);
	curl_easy_setopt(easy, CURLOPT_PRIVATE, task);

	task->_easy = easy;
	return task;
}

static inline bool
__curl_execute(http_task *task, const char *url)
{
	bool ret = false;

	curl_easy_setopt(task->_easy, CURLOPT_URL, url);
	if (CURLM_OK == curl_multi_add_handle(_H->_M, task->_easy)) {
		int easy_count = 0;
		curl_multi_perform(_H->_M, &easy_count);
		_H->_tasks.push_back(task);
		ret = true;
	}

	return ret;
}

static void
__done_request(lua_State *L, http_task *task, bool succ)
{
	if (nullptr != task->_file) {
		::fclose(task->_file);
		task->_file = nullptr;
		if (succ) {
			if (task->_mtime > 0) {
				file_utime(task->_path, task->_plen, task->_mtime, task->_mtime);
			}
		} else {
			::remove(task->_path);
		}
	}

	if (LUA_NOREF == task->_lfun) {
		return;
	}

	lua_rawgeti(L, LUA_REGISTRYINDEX, task->_lfun);
	lua_pushlightuserdata(L, (void*)task);
	int n = 1;

	if (succ && nullptr != task->_data) {
		lua_pushlstring(L, task->_data->data(), task->_data->length());
		n += 1;
	}

	loop_call(L, n, 0);
}

static void
__stop_request(lua_State *L, http_task *task)
{
	if (nullptr != task->_easy) {
		curl_multi_remove_handle(_H->_M, task->_easy);
		curl_easy_cleanup(task->_easy);
		task->_easy = nullptr;
	}

	if (nullptr != task->_file) {
		::fclose(task->_file);
		task->_file = nullptr;
	}

	if (nullptr != task->_data) {
		delete task->_data;
		task->_data = nullptr;
	}

	if (LUA_NOREF != task->_lfun) {
		luaL_unref(L, LUA_REGISTRYINDEX, task->_lfun);
		task->_lfun = LUA_NOREF;
	}

	delete task;
}

static int 
__start_get(lua_State *L)
{
	http_task *task = __curl_easy();
	if (nullptr == task) { return 0; }

	do {
		task->_type = http_task::_GET;
		curl_easy_setopt(task->_easy, CURLOPT_POST, 0L);

		size_t ulen = 0;
		const char *ustr = lua_tolstring(L, 2, &ulen);
		if (nullptr == ustr || 0 == ulen) {
			break;
		}

		if (lua_istable(L, 3)) {
			char temp[URL_SIZE];
			if (ulen > sizeof(temp) - 2) {
				break;
			}
			memcpy(temp, ustr, ulen);

			char p = '?';
			lua_pushnil(L);
			while (lua_next(L, 3)) {
				size_t kl = 0, al = 0;
				const char *key = lua_tolstring(L, -2, &kl);
				const char *arg = lua_tolstring(L, -1, &al);
				if (nullptr != key && nullptr != arg && kl > 0 && al > 0 && ulen + kl + al + 2 < sizeof(temp)) {
					temp[ulen] = p; p = '&'; ++ulen;
					memcpy(temp + ulen, key, kl); ulen += kl;
					temp[ulen] = '='; ++ulen;
					memcpy(temp + ulen, arg, al); ulen += al;
				}
				lua_pop(L, 1);
			}
			temp[ulen] = '\0';
			ustr = temp;
		}

		task->_data = new std::string();

		if (lua_isfunction(L, 4)) {
			task->_lfun = luaL_ref(L, LUA_REGISTRYINDEX);
		}

		if (!__curl_execute(task, ustr)) {
			break;
		}

		lua_pushlightuserdata(L, (void*)task); return 1;
	} while (false);

	__stop_request(L, task); return 0;
}

static int 
__start_post(lua_State *L)
{
	http_task *task = __curl_easy();
	if (nullptr == task) { return 0; }

	do {
		task->_type = http_task::_POST;
		curl_easy_setopt(task->_easy, CURLOPT_POST, 1L);

		size_t ulen = 0;
		const char *ustr = lua_tolstring(L, 2, &ulen);
		if (nullptr == ustr || 0 == ulen) {
			break;
		}

		size_t plen = 0;
		const char *pstr = lua_tolstring(L, 3, &plen);
		if (nullptr != pstr && plen > 0) {
			curl_easy_setopt(task->_easy, CURLOPT_POSTFIELDS, pstr);
			curl_easy_setopt(task->_easy, CURLOPT_POSTFIELDSIZE, (long)plen);
		}

		task->_data = new std::string();

		if (lua_isfunction(L, 4)) {
			task->_lfun = luaL_ref(L, LUA_REGISTRYINDEX);
		}

		if (!__curl_execute(task, ustr)) {
			break;
		}

		lua_pushlightuserdata(L, (void*)task); return 1;
	} while (false);

	__stop_request(L, task); return 0;
}

static int 
__start_fget(lua_State *L)
{
	http_task *task = __curl_easy();
	if (nullptr == task) { return 0; }

	do {
		task->_type = http_task::_FGET;
		curl_easy_setopt(task->_easy, CURLOPT_POST, 0L);

		size_t ulen = 0;
		const char *ustr = lua_tolstring(L, 2, &ulen);
		if (nullptr == ustr || 0 == ulen) {
			break;
		}

		size_t plen = 0;
		const char *path = lua_tolstring(L, 3, &plen);
		if (nullptr == path || 0 == plen || plen > sizeof(task->_path)) {
			break;
		}
		memcpy(task->_path, path, plen); task->_path[plen] = '\0'; task->_plen = plen;

		task->_mtime = (time_t)lua_tointeger(L, 4);

		if (lua_isfunction(L, 5)) {
			task->_lfun = luaL_ref(L, LUA_REGISTRYINDEX);
		}

		if (!__curl_execute(task, ustr)) {
			break;
		}

		lua_pushlightuserdata(L, (void*)task); return 1;
	} while (false);

	__stop_request(L, task);

	return 0;
}

static int 
__start_fput(lua_State *L)
{
	http_task *task = __curl_easy();
	if (nullptr == task) { return 0; }

	do {
		task->_type = http_task::_FPUT;
		curl_easy_setopt(task->_easy, CURLOPT_POST, 0L);
		curl_easy_setopt(task->_easy, CURLOPT_READFUNCTION, __curl_fput_function);
		curl_easy_setopt(task->_easy, CURLOPT_READDATA, (void*)task);

		curl_easy_setopt(task->_easy, CURLOPT_UPLOAD, 1L);

		size_t ulen = 0;
		const char *ustr = lua_tolstring(L, 2, &ulen);
		if (nullptr == ustr || 0 == ulen) {
			break;
		}

		size_t plen = 0;
		const char *path = lua_tolstring(L, 3, &plen);
		if (nullptr == path || 0 == plen || plen > sizeof(task->_path)) {
			break;
		}
		memcpy(task->_path, path, plen); task->_path[plen] = '\0'; task->_plen = plen;
		task->_file = (FILE*)file_open(task->_path, task->_plen, "ab");
		if (nullptr == task->_file) {
			break;
		}
		::fseek(task->_file, 0, SEEK_END);
		long fsiz = ::ftell(task->_file);
		::fseek(task->_file, 0, SEEK_SET);
		curl_easy_setopt(task->_easy, CURLOPT_INFILESIZE_LARGE, (curl_off_t)fsiz);

		if (lua_isfunction(L, 4)) {
			task->_lfun = luaL_ref(L, LUA_REGISTRYINDEX);
		}

		if (!__curl_execute(task, ustr)) {
			break;
		}

		lua_pushlightuserdata(L, (void*)task); return 1;
	} while (false);

	__stop_request(L, task);

	return 0;
}

static int 
__luaopen_http(lua_State *L)
{
	luaL_Reg r[] = {
		{ "get", __start_get },
		{ "post", __start_post },
		{ "fget", __start_fget },
		{ "fput", __start_fput },
		{ nullptr, nullptr },
	};
	luaL_newlib(L, r);
	return 1;
}

void
http_init(lua_State *L)
{
	if (nullptr != _H) {
		return;
	}

	_H = new http_data();
	_H->_M = curl_multi_init();

    luaL_requiref(L, "chttp", __luaopen_http, 0);
}

size_t
http_loop(lua_State *L)
{
	if (nullptr == _H) {
		return 0;
	}

	long timeout_ms = 0;
	curl_multi_timeout(_H->_M, &timeout_ms);
	if (timeout_ms <= 0 || timeout_ms > 10)
	{
		timeout_ms = 1;
	}
	struct timeval tv;
	tv.tv_sec = timeout_ms / 1000;
	tv.tv_usec = (timeout_ms % 1000) * 1000;

	fd_set fdr, fdw, fde;
	FD_ZERO(&fdr); FD_ZERO(&fdw); FD_ZERO(&fde); 
	int maxfd = -1;
	curl_multi_fdset(_H->_M, &fdr, &fdw, &fde, &maxfd);

	select(maxfd+1, &fdr, &fdw, &fde, &tv);

	int easy_count = 0;
	for (int i = 0; i < 256 && CURLM_CALL_MULTI_PERFORM == curl_multi_perform(_H->_M, &easy_count) && easy_count > 0; ++i);

	CURLMsg *msg = nullptr;
	int num = 0;
	for (int i = 0; i < 256 && nullptr != (msg = curl_multi_info_read(_H->_M, &num)); ++i) 
	{
		if (CURLMSG_DONE == msg->msg) 
		{
			http_task *task = nullptr;
			curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, (char**)&task);
			if (nullptr != task)
			{
				bool ok = false;
				if (CURLE_OK == msg->data.result) 
				{
					long code = 0;
					curl_easy_getinfo(task->_easy, CURLINFO_RESPONSE_CODE, &code);
					ok = (code < 400);
				}
				__done_request(L, task, ok);
				__stop_request(L, task);
				_H->_tasks.remove(task);
				break;
			}
		}
	}

	return _H->_tasks.size();
}

int
http_push(char *url, size_t ulen, char *log, size_t llen)
{
	if (nullptr == _H || _H->_tasks.size() > REQ_MAX) {
		return 0;
	}

	http_task *task = __curl_easy();
	if (nullptr == task) { return 0; }

	curl_easy_setopt(task->_easy, CURLOPT_POST, 1L);

	curl_easy_setopt(task->_easy, CURLOPT_POSTFIELDS, log);
	curl_easy_setopt(task->_easy, CURLOPT_POSTFIELDSIZE, (long)llen);

	if (!__curl_execute(task, url)) {
		__stop_request(nullptr, task);
		return 0;
	}

	return 1;
}

void
http_fini(lua_State *L)
{
	if (nullptr == _H) {
		return;
	}

	for (auto it : _H->_tasks) {
		__stop_request(L, it);
	}

	curl_multi_cleanup(_H->_M);

	delete _H; _H = nullptr;
}
