#include "lpsend.h"

static lua_State *L;
int remove_job;

const struct luaL_Reg *libraries[] = {
  base64funcs,
  streamsfuncs,
  utilfuncs,
  NULL
};

#define DEFNTOTABLE(NAME)			\
  lua_pushstring(L, #NAME);			\
  lua_pushinteger(L, NAME);			\
  lua_rawset(L, -3);

static void install_constant_tables()
{
  lua_newtable(L);
  DEFNTOTABLE(EBUSY);
  DEFNTOTABLE(ENOENT);
  lua_setglobal(L, "errno");

  lua_newtable(L);
  DEFNTOTABLE(JSUCC);
  DEFNTOTABLE(JFAIL);
  DEFNTOTABLE(JREMOVE);
  lua_setglobal(L, "lprng_exit_code");

#ifdef MOCK
  lua_getglobal(L, "lpsend");
  DEFNTOTABLE(PRINT_WRITE);
  DEFNTOTABLE(PRINT_READ);
  DEFNTOTABLE(STDIN_READ);
  lua_pop(L, 1);
#endif
}

int main(int argc, char *argv[])
{
  const struct luaL_Reg **libptr;
  int i, rc;
  char *luaprogname = LPSEND_LUA;

  openlog("LPSEND", LOG_CONS | LOG_PERROR, LOG_LPR);

  L = luaL_newstate();
  luaL_openlibs(L);
  for (libptr = libraries; *libptr; ++libptr)
    {
      luaL_register(L, "lpsend", *libptr);
      lua_pop(L, 1);
    }

  install_constant_tables();

  lua_newtable(L);
  for (i = 1; i < argc; i++)
    {
      lua_pushstring(L, argv[i]);
      lua_rawseti(L, -2, i);
    }
  lua_pushinteger(L, 0);
  lua_pushstring(L, argv[0]);
  lua_rawset(L, -3);
  lua_setglobal(L, "arg");

  rc = euidaccess(luaprogname, R_OK)
    ? luaL_loadstring(L, "require 'bytecode'")
    : luaL_loadfile(L, luaprogname);

  switch (rc)
    {
    case 0:
      break;
    case LUA_ERRFILE:
      syslog(LOG_ERR, "%s", lua_tostring(L, -1));
      return JFAIL;
    case LUA_ERRSYNTAX:
      syslog(LOG_ERR, "invalid lua script.  %s", lua_tostring(L, -1));
      return JFAIL;
    default:
      syslog(LOG_ERR, "unknown lua error %d loading main script %s",
	     rc, LPSEND_LUA);
      return JFAIL;
    }

  if (rc = lua_pcall(L, 0, 0, 0))
    {
      const char *errmsg;

      syslog(LOG_ERR, "Lua script failed!  %s",
	     (errmsg = lua_tostring(L, -1))
	     ? errmsg : "***CAN'T OBTAIN ERROR MESSAGE***");
      lua_close(L);
      return remove_job ? JREMOVE : JFAIL;
    }

  lua_close(L);
  return JSUCC;
}
