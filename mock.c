#include "lpsend.h"

#ifdef MOCK

static void mock_error(lua_State *L, const char *name, char *wanted,
		       int posn)
{
  luaL_error(L, "bad mock reply #%d to '%s' (%s expected, got %s)",
	     posn > 0 ? posn : posn + 1 + lua_gettop(L),
	     name, wanted, luaL_typename(L, posn));
}

static int mock_checkinteger(lua_State *L, const char *name, int posn)
{
  if (!lua_isnumber(L, posn))
    mock_error(L, name, "number", posn);
  return lua_tointeger(L, posn);
}

#define MOCK_checkinteger(L, ...)  mock_checkinteger(L, __func__, __VA_ARGS__)

static const char *mock_checklstring(lua_State *L,
				     const char *name, int posn, size_t * len)
{
  if (!lua_isstring(L, posn))
    mock_error(L, name, "string", posn);
  return lua_tolstring(L, posn, len);
}

#define MOCK_checklstring(L, ...)  mock_checklstring(L, __func__, __VA_ARGS__)


static int try_function(lua_State *L, char *name, int *ref)
{
  lua_getglobal(L, name);
  if (lua_isfunction(L, -1))
    return 1;
  lua_rawgeti(L, LUA_REGISTRYINDEX, *ref);
  if (*ref == LUA_NOREF || !lua_rawequal(L, -1, -2))
    {
      if (*ref != LUA_NOREF)
	luaL_unref(L, LUA_REGISTRYINDEX, *ref);
      lua_pop(L, 1);
      *ref = luaL_ref(L, LUA_REGISTRYINDEX);
      fprintf(stderr,
	      "\nMOCK ERROR: %s not %s.  Using system call.\n\n",
	      name, lua_isnil(L, -1) ? "defined" : "a function");
    }
  else
    lua_pop(L, 2);

  return 0;
}

int read_stub(lua_State *L, char *mockname, int fd, char *buffer, int buflen)
{
  int top;
  unsigned int readlen;
  const char *buffer_contents;
  static int ref = LUA_NOREF;

  top = lua_gettop(L);
  if (!try_function(L, mockname, &ref))
    return read(fd, buffer, buflen);

  lua_pushinteger(L, buflen);
  lua_call(L, 1, LUA_MULTRET);

  if (!lua_toboolean(L, top + 1))
    {
      int error_no = MOCK_checkinteger(L, top + 2);
      lua_settop(L, top);
      errno = error_no;
      return -1;
    }

  buffer_contents = MOCK_checklstring(L, -1, &readlen);
  readlen = buflen < readlen ? buflen : readlen;
  memmove(buffer, buffer_contents, readlen);
  lua_settop(L, top);
  return readlen;
}

int write_stub(lua_State *L, int fd, char *buffer, int buflen)
{
  int return_value, top;
  static int ref = LUA_NOREF;
  char *mockname = "mock_write_printer";

  top = lua_gettop(L);
  if (!try_function(L, mockname, &ref))
    return write(fd, buffer, buflen);

  lua_pushlstring(L, buffer, buflen);
  lua_call(L, 1, LUA_MULTRET);

  if (!lua_toboolean(L, top + 1))
    {
      int error_no;
      error_no = MOCK_checkinteger(L, top + 2);
      lua_settop(L, top);
      errno = error_no;
      return -1;
    }

  return_value = MOCK_checkinteger(L, top + 1);
  lua_settop(L, top);
  return return_value > buflen ? buflen : return_value;
}

int select_stub(lua_State *L, int nfds, fd_set * readfds, fd_set * writefds,
		struct timeval *tv)
{
  int top, flags, ready = 3;
  static int ref = LUA_NOREF;
  char *mockname = "mock_select";

  top = lua_gettop(L);

  if (!try_function(L, mockname, &ref))
    return select(nfds, readfds, writefds, NULL, tv);

  lua_pushinteger(L,
		  (FD_ISSET(print_writefd, writefds) ? PRINT_WRITE : 0) |
		  (FD_ISSET(print_readfd, readfds) ? PRINT_READ : 0) |
		  (FD_ISSET(0, readfds) ? STDIN_READ : 0));

  if (tv)
    {
      // Timed waits are always less than a second.
      lua_pushinteger(L, tv->tv_usec);
      lua_call(L, 2, LUA_MULTRET);
    }
  else
    lua_call(L, 1, LUA_MULTRET);

  if (!lua_toboolean(L, top + 1))
    {
      int error_no = MOCK_checkinteger(L, top + 2);
      lua_settop(L, top);
      errno = error_no;
      return -1;
    }

  flags = ~MOCK_checkinteger(L, top + 1);

  if (flags & PRINT_WRITE)
    {
      FD_CLR(print_writefd, writefds);
      ready--;
    }
  if (flags & PRINT_READ)
    {
      FD_CLR(print_readfd, readfds);
      ready--;
    }
  if (flags & STDIN_READ)
    {
      FD_CLR(0, readfds);
      ready--;
    }
  lua_settop(L, top);
  return ready;
}

int clock_gettime_stub(lua_State *L, clockid_t clockid, struct timespec *ts)
{
  int top;
  static int ref = LUA_NOREF;
  char *mockname = "mock_clock_gettime";

  top = lua_gettop(L);
  if (!try_function(L, mockname, &ref))
    return clock_gettime(clockid, ts);

  lua_call(L, 0, 2);
  ts->tv_sec = MOCK_checkinteger(L, top + 1);
  ts->tv_nsec = MOCK_checkinteger(L, top + 2);
  lua_settop(L, top);
  return 0;
}

int getlpstatus_stub(lua_State *L, int fd, int *status)
{
  int top;
  static int ref = LUA_NOREF;
  char *mockname = "mock_lpgetstatus";

  top = lua_gettop(L);
  if (!try_function(L, mockname, &ref))
    return ioctl(fd, LPGETSTATUS, status);

  lua_call(L, 0, LUA_MULTRET);
  if (!lua_toboolean(L, top + 1))
    {
      int error_no = MOCK_checkinteger(L, top + 2);
      lua_settop(L, top);
      errno = error_no;
      return -1;
    }
  *status = MOCK_checkinteger(L, top + 1);
  lua_settop(L, top);
  return 0;

}
#endif
