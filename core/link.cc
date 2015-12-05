#include <link.h>
#include <loop.h>
#include <file.h>
#include <util.h>

#ifdef __cplusplus
extern "C" {
#endif
#include <lua/lauxlib.h>
#include <lua/lua.h>
#include <lua/lualib.h>
#include <curl.h>
#ifdef __cplusplus
} //extern "C"
#endif //__cplusplu

#ifdef  _WIN32
#include <winsock2.h>
#define spin() Sleep(1)
#else //_WIN32
#include <unistd.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#define spin() usleep(1)
#endif //_WIN32

#include <stdlib.h>
#include <errno.h>
#include <assert.h>
#include <list>

#ifdef  WIN32
typedef unsigned int        sock_t;
static const unsigned int   SOCK_INVALID = 0xffffffff;
struct iovec {
	unsigned int   iov_len;
	unsigned char* iov_base;
};
#define ERR_AGAIN WSAEWOULDBLOCK 
#define ERR_INPROGRESS ERR_AGAIN 
#define ERR_RESET WSAECONNRESET 
#define ERR_INTR WSAEINTR
#define ERR_SIZE WSAEMSGSIZE

#define snprintf   _snprintf
#define vsnprintf  _vsnprintf
#define last_error ::WSAGetLastError()

#else //WIN32

#define INFINITE   0xffffffff
typedef int        sock_t;
static const int   SOCK_INVALID = -1;

#define ERR_AGAIN EAGAIN 
#define ERR_INPROGRESS EINPROGRESS
#define ERR_RESET ECONNREFUSED 
#define ERR_INTR EINTR
#define ERR_SIZE EMSGSIZE
#define last_error errno
#endif//WIN32

#define SYS_FAIL -1
#define IO_FAIL -1
#define IO_WAIT 0

static inline void
__sock_close(sock_t sock)
{
	if (SOCK_INVALID != sock) {
#ifdef  _WIN32
		::closesocket(sock);
#else //_WIN32
		::close(sock);
#endif//_WIN32
	}
}

static inline bool
__sock_nbio(sock_t sock)
{
	unsigned long b = 1;
#ifdef  _WIN32
	return -1 != ::ioctlsocket(sock, FIONBIO, &b);
#else //_WIN32
	return -1 != ::ioctl(sock, FIONBIO, &b);
#endif//_WIN32
}

static inline bool
__sock_sbuf(sock_t sock, size_t l)
{
	int rb = l;
	return -1 != ::setsockopt(sock, SOL_SOCKET, SO_SNDBUF, (const char*)&rb, sizeof(rb));
}

static inline int
__sock_recv(sock_t s, iovec *v, size_t n)
{
	int ret = IO_FAIL;

#ifdef _WIN32
	unsigned long len = 0, flg = 0;
	ret = ::WSARecv(s, (LPWSABUF)v, n, &len, &flg, NULL, NULL);
	if (0 == ret) {
		ret = (0 == len) ? IO_FAIL : len;
	} else {
		int err = last_error;
		if (ERR_AGAIN == err) {
			ret = IO_WAIT;
		} else {
			assert(ERR_INTR != err);
		}
	}
#else //_WIN32
	msghdr mh = { NULL, 0, v, n, NULL, 0, 0 };
	ret = ::recvmsg(s, &mh, 0);
	if (0 == ret) {
		ret = IO_FAIL;
	} else if (SYS_FAIL == ret) {
		int err = last_error;
		if (ERR_AGAIN == err) {
			ret = IO_WAIT;
		} else {
			assert(ERR_INTR != err);
		}
	}
#endif//_WIN32

	return ret;
}

static inline int
__sock_send(sock_t s, iovec *v, size_t n)
{
	int ret = IO_FAIL;

	if (NULL != v && n > 0 && SOCK_INVALID != s) {

#ifdef _WIN32
		unsigned long len = 0, flg = 0;
		ret = ::WSASend(s, (LPWSABUF)v, n, &len, flg, NULL, NULL);
		if (0 == ret) {
			ret = len;
		} else {
			int err = last_error;
			if (ERR_AGAIN == err) {
				ret = IO_WAIT;
			} else {
				assert(ERR_INTR != err);
			}
		}
#else//_WIN32
		msghdr mh = { NULL, 0, v, n, NULL, 0, 0 };
		ret = ::sendmsg(s, &mh, 0);
		if (SYS_FAIL == ret) {
			int err =last_error;
			if (ERR_AGAIN == err) {
				ret = IO_WAIT;
			} else {
				assert(ERR_INTR != err);
			}
		}
#endif//_WIN32
	}

	return ret;
}

struct link_item
{
	enum { _GET, _POST };
	enum { _INIT, _RECV, _SEND, _CLOSE };
	static const size_t RECV_BUFF_SIZE = 2048;
	static const size_t FILE_BUFF_SIZE = (1 << 18);
	static const size_t SOCK_SBUF_SIZE = (1 << 20);

	sock_t       _sock;
	int          _type;
	int          _step;
	char        *_rbuf;
	size_t       _rlen;
	size_t       _rpos;

	char        *_sbuf;
	size_t       _slen;
	size_t       _spos;
	FILE        *_file;
	size_t       _flen;
	size_t       _fpos;

	link_item(void) : _sock(SOCK_INVALID), _type(0), _step(0), _rbuf(nullptr), _rlen(0), _rpos(0), _sbuf(nullptr), _slen(0), _spos(0), _file(nullptr), _flen(0), _fpos(0)
	{
	}
	~link_item(void)
	{
		if (SOCK_INVALID != this->_sock) {
			__sock_close(this->_sock);
		}

		if (nullptr != this->_file) {
			::fclose(this->_file);
		}

		if (nullptr != this->_rbuf) {
			::free(this->_rbuf);
		}

		if (nullptr != this->_sbuf) {
			::free(this->_sbuf);
		}
	}
};

struct link_data
{
	sock_t   _lsock;
	std::list<link_item*> _links;
	int _c2l_recv;

	link_data(void) : _lsock(SOCK_INVALID), _c2l_recv(LUA_NOREF)
	{
	}

	~link_data(void) {
		if (SOCK_INVALID != this->_lsock) {
			__sock_close(this->_lsock);
		}

		for (auto it : this->_links) {
			delete it;
		}
	}
};

static link_data *_K = nullptr;

static inline link_item *
__link_open(sock_t sock)
{
	link_item * link = nullptr;
	if (__sock_nbio(sock)) {
		link = new link_item();
		link->_sock = sock;
	} else {
		__sock_close(sock);
	}

	return link;
}

static inline bool
__link_listen(void)
{
	_K->_lsock = ::socket(PF_INET, SOCK_STREAM, 0);
	if (SOCK_INVALID == _K->_lsock) {
		return false;
	}

	sockaddr_in si;
	si.sin_family = AF_INET;
	si.sin_addr.s_addr = 0;
	si.sin_port = htons(LINK_PORT);
	if (-1 == ::bind(_K->_lsock, (sockaddr*)&si, sizeof(si)) || !__sock_nbio(_K->_lsock) || -1 == ::listen((sock_t)_K->_lsock, SOMAXCONN)) {
		__sock_close(_K->_lsock);
		_K->_lsock = SOCK_INVALID;
		return false;
	}

	return true;
}

static inline void
__link_accept(void)
{
	sock_t sock = SOCK_INVALID;
	while (SOCK_INVALID != (sock = ::accept(_K->_lsock, NULL, NULL))) {
		if (__sock_sbuf(sock, link_item::SOCK_SBUF_SIZE)) {
			link_item *link = __link_open(sock);
			if (nullptr != link) {
				_K->_links.push_back(link);
			}
		}
	}
}

static inline void
__link_recv(lua_State *L, link_item *link)
{
	int n = 0, c = 0;

	do {
		if (link->_rpos >= link->_rlen) {
			link->_rlen += link_item::RECV_BUFF_SIZE;
			link->_rbuf = (char*)::realloc(link->_rbuf, link->_rlen);
		}

		iovec v;
		v.iov_base = (unsigned char*)link->_rbuf + link->_rpos;
		v.iov_len = link->_rlen - link->_rpos;
		n = __sock_recv(link->_sock, &v, 1);
		if (n > 0) {
			link->_rpos += n; c = 0;
		} else if (0 == n) {
			spin(); ++c;
		}

	} while (n >= 0 && c < 10);

	int k = 0;
	char *f[5] = {0}, *p[20] = {0};
	char *b = link->_rbuf, *e = link->_rbuf + link->_rpos;

	while (b < e) {
		if (nullptr == f[0]) {
			if ('/' == *b) { f[0] = b; }
		} else {
			if (nullptr == f[1]) {
				if ('?' == *b) {
					f[1] = b;
				} else if (' ' == *b) {
					f[1] = f[2] = b;
				}
			} else {
				if (nullptr == f[2]) {
					if (' ' == *b) {
						f[2] = b;
					}
				} else {
					if (nullptr == p[0] && '?' == *(f[1])) {
						char *m = f[1] + 1, *n = f[2];
						p[k++] = m;
						while (m <= n && k < 20) {
							if ('=' == *m) {
								p[k++] = m;
							}
							else if ('&' == *m) {
								char *t = p[k - 1] + 1;
								p[k++] = t + util_url_decode(t, m - t);
								p[k++] = m + 1;
							} else if (' ' == *m) {
								char *t = p[k - 1] + 1;
								p[k++] = t + util_url_decode(t, m - t);
								break;
							}
							++m;
						}
					}
					if (nullptr == f[3]) {
						if (':' == *b) {
							if (0 == memcmp("Content-Length", b - sizeof("Content-Length") + 1, sizeof("Content-Length") - 1)) {
								f[3] = b + 2;
							}
						}
					} else {
						if (nullptr == f[4]) {
							if ('\r' == *b) {
								f[4] = b;
							}
						}
					}
				}
			}
		}
		++b;
	}

	if (n < 0 || nullptr == f[2] || 0 != (k % 3)) {
		link->_step = link_item::_CLOSE;
		return;
	}

	if (LUA_NOREF != _K->_c2l_recv) {
		link->_step = link_item::_RECV;

		lua_rawgeti(L, LUA_REGISTRYINDEX, _K->_c2l_recv);

		lua_pushlightuserdata(L, (void*)link);

		lua_pushinteger(L, 'G' == link->_rbuf[0] ? link_item::_GET : link_item::_POST);

		lua_pushlstring(L, f[0] + 1, f[1] - f[0] - 1);

		if (k > 0) {
			lua_newtable(L);
			for (int i = 0; i < k; i += 3) {
				lua_pushlstring(L, p[i], p[i + 1] - p[i]);
				lua_pushlstring(L, p[i + 1] + 1, p[i + 2] - p[i + 1] - 1);
				lua_rawset(L, -3);
			}
		} else {
			lua_pushnil(L);
		}

		if (nullptr != f[3] && nullptr != f[4]) {
			*(f[4]) = '\0';
			size_t l = atoi(f[3]);
			lua_pushlstring(L, link->_rbuf + link->_rpos - l, l);
		} else {
			lua_pushnil(L);
		}

		loop_call(L, 5, 0);
	} else {
		link->_step = link_item::_CLOSE;
	}
}

static void
__link_send(link_item *link)
{
	if (link_item::_SEND != link->_step && nullptr != link->_sbuf) {
		return;
	}

	int n = 0;
	do {
		iovec v;
		v.iov_base = (unsigned char*)link->_sbuf + link->_spos;
		v.iov_len = link->_slen - link->_spos;
		n = __sock_send(link->_sock, &v, 1);
		if (n > 0) {
			link->_spos += n;
		}
	} while (n >= 0 && link->_spos < link->_slen);

	if (n < 0 || link->_spos >= link->_slen) {
		link->_step = link_item::_CLOSE;
	}
}

static void
__link_fsend(link_item *link)
{
	if (link_item::_SEND != link->_step && nullptr != link->_file) {
		return;
	}

	int c = 0;
	int sn = 0;
	do {
		int fn = link->_flen - link->_fpos;
		int bn = link_item::FILE_BUFF_SIZE - link->_slen;

		int rn = fn > bn ? bn : fn;
		if (rn > 0) {
			if (rn != ::fread(link->_sbuf + link->_slen, 1, rn, link->_file)) {
				sn = -2; break;
			}
			link->_fpos += rn;
			link->_slen += rn;
		}

		iovec v;
		v.iov_base = (unsigned char*)link->_sbuf + link->_spos;
		v.iov_len = link->_slen - link->_spos;
		sn = __sock_send(link->_sock, &v, 1);
		if (sn > 0) {
			link->_spos += sn;
			if (link->_spos >= link->_slen) {
				link->_spos = link->_slen = 0;
			}
			c = 0;
		} else if (0 == sn) {
			spin(); ++c;
		}
	} while (sn >= 0 && c < 10 && (link->_spos < link->_slen || link->_fpos < link->_flen));

	if (sn < 0 || link->_spos >= link->_slen) {
		link->_step = link_item::_CLOSE;
	}
}

static int
__l2c_send(lua_State *L)
{
	link_item *link = (link_item*)lua_touserdata(L, 2);
	if (nullptr == link) {
		return 0;
	}

	if (link_item::_RECV != link->_step || SOCK_INVALID == link->_sock) {
		return 0;
	}
	link->_step = link_item::_SEND;

	iovec v[2];
	int vn = 1;
	int vl = 0;

	size_t cl = 0;
	const char *c = lua_tolstring(L, 3, &cl);
	if (nullptr != c && cl > 0) {
		v[1].iov_base = (unsigned char*)c;
		v[1].iov_len = cl;
		vn = 2;
		vl += v[1].iov_len;
	}
	
	size_t hl = 0;
	const char *h = lua_tolstring(L, 4, &hl);
	if (nullptr == h) {
		h = "";
	}

	char t[512];
	size_t tl = snprintf(t, sizeof(h), "HTTP/1.1 200 OK\r\n%sContent-Length:%u\r\n\r\n", h, cl);
	v[0].iov_base = (unsigned char*)t;
	v[0].iov_len = tl;
	vl += v[0].iov_len;

	int n = __sock_send(link->_sock, v, vn);
	if (n >= 0 && n < vl) {
		link->_slen = vl - n;
		link->_sbuf = new char[link->_slen];
		if (n < (int)v[0].iov_len) {
			memcpy(link->_sbuf, ((char*)v[0].iov_base) + n, v[0].iov_len - n);
			memcpy(link->_sbuf + v[0].iov_len - n, v[1].iov_base, v[1].iov_len);
		} else {
			memcpy(link->_sbuf, ((char*)v[1].iov_base) + n - v[0].iov_len, vl - n);
		}
		__link_send(link);
	} else {
		link->_step = link_item::_CLOSE;
	}

	return 0;
}

static int
__l2c_fsend(lua_State *L)
{
	link_item *link = (link_item*)lua_touserdata(L, 2);
	if (nullptr == link) {
		return 0;
	}

	link->_step = link_item::_SEND;

	size_t pl = 0;
	const char *p = lua_tolstring(L, 3, &pl);
	if (nullptr != p && pl > 0) {
		link->_file = (FILE*)file_open((char*)p, pl, "rb");
		if (nullptr == link->_file) {
			link->_step = link_item::_CLOSE;
			return 0;
		}
		::fseek(link->_file, 0, SEEK_END);
		link->_flen = ::ftell(link->_file);
		::fseek(link->_file, 0, SEEK_SET);
	}

	size_t hl = 0;
	const char *h = lua_tolstring(L, 4, &hl);
	if (nullptr == h) {
		h = "";
	}

	link->_sbuf = (char*)::malloc(link_item::FILE_BUFF_SIZE);
	link->_slen = snprintf(link->_sbuf, link_item::FILE_BUFF_SIZE, "HTTP/1.1 200 OK\r\n%sContent-Length: %u\r\n\r\n", h, (unsigned int)link->_flen);

	__link_fsend(link);

	return 0;
}

static int
__l2c_close(lua_State *L)
{
	link_item *link = (link_item*)lua_touserdata(L, 2);
	if (nullptr != link) {
		link->_step = link_item::_CLOSE;
	}

	return 0;
}

static int
__l2c_bind(lua_State *L)
{
	assert(1 == lua_gettop(L));
	assert(lua_istable(L, -1));

	if (LUA_NOREF != _K->_c2l_recv) {
		luaL_unref(L, LUA_REGISTRYINDEX, _K->_c2l_recv);
		_K->_c2l_recv = LUA_NOREF;
	}

	lua_getfield(L, 1, "recv");
	if (lua_isfunction(L, -1)) {
		_K->_c2l_recv = luaL_ref(L, LUA_REGISTRYINDEX);
	}

	return 0;
}

static int
__luaopen_link(lua_State *L)
{
	luaL_Reg r[] = {
			{ "bind", __l2c_bind },
			{ "send", __l2c_send },
			{ "fsend", __l2c_fsend },
			{ "close", __l2c_close },
			{ nullptr, nullptr },
	};
	luaL_newlib(L, r);
	return 1;
}

void
link_init(lua_State *L)
{
	if (nullptr != _K) {
		return;
	}

#ifdef  _WIN32
	WSADATA wsa;
	int ret = WSAStartup(MAKEWORD(2, 2), &wsa);
	if (0 != ret || 2 != LOBYTE(wsa.wVersion) || 2 != HIBYTE(wsa.wVersion)) {
		return;
	}
#endif//_WIN32

	_K = new link_data();
	if (!__link_listen()) {
		LOGF("link-listen failed");
		delete _K; _K = nullptr;
		return;
	}
	LOGE("link-listen success");

	luaL_requiref(L, "clink", __luaopen_link, 0);
}

void
link_loop(lua_State *L) {
	if (nullptr == _K) {
		return;
	}

	__link_accept();

	for (auto it : _K->_links) {
		__link_recv(L, it);
	}

	typedef std::list<link_item*>::iterator link_item_it;

	for (link_item_it it = _K->_links.begin(); _K->_links.end() != it;) {
		link_item *link = (*it);
		if (link_item::_CLOSE == link->_step) {
			delete link;
			_K->_links.erase(it++);
		} else {
			++it;
		}
	}
}

void
link_fini(lua_State *L)
{
	if (nullptr == _K) {
		return;
	}

	delete _K; _K = nullptr;

#ifdef  _WIN32
	::WSACleanup();
#endif//_WIN32
}
