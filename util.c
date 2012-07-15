#include "lpsend.h"

static int lpsend_syslog(lua_State *L)
{
  syslog(LOG_ERR, luaL_checkstring(L, 1));
  return 0;
}

static int errno_to_string(lua_State *L)
{
  lua_pushstring(L, strerror(luaL_checkinteger(L, 1)));
  return 1;
}

static int request_remove_job(lua_State *L)
{
  remove_job = 1;
  return 0;
}

static int get_next_job_id(lua_State *L)
{
  int jobident;
  int jidfd = open(luaL_checkstring(L, 1), O_RDWR | O_CREAT | O_RDWR, 0666);

  if (jidfd < 0)
    luaL_error(L, "Can't open job ident file.  %s", strerror(errno));

  if (read(jidfd, &jobident, sizeof(jobident)) < sizeof(jobident) ||
      (++jobident > 0xFFFFFF))
    jobident = 0;

  lseek(jidfd, 0, SEEK_SET);
  write(jidfd, &jobident, sizeof(jobident));
  ftruncate(jidfd, sizeof(jobident));
  close(jidfd);
  lua_pushinteger(L, jobident);
  return 1;
}

static int sendmail(lua_State *L)
{
  const char *message, *recipient, *msgptr;
  int pipes[2];
  int remaining, actual, status;

  recipient = luaL_checkstring(L, 1);
  message = luaL_checkstring(L, 2);

  if (pipe(pipes) < 0)
    luaL_error(L, "Can't get pipe for " SENDMAIL_NAME ".  %s",
	       strerror(errno));

  switch (fork())
    {
    case -1:
      luaL_error(L, "Can't create " SENDMAIL_NAME " process.  %s",
		 strerror(errno));
    case 0:
      close(pipes[1]);
      if (dup2(pipes[0], 0) != -1)
	execlp(SENDMAIL, SENDMAIL, recipient, NULL);
      syslog(LOG_ERR, "Can't execute " SENDMAIL_NAME ".  %m");
      exit(1);
    }

  close(pipes[0]);

  for (remaining = strlen(message), msgptr = message;
       remaining > 0; remaining -= actual, msgptr += actual)
    {
      actual = write(pipes[1], msgptr, remaining);
      if (actual < 0)
	{
	  int error_no = errno;

	  close(pipes[1]);
	  luaL_error(L, "Problem writing to " SENDMAIL_NAME ".  %s",
		     strerror(error_no));
	}
    }

  close(pipes[1]);
  wait(&status);
  if (WIFEXITED(status) && WEXITSTATUS(status) != 0)
    luaL_error(L, SENDMAIL_NAME "exited code %d.", WEXITSTATUS(status));
  if (WIFSIGNALED(status))
    luaL_error(L, SENDMAIL_NAME "got signal %d.", WTERMSIG(status));

  return 0;
}

static int permitted_chars[256], invalid_chars[256];

static int permit_character(lua_State *L)
{
  permitted_chars[luaL_checkinteger(L, 1) & 0xFF] = 1;
  return 0;
}

static int forbid_character(lua_State *L)
{
  permitted_chars[luaL_checkinteger(L, 1) & 0xFF] = 0;
  return 0;
}

static int invalid_character_found(lua_State *L)
{
  lua_pushboolean(L, invalid_chars[luaL_checkinteger(L, 1) & 0xFF]);
  return 1;
}

int lpsend_validate(unsigned char *buf, int len)
{
  int valid = 1, ch;

  while (len--)
    if (!permitted_chars[ch = *buf++])
      {
	invalid_chars[ch & 0xFF] = 1;
	valid = 0;
      }

  return valid;
}

static int stopwatch(lua_State *L)
{
  struct timespec *ts;

  int since(lua_State *L)
  {
    struct timespec ts_now, *ts_then;
    CLOCK_GETTIME(CLOCK_TYPE, &ts_now);
    ts_then = lua_touserdata(L, lua_upvalueindex(1));
    lua_pushinteger(L, CLOCK_DIFF_MSEC(ts_now, *ts_then));
    return 1;
  }

  ts = lua_newuserdata(L, sizeof(struct timespec));
  CLOCK_GETTIME(CLOCK_TYPE, ts);
  lua_pushcclosure(L, since, 1);
  return 1;
}

static int lpsend_time(lua_State *L)
{
  lua_pushinteger(L, time(NULL));
  return 1;
}


char *wall_clock()
{
  struct timespec ts;
  char static outbuf[32];

  clock_gettime(CLOCK_REALTIME, &ts);
  snprintf(outbuf, sizeof(outbuf), "%ld.%09ld", ts.tv_sec, ts.tv_nsec);
  return outbuf;
}

static int lpsend_wall_clock(lua_State *L)
{
  lua_pushstring(L, wall_clock());
  return 1;
}

static int strip_for_logging(lua_State *L)
{
  luaL_Buffer buffer;
  const char *in_string;
  size_t length;

  in_string = luaL_checklstring(L, 1, &length);

  luaL_buffinit(L, &buffer);

  while (length--)
    {
      const char ch = *in_string++;

      if (isblank(ch) || isprint(ch))
	luaL_addlstring(&buffer, &ch, 1);
      else if (ch == '\n')
	luaL_addlstring(&buffer, " ", 1);
    }

  luaL_pushresult(&buffer);
  return 1;
}

static int lpsend_exists(lua_State *L)
{
  struct stat st;
  lua_pushboolean(L, stat(luaL_checkstring(L, 1), &st) == 0);
  return 1;
}

#define DEFCTYPE(NAME) static int lpsend_##NAME(lua_State *L) \
  { lua_pushboolean(L, NAME(luaL_checkinteger(L, 1)) != 0); return 1; }

#define DECLCTYPE(NAME) { #NAME, lpsend_##NAME }

DEFCTYPE(isalnum)
DEFCTYPE(isalpha)
DEFCTYPE(isascii)
DEFCTYPE(isblank)
DEFCTYPE(iscntrl)
DEFCTYPE(isdigit)
DEFCTYPE(isgraph)
DEFCTYPE(islower)
DEFCTYPE(isprint)
DEFCTYPE(ispunct)
DEFCTYPE(isspace)
DEFCTYPE(isupper)
DEFCTYPE(isxdigit)

const luaL_Reg utilfuncs[] = {
  DECLCTYPE(isalnum),
  DECLCTYPE(isalpha),
  DECLCTYPE(isascii),
  DECLCTYPE(isblank),
  DECLCTYPE(iscntrl),
  DECLCTYPE(isdigit),
  DECLCTYPE(isgraph),
  DECLCTYPE(islower),
  DECLCTYPE(isprint),
  DECLCTYPE(ispunct),
  DECLCTYPE(isspace),
  DECLCTYPE(isupper),
  DECLCTYPE(isxdigit),
  {"stopwatch", stopwatch},
  {"time", lpsend_time},
  {"wall_clock", lpsend_wall_clock},
  {"errno_to_string", errno_to_string},
  {"syslog", lpsend_syslog},
  {"sendmail", sendmail},
  {"get_next_job_id", get_next_job_id},
  {"request_remove_job", request_remove_job},
  {"permit_character", permit_character},
  {"forbid_character", forbid_character},
  {"invalid_character_found", invalid_character_found},
  {"strip_for_logging", strip_for_logging},
  {"exists", lpsend_exists},
  {NULL, NULL}
};
