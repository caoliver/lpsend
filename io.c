#include "lpsend.h"

// Configuration entries.
static long hold_time_msec = HOLD_TIME_MSEC_DEFAULT;
static long drain_time_msec = DRAIN_TIME_MSEC_DEFAULT;
static long write_stall_limit_msec = WRITE_STALL_LIMIT_MSEC_DEFAULT;
static long read_stall_limit_msec = READ_STALL_LIMIT_MSEC_DEFAULT;
static long readback_wait_msec = READBACK_WAIT_MSEC_DEFAULT;
static long select_wait_msec = SELECT_WAIT_MSEC_DEFAULT;
static int strip_parity;
static long write_limit;

#ifdef MOCK
int print_writefd = -1, print_readfd = -1;
#else
static int print_writefd = -1, print_readfd = -1;
#endif

// Readback thread IPC
static int readback_error, readback_error_fd;
static int readback_pipe[2];
static pthread_mutex_t readback_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t readback_thread;
static unsigned char readback_thread_buffer[READBACK_THREAD_BUFFER_SIZE];
static int readback_thread_read_actual;

static int number_fds;

#define ERROR_FD_IS_PIPE_READ 0
#define ERROR_FD_IS_PIPE_WRITE 1
#define ERROR_FD_IS_PRINTER 2

static void *readback_loop(void *dummy)
{
  pthread_mutex_lock(&readback_lock);

  while (1)
    {
      switch (readback_thread_read_actual =
	      read(print_writefd,
		   readback_thread_buffer, READBACK_THREAD_BUFFER_SIZE))
	{
	case -1:
	  if (errno != EAGAIN)
	    {
	      readback_error_fd = ERROR_FD_IS_PRINTER;
	      goto bugout;
	    }
	case 0:
	  usleep(1000 * readback_wait_msec);
	  continue;
	}

      if (write(readback_pipe[1], "\0", 1) == -1)
	{
	  readback_error_fd = ERROR_FD_IS_PIPE_WRITE;
	  break;
	}
      pthread_mutex_lock(&readback_lock);
    }

bugout:

  readback_error = errno;
  close(readback_pipe[1]);
  pthread_exit(NULL);
}

static int read_printer(char *buffer, int size, int *thread_fd)
{
  char dummy;

  if (size > readback_thread_read_actual)
    size = readback_thread_read_actual;

  switch (read(print_readfd, &dummy, 1))
    {
    case 0:
      errno = readback_error;
      *thread_fd = readback_error_fd;
    case -1:
      return -1;
    }

  // Read truncates!
  memmove(buffer, readback_thread_buffer, size);
  pthread_mutex_unlock(&readback_lock);
  return size;
}

static int get_number_named(lua_State *L, char *name, long *param)
{
  int is_set = 0;
  lua_getfield(L, 1, name);
  if (!lua_isnil(L, -1))
    {
      long msec = luaL_checkinteger(L, -1);
      *param = msec < 0 ? 0 : msec;
      is_set = 1;
    }
  lua_pop(L, 1);
  return is_set;
}

// config_table:<timeouts> -> void
static int set_timeouts(lua_State *L)
{
  get_number_named(L, "select_wait", &select_wait_msec);
  get_number_named(L, "hold_time", &hold_time_msec);
  get_number_named(L, "drain_time", &drain_time_msec);
  get_number_named(L, "write_stall_limit", &write_stall_limit_msec);
  get_number_named(L, "readback_wait", &readback_wait_msec);
  get_number_named(L, "read_stall_limit", &read_stall_limit_msec);

  return 0;
}

// config_table:printer_name -> errno or false
static void initialize_printer(lua_State *L)
{
  int tries = 0;
  const char *printer_name;
  long open_tries = OPEN_TRIES;
  long open_try_wait_msec = OPEN_TRY_WAIT;

  lua_getfield(L, 1, "printer");
  printer_name = luaL_checkstring(L, -1);
  get_number_named(L, "open_tries", &open_tries);
  get_number_named(L, "open_try_wait", &open_try_wait_msec);
  get_number_named(L, "write_limit", &write_limit);
  if (write_limit < 0)
    write_limit = 0;

  do
    usleep(1000 * (open_try_wait_msec));
  while ((print_writefd = open(printer_name, O_RDWR | O_NONBLOCK)) < 0 &&
	 errno == EBUSY && ++tries < (open_tries));

  // Any of these failures means the caller should clean up and exit,
  // so we leave it to process rundown to clean up anything allocated.
  // N.B.: pipe and pthread_create do not return EBUSY; returning EBUSY
  // means the printer is held by someone else.
  if (print_writefd < 0 ||
      pipe(readback_pipe) < 0 ||
      pthread_create(&readback_thread, NULL, readback_loop, NULL) < 0)
    lua_pushinteger(L, errno);
  else
    {
      print_readfd = readback_pipe[0];
      number_fds = 1 + (print_readfd > print_writefd
			? print_readfd : print_writefd);
      lua_pushboolean(L, 0);
    }
}

static int initialize_io(lua_State *L)
{
  lua_getfield(L, 1, "strip_parity");
  strip_parity = lua_toboolean(L, 1);
  lua_pop(L, 1);
  set_timeouts(L);
  initialize_printer(L);
  // Replace the table with the status value.
  lua_replace(L, -2);

#ifndef MOCK
  fcntl(0, F_SETFL, O_NONBLOCK | fcntl(0, F_GETFL));
#endif

  return 1;
}

int posix_error(lua_State *L, char *explanation)
{
  int error_no = errno;
  lua_pushstring(L, "posix_error");
  lua_pushinteger(L, error_no);
  lua_pushstring(L, explanation);
  return 3;
}

#define DFA_BYTE_CLASS static unsigned char byte_class
#define DFA_TRANSITIONS static unsigned char dfa_transition
#include "dfa.h"

static int process_readback(lua_State *L,
				   int readback_available,
				   int discard_normal_readback)
{
  static char in_buffer[READBACK_BUFFER_SIZE];
  static int buffer_used;
  static int beginning;
  static int state = Q_START;
  static struct timespec last_nonzero_read;
  struct timespec when_read;
  int actual = 0;
  int new_used;
  int room;
  int exception;

  void handle_normal_readback(char *str, int len)
  {
    lua_pushstring(L, "ps_msg");
    if (discard_normal_readback)
      lua_pushinteger(L, len);
    else
      lua_pushlstring(L, str, len);
  };

  int scan_readback()
  {
    int cursor;
    unsigned char *ch = (unsigned char *) &in_buffer[buffer_used];

    for (cursor = buffer_used; cursor < new_used; cursor++)
      {
	state = dfa_transition[state][byte_class[*ch++]];
	switch (state)
	  {
	  case Q_START:
	    beginning = 0;
	    break;
	  case Q_PERCENT:
	    beginning = cursor;
	    break;
	  case Q_PERCENT_AGAIN:
	    beginning = cursor - 1;
	    break;
	  case Q_PS_MSG:
	    // End of PostScript Message.
	    handle_normal_readback(in_buffer, beginning);
	    lua_pushlstring(L,
			    &in_buffer[beginning + 4],
			    cursor - beginning - 9);
	    if (cursor < new_used)
	      {
		buffer_used = new_used - cursor - 1;
		memmove(in_buffer, &in_buffer[cursor + 1], buffer_used);
		state = Q_START;
		beginning = 0;
	      }
	    return 3;
	  case Q_STRAY_DELIM:
	    // Stray delimiter found.  Try to recover by setting state.
	    state = Q_START;
	    lua_pushstring(L, "stray_delimiter");
	    return 1;
	  }
      }
    return 0;
  };

  if (readback_available)
    {
      int error_fd = 0;
      char *explain[] = {
	"Can't read sentinel pipe",
	"Can't write sentinel pipe",
	"Can't fetch printer readback from printer"
      };

      if ((room = READBACK_BUFFER_SIZE - buffer_used) < READBACK_FREE_MINIMUM)
	luaL_error(L, "Printer readback buffer exhausted");

      actual = READ_PRINTER(&in_buffer[buffer_used], room, &error_fd);

      if (actual < 0)
	return posix_error(L, explain[error_fd]);

      CLOCK_GETTIME(CLOCK_TYPE, &when_read);
    }

  // Nothing read.
  if (actual == 0)
    {
      if (buffer_used &&
	  CLOCK_DIFF_MSEC(when_read, last_nonzero_read) > hold_time_msec)
	{
	  lua_pushboolean(L, 1);
	  handle_normal_readback(in_buffer, buffer_used);
	  buffer_used = 0;
	  state = Q_START;
	  beginning = 0;
	  lua_pushboolean(L, 0);
	  return 3;
	}

      return 0;
    }

  last_nonzero_read = when_read;

  // UEL and PJL are asynchronous, so handle them first.
  if (actual == 9 && !memcmp(&in_buffer[buffer_used], "\033%-12345X", 9))
    {
      lua_pushstring(L, "uel_read");
      return 1;
    }

  if (actual > 8 &&
      !memcmp(&in_buffer[buffer_used], "@PJL ", 5) &&
      !memcmp(&in_buffer[buffer_used + actual - 3], "\r\n\f", 3))
    {
      lua_pushstring(L, "pjl_msg");
      lua_pushlstring(L, &in_buffer[buffer_used + 5], actual - 6);
      return 2;
    }

  new_used = actual + buffer_used;

  if (exception = scan_readback())
    return exception;

  buffer_used = new_used;

  if (state == Q_START && buffer_used > 0)
    {
      handle_normal_readback(in_buffer, buffer_used);
      lua_pushboolean(L, 0);
      buffer_used = 0;
      return 3;
    }

  if (beginning == 0)
    return -1;

  handle_normal_readback(in_buffer, beginning);
  buffer_used -= beginning;
  memmove(in_buffer, &in_buffer[beginning], buffer_used);
  beginning = 0;
  lua_pushboolean(L, 0);
  return 3;
}

static int io_loop(lua_State *L)
{
  static int first_time_through = 1;
  int exception;
  static int write_time_set;
  fd_set writefds, readfds;
  struct timeval tv;
  int discard_normal_readback;
  static int skip_stdin;
  static struct timespec drain_elapsed;
  static char output_buffer[OUTPUT_BUFFER_SIZE];
  static int output_buffer_cursor, output_buffer_in_use;

  int append_to_output_buffer(lua_State *L, int posn)
  {
    const char *src;
    size_t len;

    src = luaL_checklstring(L, posn, &len);

    if (len > OUTPUT_BUFFER_SIZE - output_buffer_in_use)
      return -1;

    if (output_buffer_in_use > 0)
      {
	int base =
	  (output_buffer_cursor + output_buffer_in_use) &
	  (OUTPUT_BUFFER_SIZE - 1);

	if (base + len <= OUTPUT_BUFFER_SIZE)
	  memcpy(&output_buffer[base], src, len);
	else
	  {
	    int left_size = OUTPUT_BUFFER_SIZE - base;
	    memcpy(&output_buffer[base], src, left_size);
	    memcpy(output_buffer, &src[left_size], len - left_size);
	  }
      }
    else
      {
	output_buffer_cursor = 0;
	memcpy(output_buffer, src, len);
      }

    output_buffer_in_use += len;

    return 0;
  }

  void process_arguments()
  {
    static char
      add_to_output[] = "add_to_output",
      reset_write_stall_timer[] = "reset_write_stall_timer";

    lua_getfield(L, 1, "clear_buffer");
    if (lua_toboolean(L, -1))
      {
	output_buffer_cursor = 0;
	output_buffer_in_use = 0;
      }
    lua_pop(L, 1);

    lua_getfield(L, 1, reset_write_stall_timer);
    if (lua_toboolean(L, -1))
      write_time_set = 0;
    lua_pop(L, 1);
    lua_pushnil(L);
    lua_setfield(L, 1, reset_write_stall_timer);

    lua_getfield(L, 1, "freeze_input");
    if (!lua_isnil(L, -1) && (skip_stdin = lua_toboolean(L, -1)))
      // If the user freezes input, then he likely wants to know
      // when readback dries up.
      CLOCK_GETTIME(CLOCK_TYPE, &drain_elapsed);
    lua_pop(L, 1);

    lua_getfield(L, 1, "notices_only");
    discard_normal_readback = lua_toboolean(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, 1, add_to_output);
    if (!lua_isnil(L, -1) && append_to_output_buffer(L, -1) == -1)
      {
	// BUFFER EXHAUSTED!  Should never happen since inserts
	// only are done at BOJ and EOJ.  Why indeed?
	syslog(LOG_ERR, "Output buffer exhausted.  Why?");
	exit(JFAIL);
      }
    lua_pop(L, 1);
    lua_pushnil(L);
    lua_setfield(L, 1, add_to_output);
  }

  int check_lpstatus()
  {
    static int oldstatus = 0x18;
    int status;

    if (GETLPSTATUS(print_writefd, &status) == -1)
      return posix_error(L, "ioctl/lpgetstatus failed");

    if (oldstatus != status)
      {
	oldstatus = status;
	lua_pushstring(L, "printer_status");
	lua_pushinteger(L, status);
	return 2;
      }

    return 0;
  }

  void setup_select()
  {
    FD_ZERO(&readfds);
    FD_ZERO(&writefds);
    FD_SET(print_readfd, &readfds);
    if (output_buffer_in_use > 0)
      FD_SET(print_writefd, &writefds);
    else if (!skip_stdin)
      FD_SET(0, &readfds);
    tv.tv_usec = 1000 * select_wait_msec;
    tv.tv_sec = 0;
  }

  int is_stalled(struct timespec *last, int *is_set, int limit, char *errmsg)
  {
    struct timespec now;

    CLOCK_GETTIME(CLOCK_TYPE, &now);
    if (!*is_set)
      {
	*last = now;
	*is_set = 1;
      }
    else if (limit > 0 && CLOCK_DIFF_MSEC(now, *last) > limit)
      {
	lua_pushstring(L, errmsg);
	return 1;
      }

    return 0;
  }

  int try_write_printer()
  {
    static struct timespec last_write_time;
    int actual;
    int write_max;

    if (!FD_ISSET(print_writefd, &writefds))
      return is_stalled(&last_write_time,
			&write_time_set,
			write_stall_limit_msec, "write_stalled");

    write_max = OUTPUT_BUFFER_SIZE - output_buffer_cursor;
    if (write_limit && write_max > write_limit)
      write_max = write_limit;

    actual = WRITE_PRINTER(&output_buffer[output_buffer_cursor],
			   output_buffer_in_use > write_max
			   ? write_max : output_buffer_in_use);

    if (actual == -1)
      return posix_error(L, "printer write failed");

    CLOCK_GETTIME(CLOCK_TYPE, &last_write_time);
    // Any write may generate readback, so reset the drain timer.
    CLOCK_GETTIME(CLOCK_TYPE, &drain_elapsed);
    write_time_set = 1;
    output_buffer_cursor += actual;
    if (output_buffer_cursor == OUTPUT_BUFFER_SIZE)
      output_buffer_cursor = 0;
    output_buffer_in_use -= actual;

    return 0;
  }

  int try_read_stdin()
  {
    static int discard_remainder;
    static struct timespec last_read_time;
    static int time_set;
    static int first_line_seen;
    int actual;

    if (!FD_ISSET(0, &readfds))
      return is_stalled(&last_read_time,
			&time_set, read_stall_limit_msec, "input_stalled");

    switch (actual = READ_STDIN(output_buffer, OUTPUT_BUFFER_SIZE))
      {
      case -1:
	return posix_error(L, "stdin read failed");
      case 0:
	skip_stdin = 1;
	CLOCK_GETTIME(CLOCK_TYPE, &drain_elapsed);
	lua_pushstring(L, "eoj_reached");
	return 1;
      default:
	CLOCK_GETTIME(CLOCK_TYPE, &last_read_time);
	time_set = 1;

	if (!first_line_seen)
	  {
	    first_line_seen = 1;
	    if (actual < 4 || memcmp(output_buffer, "%!PS", 4))
	      {
		lua_pushstring(L, "not_postscript");
		return 1;
	      }
	  }

	if (strip_parity)
	  {
	    int i;

	    for (i = 0; i < actual; i++)
	      output_buffer[i] &= 0x7f;
	  }

	if (!lpsend_validate((unsigned char *) output_buffer, actual) &&
	    !discard_remainder)
	  {
	    discard_remainder = 1;
	    lua_pushstring(L, "invalid_data");
	    return 1;
	  }

	if (!discard_remainder)
	  {
	    output_buffer_in_use = actual;
	    output_buffer_cursor = 0;
	  }
      }

    return 0;
  }

  if (print_writefd < 0)
    luaL_error(L, "I/O not initialized");

  if (first_time_through)
    {
      first_time_through = 0;
      if (exception = check_lpstatus())
	return exception;
    }

  process_arguments();

  while (1)
    {
      int ready;
      int readback_available;
      sigset_t pending_set;
      static int sig_reported;

      setup_select();
      ready = SELECT(number_fds, &readfds, &writefds, NULL, &tv);
      sigpending(&pending_set);
      if (!sig_reported && sigismember(&pending_set, SIGINT))
	{
	  sig_reported = 1;
	  lua_pushstring(L, "got_signal");
	  return 1;
	}

      if (ready == -1)
	return posix_error(L, "select failed");

      if (ready == 0)
	{
	  if (exception = check_lpstatus())
	    return exception;

	  if (skip_stdin)
	    {
	      struct timespec now;

	      CLOCK_GETTIME(CLOCK_TYPE, &now);
	      if (CLOCK_DIFF_MSEC(now, drain_elapsed) > drain_time_msec)
		{
		  lua_pushstring(L, "readback_drained");
		  return 1;
		}
	    }
	}

      if (readback_available = FD_ISSET(print_readfd, &readfds))
	CLOCK_GETTIME(CLOCK_TYPE, &drain_elapsed);

      switch (exception =
	      process_readback(L, readback_available,
			       discard_normal_readback))
	{
	case -1:
	  continue;
	case 0:
	  break;
	default:
	  return exception;
	}

      if (output_buffer_in_use > 0)
	{
	  if (exception = try_write_printer())
	    return exception;
	  continue;
	}

      if (!skip_stdin && output_buffer_in_use == 0 &&
	  (exception = try_read_stdin()))
	return exception;
    }
}

static int check_vid_pid(lua_State *L)
{
  int id[2];
  int equal;

  if (ioctl(print_writefd, LPIOC_GET_VID_PID(sizeof(int[2])), id) == -1)
    {
      lua_pushnil(L);
      lua_pushinteger(L, errno);
      return 2;
    }

  lua_getfield(L, 1, "vid");
  lua_pushinteger(L, id[0]);
  equal = lua_rawequal(L, -1, -2);

  lua_getfield(L, 1, "pid");
  lua_pushinteger(L, id[1]);
  equal &= lua_rawequal(L, -1, -2);

  lua_pop(L, 4);
  lua_pushboolean(L, equal);
  return 1;
}

const luaL_Reg iofuncs[] = {
  {"io_loop", io_loop},
  {"check_vid_pid", check_vid_pid},
  {"set_timeouts", set_timeouts},
  {"initialize_io", initialize_io},
  {NULL, NULL}
};
