#include <file.h>
#include <http.h>

#ifdef __cplusplus
extern "C" {
#endif
#include <lua/lauxlib.h>
#include <lua/lua.h>
#include <lua/lualib.h>
#ifdef __cplusplus
} //extern "C"
#endif //__cplusplus

#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <sys/utime.h>
#include <windows.h>
#else //_WIN32
#include <dirent.h>
#include <utime.h>
#include <unistd.h>
#endif

#ifdef ANDROID
#include <android/log.h>
#define lprint(...)  __android_log_print(ANDROID_LOG_ERROR, "daemon", "%s", __VA_ARGS__)
#else //ANDROID
#define lprint(...)  printf("%s", __VA_ARGS__)
#endif//ANDROID

#ifdef _WIN32
#ifndef S_ISDIR
#define S_ISDIR(mode) ((mode) & _S_IFDIR)
#endif
#ifndef S_ISREG
#define S_ISREG(mode) ((mode) & _S_IFREG)
#endif
#ifndef S_ISLNK
#define S_ISLNK(mode) (0)
#endif
#ifndef S_ISSOCK
#define S_ISSOCK(mode) (0)
#endif
#ifndef S_ISFIFO
#define S_ISFIFO(mode) (0)
#endif
#ifndef S_ISCHR
#define S_ISCHR(mode) ((mode) & _S_IFCHR)
#endif
#ifndef S_ISBLK
#define S_ISBLK(mode) (0)
#endif
#define S_IRWXU 0
#define S_IRWXG 0
#define S_IRWXO 0

#define utime _utime
#define mkdir(a, b) _mkdir(a)
#define rmdir _rmdir
#define umask _umask
#define utimbuf _utimbuf
#define stat _stati64
#define unlink _unlink
typedef unsigned short mode_t;
#endif //_WIN32

static const char  *__DIR_METATABLE = "__DIR_METATABLE__";
static const size_t __LOG_HEAD_SIZE = 17;

struct file_data 
{
	char   _home[PATH_SIZE];
	char   _temp[PATH_SIZE];
	size_t _hlen;

	size_t _lmask;
	FILE  *_lfile;
	char   _lpath[PATH_SIZE];
	size_t _lplen;
	char   _lpush[PATH_SIZE];
	size_t _lulen;
	char   _lbuff[1 << 16];
	size_t _lblen;
	char   _ldate[8]; // "yyyymmdd"

	file_data() : _hlen(0), _lmask(0xff), _lfile(nullptr), _lplen(0), _lulen(0), _lblen(0)
	{
		this->_home[0] = '\0';
		this->_temp[0] = '\0';
		this->_lpath[0] = '\0';
		this->_lpush[0] = '\0';
		this->_lbuff[0] = '\0';
		this->_ldate[0] = '\0';
	}

	~file_data()
	{
		if (nullptr != this->_lfile) {
			::fclose(this->_lfile);
		}
	}
};

static file_data * _F = nullptr;

static inline char*
__file_fullpath(char *path, size_t plen, size_t *flen)
{
	char *fp = (char*)path;
	size_t fl = plen;

	if ('/' != path[0]) {
		fp = _F->_temp;
		fl = _F->_hlen;
		if (fl + plen < sizeof(_F->_temp)) {
			memcpy(fp + fl, path, plen); fl += plen;
			_F->_temp[fl] = '\0';
		}
	}

	if (nullptr != flen) { *flen = fl; }
	return fp;
}

static inline size_t
__file_dirname(const char *path, size_t plen)
{
	if (nullptr == path || 0 == plen) {
		return 0;
	}

	const char *h = path, *t = path + plen - 1;
	while (t > h && '/' == *t) --t;
	while (t > h && '/' != *t) --t;
	while (t > h && '/' == *t) --t;

	return t > h ? (t - h + 1) : ('/' == *t ? 1 : plen);
}

static inline size_t
__file_basename(const char *path, size_t plen)
{
	if (nullptr == path || 0 == plen) {
		return plen;
	}

	const char *h = path, *t = path + plen - 1;
	while (t > h && '/' != *t) --t;

	return t - h;
}

static inline int
__file_mkdir(char *path, size_t plen)
{
	if (nullptr == path || 0 == plen) {
		return 0;
	}

	int ret = 1;
	char *c = path + 1, *e = path + plen;
	while (c < e)
	{
		if ('/' == *c && '/' != *(c - 1)) 
		{
			*c = '\0';
			int r = mkdir(path, S_IRWXU | S_IRWXG | S_IRWXO);
			if (0 != r && (EEXIST != errno))
			{
				ret = 0; break;
			}
			*c = '/';
		}
		++c;
	}

	if (1 == ret && '/' != *(c - 1)) {
		char t = *c; *c = '\0';
		int r = mkdir(path, S_IRWXU | S_IRWXG | S_IRWXO); *c = t;
		if (0 != r && (EEXIST != errno))
		{
			ret = 0;
		}
	}

	return ret;
}

void
__rmdir_r(char *path, size_t plen)
{
	struct stat st;
	if (0 == stat(path, &st)) {
		if (S_ISDIR(st.st_mode)) {
#ifdef _WIN32
			struct _finddata_t fd;
			char *ft = path + plen;
			char kp[] = { ft[0], ft[1], ft[2] };
			ft[0] = '/'; ft[1] = '*'; ft[2] = '\0'; 
			long fh = ::_findfirst(path, &fd);
			ft[0] = kp[0]; ft[1] = kp[1]; ft[2] = kp[2]; 
			if (-1 != fh) {
				do {
					if (0 != strcmp(".", fd.name) && 0 != strcmp("..", fd.name) ) {
						size_t dlen = strlen(fd.name);
						size_t nlen = plen + dlen + 1;
						assert(nlen < PATH_SIZE);
						if (nlen < PATH_SIZE) {
							path[plen] = '/';
							memcpy(path + plen + 1, fd.name, dlen);
							path[nlen] = '\0';
							__rmdir_r(path, nlen);
						}
					}
				} while (0 == ::_findnext(fh, &fd));

				::_findclose(fh);
			}
#else //_WIN32
			DIR *dp = ::opendir(path);
			if (nullptr != dp) {
				dirent *ep;
				while (nullptr != (ep = ::readdir(dp))) {
					if (0 != strcmp(".", ep->d_name) && 0 != strcmp("..", ep->d_name)) {
						size_t dlen = strlen(ep->d_name);
						size_t nlen = plen + dlen + 1;
						assert(nlen < PATH_SIZE);
						if (nlen < PATH_SIZE) {
							path[plen] = '/';
							memcpy(path + plen + 1, ep->d_name, dlen);
							path[nlen] = '\0';
							__rmdir_r(path, nlen);
						}
					}
				}
			}
			::closedir(dp);
#endif //_WIN32
			path[plen] = '\0';
			rmdir(path);
		} else {
			unlink(path);
		}
	}
}

static inline int
__file_rmdir(char *path, size_t plen)
{
	size_t flen = 0;
	char *fpath = (char*)__file_fullpath(path, plen, &flen);

	__rmdir_r(fpath, flen);
	return 1;
}

static inline int
__file_utime(char *path, size_t plen, time_t mtime, time_t atime)
{
	size_t flen = 0;
	const char *fpath = __file_fullpath(path, plen, &flen);

	utimbuf ub; ub.actime = atime; ub.modtime = mtime;
	return 0 == utime(fpath, &ub) ? 1 : 0;
}

static inline int
__file_check(char *path, size_t plen, size_t fsize, time_t mtime)
{
	size_t flen = 0;
	const char *fpath = __file_fullpath(path, plen, &flen);

	struct stat st;
	return (0 == stat(fpath, &st) && (0 == fsize || fsize == st.st_size) && (0 == mtime || mtime == st.st_mtime)) ? 1 : 0;
}

static inline int
__file_log(int lv, int dlen)
{
	if (nullptr == _F || 0 == (_F->_lmask & (1 << lv))) {
		return 0;
	}

	char lvs[LOG_MAX] = { 'D', 'I', 'W', 'E', 'F' };
	_F->_lblen = 0;
	_F->_lbuff[_F->_lblen++] = lvs[lv];
	_F->_lbuff[_F->_lblen++] = LOG_SPLIT;

	time_t now = ::time(NULL);
	_F->_lblen += strftime(_F->_lbuff + _F->_lblen , sizeof(_F->_lbuff) - _F->_lblen, "%Y%m%d%H%M%S", localtime(&now));
	_F->_lbuff[_F->_lblen++] = LOG_SPLIT;
	_F->_lblen += dlen;
	_F->_lbuff[_F->_lblen++] = '\n';

	if (_F->_lplen > 0 && _F->_lblen > 0 && _F->_lblen < sizeof(_F->_lbuff) - 1) {
		if (0 != memcmp(_F->_ldate, _F->_lbuff + 2, sizeof(_F->_ldate))) {
			memcpy(_F->_ldate, _F->_lbuff + 2, sizeof(_F->_ldate));
			char *p = _F->_lpath + _F->_lplen;
			memcpy(p, _F->_ldate, sizeof(_F->_ldate)); p += sizeof(_F->_ldate);
			*p++ = '.'; *p++ = 't'; *p++ = 'x'; *p++ = 't'; *p++ = '\0';

			if (nullptr != _F->_lfile) {
				::fclose(_F->_lfile); _F->_lfile = nullptr;
			}
		}
		if (nullptr == _F->_lfile) {
			_F->_lfile = ::fopen(_F->_lpath, "at");
		}
		if (nullptr != _F->_lfile) {
			::fwrite(_F->_lbuff, 1, _F->_lblen, _F->_lfile);
		}

		if (_F->_lulen > 0) {
			http_push(_F->_lpush, _F->_lulen, _F->_lbuff, _F->_lblen);
		}

		_F->_lbuff[_F->_lblen] = '\0'; lprint(_F->_lbuff);
	}

	return 1;
}

struct dir_data 
{
	char   _path[PATH_SIZE];
	size_t _plen;
#ifdef _WIN32
	long   _dir;
#else //_WIN32
	DIR   *_dir;
#endif //_WIN32
	int    _closed;
};

static int 
__dir_iter(lua_State *L)
{
	int ret = 0;

	dir_data *d = (dir_data *)luaL_checkudata(L, 1, __DIR_METATABLE);
	luaL_argcheck (L, nullptr != d && 0 == d->_closed, 1, "closed directory");

#ifdef _WIN32
	struct _finddata_t fd;
	if (-1 == d->_dir) {
		if (-1 == (d->_dir = ::_findfirst (d->_path, &fd))) {
			lua_pushnil (L);
			lua_pushstring (L, strerror (errno));
			d->_closed = 1;
			ret = 2;
		} else {
			lua_pushstring (L, fd.name);
			ret = 1;
		}
	} else {
		if (-1 == ::_findnext (d->_dir, &fd)) {
			::_findclose (d->_dir);
			d->_closed = 1;
			ret = 0;
		} else {
			lua_pushstring (L, fd.name);
			ret = 1;
		}
	}
#else //_WIN32
	struct dirent *de = ::readdir(d->_dir);
	if (nullptr != de) {
		lua_pushstring(L, de->d_name);
		ret = 1;
	} else {
		::closedir (d->_dir);
		d->_closed = 1;
		ret = 0;
	}
#endif //_WIN32

	return ret;
}

static int 
__dir_close(lua_State *L)
{
	dir_data *d = (dir_data*)lua_touserdata(L, 1);
#ifdef _WIN32
	if (0 == d->_closed && -1 != d->_dir) {
		::_findclose(d->_dir); d->_dir = -1;
	}
#else
	if (0 == d->_closed && nullptr != d->_dir) {
		::closedir(d->_dir); d->_dir = nullptr;
	}
#endif
	d->_closed = 1;

	LOGD("__dir_close: %s", d->_path);
	return 0;
}

static int 
__dir_init_meta(lua_State *L)
{
	luaL_newmetatable (L, __DIR_METATABLE);

	lua_newtable(L);
	lua_pushcfunction(L, __dir_iter);
	lua_setfield(L, -2, "next");
	lua_pushcfunction (L, __dir_close);
	lua_setfield(L, -2, "close");

	lua_setfield(L, -2, "__index");
	lua_pushcfunction (L, __dir_close);
	lua_setfield (L, -2, "__gc");
	return 1;
}

static int
__l2c_dir(lua_State *L)
{
	size_t plen = 0;
	const char *path = luaL_checklstring (L, 1, &plen);

	lua_pushcfunction (L, __dir_iter);
	dir_data *d = (dir_data*)lua_newuserdata(L, sizeof(dir_data));
	luaL_getmetatable (L, __DIR_METATABLE);
	lua_setmetatable (L, -2);

	size_t flen = 0;
	char *fpath = __file_fullpath((char*)path, plen, &flen);
	memcpy(d->_path, fpath, flen); d->_plen = flen;
	d->_path[d->_plen] = '\0';

#ifdef _WIN32
	d->_dir = -1;
	d->_path[d->_plen++] = '/'; d->_path[d->_plen++] = '*'; d->_path[d->_plen] = '\0';
#else //_WIN32
	d->_dir = ::opendir(d->_path);
	if (nullptr == d->_dir) {
		luaL_error (L, "dir open error %s: %s", path, strerror(errno));
		return 0;
	}
#endif //_WIN32

	d->_closed = 0;
	return 2;
}

static int
__l2c_pushattr(lua_State *L, char *path, size_t plen)
{
	lua_newtable(L);

	struct stat st;
	if (0 != stat(path, &st)) {
		return 1;
	}

	// mode
	lua_pushlstring (L, "md", 2);
	const char *mode = "o";
	if (S_ISREG(st.st_mode)) {
		mode = "f";
	} else if (S_ISDIR(st.st_mode)) {
		mode = "d";
	} else if (S_ISLNK(st.st_mode)) {
		mode = "l";
	} else if (S_ISSOCK(st.st_mode)) {
		mode = "s";
	} else if (S_ISFIFO(st.st_mode)) {
		mode = "p";
	} else if (S_ISCHR(st.st_mode)) {
		mode = "c";
	} else if (S_ISBLK(st.st_mode)) {
		mode = "b";
	}
	lua_pushlstring(L, mode, 1);
	lua_rawset (L, -3);

	// size
	lua_pushlstring (L, "sz", 2);
	lua_pushnumber (L, (lua_Number)st.st_size);
	lua_rawset (L, -3);

	// dev
	lua_pushlstring (L, "dv", 2);
	lua_pushnumber (L, (lua_Number)st.st_dev);
	lua_rawset (L, -3);

	// inode
	lua_pushlstring (L, "no", 2);
	lua_pushnumber (L, (lua_Number)st.st_ino);
	lua_rawset (L, -3);

	// nlink
	lua_pushlstring (L, "nl", 2);
	lua_pushnumber (L, (lua_Number)st.st_nlink);
	lua_rawset (L, -3);

	// ctime
	lua_pushlstring (L, "ct", 2);
	lua_pushnumber (L, (lua_Number)st.st_ctime);
	lua_rawset (L, -3);

	// mtime
	lua_pushlstring (L, "mt", 2);
	lua_pushnumber (L, (lua_Number)st.st_mtime);
	lua_rawset (L, -3);

	// atime
	lua_pushlstring (L, "at", 2);
	lua_pushnumber (L, (lua_Number)st.st_atime);
	lua_rawset (L, -3);

	// uid
	lua_pushlstring (L, "ui", 2);
	lua_pushnumber (L, (lua_Number)st.st_uid);
	lua_rawset (L, -3);

	// uid
	lua_pushlstring (L, "gi", 2);
	lua_pushnumber (L, (lua_Number)st.st_gid);
	lua_rawset (L, -3);

	// permissions
	lua_pushlstring (L, "pm", 2);
	char perms[10] = "---------";
#ifdef _WIN32
	if (st.st_mode & _S_IREAD) { perms[0] = 'r'; perms[3] = 'r'; perms[6] = 'r'; }
	if (st.st_mode & _S_IWRITE) { perms[1] = 'w'; perms[4] = 'w'; perms[7] = 'w'; }
	if (st.st_mode & _S_IEXEC) { perms[2] = 'x'; perms[5] = 'x'; perms[8] = 'x'; }
#else
	if (st.st_mode & S_IRUSR) perms[0] = 'r';
	if (st.st_mode & S_IWUSR) perms[1] = 'w';
	if (st.st_mode & S_IXUSR) perms[2] = 'x';
	if (st.st_mode & S_IRGRP) perms[3] = 'r';
	if (st.st_mode & S_IWGRP) perms[4] = 'w';
	if (st.st_mode & S_IXGRP) perms[5] = 'x';
	if (st.st_mode & S_IROTH) perms[6] = 'r';
	if (st.st_mode & S_IWOTH) perms[7] = 'w';
	if (st.st_mode & S_IXOTH) perms[8] = 'x';
#endif
	lua_pushlstring(L, perms, 9);
	lua_rawset (L, -3);

	return 1;
}

static int
__l2c_attr(lua_State *L)
{
	size_t plen = 0;
	const char *path = luaL_checklstring(L, 1, &plen);
	size_t flen = 0;
	char *fpath = __file_fullpath((char*)path, plen, &flen);

	return __l2c_pushattr(L, fpath, flen);
}

static int
__l2c_list(lua_State *L)
{
	size_t plen = 0;
	const char *path = luaL_checklstring(L, 1, &plen);
	size_t flen = 0;
	char *fpath = __file_fullpath((char*)path, plen, &flen);

	struct stat st;
	if (0 != stat(fpath, &st) || 0 == S_ISDIR(st.st_mode)) {
		return 0;
	}

	lua_newtable(L);

#ifdef _WIN32
	struct _finddata_t fd;
	char *ft = fpath + flen;
	char kp[] = { ft[0], ft[1], ft[2] };
	ft[0] = '/'; ft[1] = '*'; ft[2] = '\0'; 
	long fh = ::_findfirst(fpath, &fd);
	ft[0] = kp[0]; ft[1] = kp[1]; ft[2] = kp[2]; 
	if (-1 != fh) {
		do {
			if (0 != strcmp(".", fd.name) && 0 != strcmp("..", fd.name) ) {
				size_t dlen = strlen(fd.name);
				size_t nlen = flen + dlen + 1;
				assert(nlen < PATH_SIZE);
				if (nlen < PATH_SIZE) {
					fpath[flen] = '/';
					memcpy(fpath + flen + 1, fd.name, dlen);
					fpath[nlen] = '\0';

					lua_pushlstring(L, fd.name, dlen);
					__l2c_pushattr(L, fpath, nlen);
					lua_rawset (L, -3);
				}
			}
		} while (0 == ::_findnext(fh, &fd));

		::_findclose(fh);
	}
#else //_WIN32
	DIR *dp = ::opendir(path);
	if (nullptr != dp) {
		dirent *ep;
		while (nullptr != (ep = ::readdir(dp))) {
			if (0 != strcmp(".", ep->d_name) && 0 != strcmp("..", ep->d_name)) {
				size_t dlen = strlen(ep->d_name);
				size_t nlen = flen + dlen + 1;
				assert(nlen < PATH_SIZE);
				if (nlen < PATH_SIZE) {
					fpath[flen] = '/';
					memcpy(fpath + flen + 1, ep->d_name, dlen);
					fpath[nlen] = '\0';
					lua_pushlstring(L, ep->d_name, dlen);
					__l2c_pushattr(L, fpath, nlen);
					lua_rawset (L, -3);
				}
			}
		}
	}
	::closedir(dp);
#endif //_WIN32

	return 1;
}

static int
__l2c_dirname(lua_State *L)
{
	size_t plen = 0;
	const char *path = luaL_checklstring(L, 1, &plen);
	size_t dlen = __file_dirname(path, plen);
	if (0 == dlen) {
		return 0;
	}

	lua_pushlstring(L, path, dlen);
	return 1;
}

static int
__l2c_mkdir(lua_State *L)
{
	size_t plen = 0;
	const char *path = luaL_checklstring(L, 1, &plen);
	int ret = __file_mkdir((char*)path, plen);

	lua_pushboolean(L, ret);
	return 1;
}

static int
__l2c_rmdir(lua_State *L)
{
	size_t plen = 0;
	const char *path = luaL_checklstring(L, 1, &plen);
	int ret = __file_rmdir((char*)path, plen);

	lua_pushboolean(L, ret);
	return 1;
}

static int
__l2c_utime(lua_State *L)
{
	size_t plen = 0;
	const char *path = luaL_checklstring(L, 1, &plen);
	time_t mtime = (time_t)luaL_checklong(L, 2);
	time_t atime = (time_t)luaL_checklong(L, 3);
	int ret = __file_utime((char*)path, plen, mtime, atime);

	lua_pushboolean(L, ret);
	return 1;
}

static int
__l2c_check(lua_State *L)
{
	size_t plen = 0;
	const char *path = luaL_checklstring(L, 1, &plen);
	size_t fsize = (size_t)luaL_checklong(L, 2);
	time_t mtime = (time_t)luaL_checklong(L, 3);
	int ret = __file_check((char*)path, plen, fsize, mtime);

	lua_pushboolean(L, ret);
	return 1;
}

static int
__l2c_log(lua_State *L)
{
	int lv = (int)luaL_checklong(L, 1);
	size_t llen = 0;
	const char *log = luaL_checklstring(L, 2, &llen);
	if (llen > sizeof(_F->_lbuff) - __LOG_HEAD_SIZE -8) {
		llen = sizeof(_F->_lbuff) - __LOG_HEAD_SIZE -8;
	}

	memcpy(_F->_lbuff + __LOG_HEAD_SIZE, log, llen);
	__file_log(lv, llen);

	return 0;
}

static int
__l2c_lmask(lua_State *L)
{
	_F->_lmask = (size_t)luaL_checklong(L, 1);
	lua_pushboolean(L, 1);
	return 1;
}

static int
__l2c_lpath(lua_State *L)
{
	size_t plen = 0;
	const char *path = luaL_checklstring(L, 1, &plen);
	size_t flen = 0;
	char *fpath = __file_fullpath((char*)path, plen, &flen);
	int ret = __file_mkdir((char*)fpath, __file_dirname(fpath, flen));
	if (0 != ret) {
		memcpy(_F->_lpath, fpath, flen);
		_F->_lplen = flen;
		_F->_ldate[0] = '\0';
		if (nullptr != _F->_lfile) {
			::fclose(_F->_lfile);
			_F->_lfile = nullptr;
		}
	}

	lua_pushboolean(L, ret);
	return 1;
}

static int
__l2c_lpush(lua_State *L)
{
	size_t plen = 0;
	const char *push = luaL_checklstring(L, 1, &plen);
	memcpy(_F->_lpush, push, plen); _F->_lpush[plen] = '\0';
	_F->_lulen = plen;

	lua_pushboolean(L, 1);
	return 1;
}

static int 
__luaopen_file(lua_State *L)
{
	__dir_init_meta (L);

	luaL_Reg r[] = {
		{ "dir", __l2c_dir },
		{ "attr", __l2c_attr },
		{ "list", __l2c_list },
		{ "dirname", __l2c_dirname },
		{ "mkdir", __l2c_mkdir },
		{ "rmdir", __l2c_rmdir },
		{ "utime", __l2c_utime },
		{ "check", __l2c_check },
		{ "log", __l2c_log },
		{ "lmask", __l2c_lmask },
		{ "lpath", __l2c_lpath },
		{ "lpush", __l2c_lpush },
		{ nullptr, nullptr },
	};
	luaL_newlib(L, r);
	return 1;
}

void
file_init(lua_State *L, const char *home, size_t hlen)
{
	if (nullptr == _F) {
		_F = new file_data();
		if (nullptr != home && hlen > 0 && hlen < sizeof(_F->_home)) {
			memcpy(_F->_home, home, hlen); _F->_home[hlen] = '\0'; 
			memcpy(_F->_temp, _F->_home, hlen + 1);
			_F->_hlen = hlen;

			memcpy(_F->_lpath, _F->_home, hlen);
			_F->_lplen = hlen;
			__file_mkdir(_F->_lpath, __file_dirname(_F->_lpath, _F->_lplen));
		}

		luaL_requiref(L, "cfile", __luaopen_file, 0);
	}
}

void
file_fini(lua_State *L)
{
	if (nullptr == _F) {
		return;
	}

	delete _F; _F = nullptr;
}

void*
file_open(char *path, size_t plen, const char *mode)
{
	size_t flen = 0;
	char *fpath = __file_fullpath(path, plen, &flen);

	__file_mkdir((char*)fpath, __file_dirname(fpath, flen));
	return (void*)::fopen(fpath, mode);
}

int
file_utime(char *path, size_t plen, time_t mtime, time_t atime)
{
	return __file_utime(path, plen, mtime, atime);
}

int
file_clog(int lv, const char *fmt, ...)
{	
	va_list vl;
	va_start(vl, fmt);
	char *p = _F->_lbuff + __LOG_HEAD_SIZE;
	*p++ = 'C'; *p++ = LOG_SPLIT;
	int dlen = vsnprintf(p, _F->_lbuff + sizeof(_F->_lbuff) - p - 1, fmt, vl) + 2;
	va_end(vl);
	return __file_log(lv, dlen);
}
