#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <err.h>
#include <ctype.h>

#include <syslog.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <time.h>
#include <linux/lp.h>
#include <linux/ioctl.h>
#include <libgen.h>
#include <pthread.h>

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

int euidaccess(const char *pathname, int mode);

#define LPSEND_LUA "/lib/lpsend.lua"

#define SENDMAIL "/usr/local/sbin/exim"
#define SENDMAIL_NAME "EXIM"

#define BLKSIZ 4096

#define CLOCK_TYPE CLOCK_MONOTONIC_RAW
#define CLOCK_DIFF_MSEC(NOW, THEN)					\
  (1000*((NOW).tv_sec-(THEN).tv_sec) +					\
   ((NOW).tv_nsec-(THEN).tv_nsec)/1000000)

// How many times to try opening printer
#define OPEN_TRIES 5
// How many ms to wait between tries.
#define OPEN_TRY_WAIT 200

// Output buffer size must be a power of two.
#define OUTPUT_BUFFER_SIZE (1<<12)
#define READBACK_THREAD_BUFFER_SIZE (1<<10)
#define READBACK_BUFFER_SIZE (1<<12)
#define READBACK_FREE_MINIMUM (1<<10)

#define HOLD_TIME_MSEC_DEFAULT 1000
#define DRAIN_TIME_MSEC_DEFAULT 120000
#define WRITE_STALL_LIMIT_MSEC_DEFAULT 120000
#define READ_STALL_LIMIT_MSEC_DEFAULT 1000
#define READBACK_WAIT_MSEC_DEFAULT 20
#define SELECT_WAIT_MSEC_DEFAULT 100

// LPRng filter result constants.
#define JSUCC 0			// SUCCESS
#define JFAIL 1			// Fail, but try again.
#define JREMOVE 3		// Fail, and delete job.

#ifdef MOCK
// LPRng data streams.
#define PRINT_READ 1
#define PRINT_WRITE 2
#define STDIN_READ 4

extern int print_writefd, print_readfd;
#endif

extern const luaL_Reg mailfuncs[], base64funcs[], streamsfuncs[], utilfuncs[];

int lpsend_validate(unsigned char *data, int len);

#define IOCNR_GET_VID_PID 6
#define LPIOC_GET_VID_PID(len) _IOC(_IOC_READ, 'P', IOCNR_GET_VID_PID, len)

// L is expected in context.
#ifdef MOCK
int read_stub(lua_State *, char *, int, char *, int);
int write_stub(lua_State *, int, char *, int);
int select_stub(lua_State *, int, fd_set *, fd_set *, struct timeval *);
int clock_gettime_stub(lua_State *, clockid_t type, struct timespec *);
int getlpstatus_stub(lua_State *, int, int *);

#define READ_STDIN(BUF, LEN)				\
  read_stub(L, "mock_read_stdin", 0, (BUF), (LEN))
#define READ_PRINTER(BUF, LEN, ERROR_FD_PTR)			\
  read_stub(L, "mock_read_printer", print_readfd, (BUF), (LEN))
#define WRITE_PRINTER(BUF, LEN)			\
  write_stub((L), print_writefd, (BUF), (LEN))
#define SELECT(NFDS, READFDS, WRITEFDS, EXCEPTFDS, TIMEOUT)	\
  select_stub(L, (NFDS), (READFDS), (WRITEFDS), (TIMEOUT))
#define CLOCK_GETTIME(TYPE, TIMESPEC)		\
  clock_gettime_stub(L, (TYPE), (TIMESPEC))
#define GETLPSTATUS(FD, STATUS_PTR)		\
  getlpstatus_stub(L, (FD), (STATUS_PTR))
#else
#define READ_STDIN(BUF, LEN)			\
  read(0, (BUF), (LEN))
#define READ_PRINTER(BUF, LEN, ERROR_FD_PTR)	\
  read_printer((BUF), (LEN), (ERROR_FD_PTR))
#define WRITE_PRINTER(BUF, LEN)			\
  write(print_writefd, (BUF), (LEN))
#define SELECT(NFDS, READFDS, WRITEFDS, EXCEPTFDS, TIMEOUT)	\
  select((NFDS), (READFDS), (WRITEFDS), (EXCEPTFDS), (TIMEOUT))
#define CLOCK_GETTIME(TYPE, TIMESPEC)		\
  clock_gettime((TYPE), (TIMESPEC))
#define GETLPSTATUS(FD, STATUS_PTR)		\
  ioctl((FD), LPGETSTATUS, (STATUS_PTR))
#endif
