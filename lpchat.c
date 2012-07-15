/* lpchat - a simple utility for chatting with HP PostScript printers
 *
 * Christopher Oliver - 2012-6-3
 */

#define BUFSIZ (2<<14)

#include <pthread.h>
#include <err.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <linux/lp.h>
#include <linux/ioctl.h>
#include <sys/select.h>
#include <unistd.h>
#include <getopt.h>
#include <errno.h>

#define UEL "\033%-12345X"
#define SKIP_BAD_ESC "Skipping bad escape: "

#define IOCNR_GET_VID_PID 6
#define IOCNR_SOFT_RESET 7

#define LPIOC_GET_VID_PID(len) _IOC(_IOC_READ, 'P', IOCNR_GET_VID_PID, len)
#define LPIOC_SOFT_RESET _IOC(_IOC_NONE, 'P', IOCNR_SOFT_RESET, 0)

char *ctrl_names[] = {
  " NUL ", " SOH ", " STX ", " ETX ", " EOT ", " ENQ ", " ACK ",
  " BEL ", " BS ", " HT ", " LF ", " VT ", " FF ", " CR ", " SO ",
  " SI ", " DLE ", " XON ", " DC2 ", " XOFF ", " DC4 ", " NAK ",
  " SYN ", " ETB ", " CAN ", " EM ", " SUB ", " ESC ", " FS ",
  " GS ", " RS ", " US ", " SPACE "
};

pthread_mutex_t console_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_t reporter_thread;

int prtfd;

int intty, outtty;
int show_read_breaks, show_control, show_null, show_write_size;
int show_input = 1;
int postscript = 1, no_squeeze, pjl_short_cut, recognizer;
int show_usb_ids, soft_reset;

#define DFLT_COLOR -1
#define FLUSH_MSG "%%[ Flushing: rest of job "

void show(char *str, int len, int fgcolor, int bgcolor)
{
  pthread_mutex_lock(&console_lock);
  if (outtty && fgcolor != DFLT_COLOR)
    if (bgcolor != DFLT_COLOR)
      printf("\e[48;5;%d;38;5;%dm", bgcolor, fgcolor);
    else
      printf("\e[38;5;%dm", fgcolor);

  fwrite(str, len, 1, stdout);
  if (outtty && fgcolor != DFLT_COLOR)
    printf("\e[0m");
  fflush(stdout);
  pthread_mutex_unlock(&console_lock);
}

int squeeze(char *buf, int len)
{
  char ch, *src = buf, *dst = buf;

  while (len-- && *src++ != '\r')
    ++dst;

  while (len-- > 0)
    if ((ch = *src++) != '\r')
      *dst++ = ch;

  return dst - buf;
}


char *ctrl_as_string(unsigned int ch)
{
  static char hexbuf[8];

  if (ch < 128)
    return ch == '\177' ? " DEL " : ctrl_names[ch];

  snprintf(hexbuf, 8, " 0x%02X ", ch);
  return hexbuf;
}

show_buffer(unsigned char *buf, int remain, int show_control,
	    int main_color, int ctrl_color)
{
  int start = 0, end = 0, first_color = ctrl_color, second_color = 0, tmp;

  while (remain)
    {
      unsigned int ch = buf[end];

      if (ch > 31 && ch < 127)
	{
	  ++end;
	  remain--;
	  continue;
	}
      if (start != end)
	{
	  first_color = ctrl_color;
	  second_color = 0;
	  show(&buf[start], (end - start), main_color, DFLT_COLOR);
	}
      if (outtty)
	{
	  if (show_control)
	    {
	      char *ctrl = ctrl_as_string(ch);

	      show(ctrl, strlen(ctrl), first_color, second_color);
	    }
	}
      else
	show(&buf[end], 1, 0, 0);

      if (buf[end] == '\n')
	show("\n", 1, DFLT_COLOR, DFLT_COLOR);
      remain--;
      start = ++end;
      tmp = first_color;
      first_color = second_color;
      second_color = tmp;
    }
  if (start != end)
    show(&buf[start], (end - start), main_color, DFLT_COLOR);
}

void get_status()
{
  int status = 0x18;

  if (outtty)
    {
      int newstatus;

      if (ioctl(prtfd, LPGETSTATUS, &newstatus) < 0)
	err(1, "Can't get printer status");
      if (newstatus != status)
	{
	  char msg[64];
	  sprintf(msg, " Status is now %X ", (status = newstatus));
	  show(msg, strlen(msg), 226, 32);
	}
    }
}

void *reporter(void *dummy)
{
  unsigned char inbuf[4096];
  int actual;
  int last_actual = -1, null_count = 0;

  while ((actual = read(prtfd, inbuf, sizeof(inbuf))) >= 0 || errno == EAGAIN)
    {
      if (actual < 0)
	actual = 0;

      if (show_null)
	{
	  if (actual == 0)
	    {
	      if (show_null == 2)
		null_count++;
	      if (last_actual > 0)
		show(" ", 1, 23, 23);
	    }
	  else if (show_null == 2 && null_count > 0)
	    {
	      char numbuf[13];

	      snprintf(numbuf, 13, "<%d>", null_count);
	      show(numbuf, strlen(numbuf), 15, 23);
	      null_count = 0;
	    }

	  last_actual = actual;
	}

      if (no_squeeze || (actual = squeeze(inbuf, actual)))
	{
	  if (actual)
	    {
	      int recognized = 0;

	      if (recognizer && actual >= 5)
		{
		  if (!strncmp("@PJL ", inbuf, 5))
		    {
		      char *end = memchr(inbuf, 0x0C, actual);

		      if (!end)
			goto notfound;

		      recognized = 1;
		    }
		  else if (actual > sizeof(FLUSH_MSG) &&
			   !strncmp(inbuf, FLUSH_MSG, sizeof(FLUSH_MSG) - 1))
		    recognized = 1;
		}

	    notfound:

	      if (actual)
		if (recognized)
		  show_buffer(inbuf, actual, show_control, 25, 27);
		else
		  show_buffer(inbuf, actual, show_control, 9, 196);
	    }

	  if (actual > 0 && show_read_breaks && outtty)
	    show(" ", 1, 220, 220);
	}
      else
	usleep(20000);
    }

  err(1, "Trouble reading printer");
}

void buffer_append(char **buf_ptr, char *src, int src_len, int *buf_used)
{
  if (BUFSIZ < *buf_used + src_len)
    errx(1, "Too much junk!  Bailing.");

  memcpy(*buf_ptr, src, src_len);
  *buf_used += src_len;
  *buf_ptr += src_len;
}

#define ESCHELP "<ESC>: <ESC>   u: <UEL>   p: @PJL   /: <UEL>@PJL"

writer(char *printer)
{
  char inbuf[BUFSIZ], readbuf[BUFSIZ];
  int remain = 0, offset = 0, actual;
  struct timeval timeout;
  fd_set writefds;

  outtty = isatty(1);
  intty = isatty(0);

  if ((prtfd = open(printer, O_RDWR | O_NONBLOCK)) < 0)
    err(1, "Can't open printer %s", printer);

  if (show_usb_ids)
    {
      int id[2];
      if (ioctl(prtfd, LPIOC_GET_VID_PID(sizeof(int[2])), &id) == -1)
	warn("trouble getting VID/PID");
      else
	printf("VID = %04X / PID = %04X\n", id[0], id[1]);
    }

  if (soft_reset)
    if (ioctl(prtfd, LPIOC_SOFT_RESET) == -1 && errno != ETIMEDOUT)
      warn("trouble resetting printer");


  pthread_create(&reporter_thread, NULL, reporter, NULL);

  get_status();

  if (postscript)
    {
      strcpy(inbuf, "%!PS-Adobe\n");
      remain = strlen(inbuf);
    }

  while (1)
    {
      if (!remain)
	{
	  char *dst = inbuf, *src = readbuf;
	  char msgbuf[32];

	  if (outtty & intty)
	    show("> ", 2, 4, DFLT_COLOR);
	  if ((actual = read(0, readbuf, sizeof(inbuf))) < 0)
	    err(1, "trouble reading input");

	  if (!actual)
	    break;

	  while (actual)
	    {
	      if (pjl_short_cut && *src == '\033' && actual)
		{
		  switch (*++src)
		    {
		    case '\033':
		      buffer_append(&dst, "\033", 1, &remain);
		      ++src;
		    case 'u':
		      buffer_append(&dst, UEL, sizeof(UEL) - 1, &remain);
		      ++src;
		      break;
		    case '/':
		      buffer_append(&dst, UEL, sizeof(UEL) - 1, &remain);
		    case 'p':
		      buffer_append(&dst, "@PJL", 4, &remain);
		      ++src;
		      break;
		    case '\012':
		      src++;
		      break;
		    case '?':
		      show(ESCHELP, sizeof(ESCHELP) - 1, 226, 0);
		      show("\n", 1, DFLT_COLOR, DFLT_COLOR);
		      src++;
		      break;
		    default:
		      if (*src < 33 || *src >= 127)
			snprintf(msgbuf, sizeof(msgbuf),
				 " Skipping bad escape: %s ",
				 ctrl_as_string(*src));
		      else
			snprintf(msgbuf, sizeof(msgbuf),
				 " Skipping bad escape: %c ", *src);
		      show(msgbuf, strlen(msgbuf), 226, 0);
		      show("\n", 1, DFLT_COLOR, DFLT_COLOR);
		      src++;
		    }
		  actual -= 2;
		  continue;
		}

	      actual--;
	      buffer_append(&dst, src++, 1, &remain);
	    }

	  if (show_input && !intty && outtty)
	    show_buffer(inbuf, remain, 1, 10, 46);

	  offset = 0;
	}

      if (!remain)
	continue;


      FD_ZERO(&writefds);
      FD_SET(prtfd, &writefds);
      timeout.tv_sec = 0;
      timeout.tv_usec = 100000;
      switch (select(prtfd + 1, NULL, &writefds, NULL, &timeout))
	{
	case 0:
	  get_status();
	  continue;
	case -1:
	  err(1, "trouble writing printer");
	}
      if ((actual = write(prtfd, &inbuf[offset], remain)) < 0)
	if (errno != EAGAIN)
	  err(1, "trouble writing printer");
	else
	  actual = 0;

      if (show_write_size && outtty)
	{
	  char numbuf[13];

	  snprintf(numbuf, 13, "<%d>", actual);
	  show(numbuf, strlen(numbuf), 15, 52);
	}

      remain -= actual;
      offset += actual;
    }


  if (postscript)
    {
      while (!(actual = write(prtfd, "\004", 1)));
      if (actual < 0)
	err(1, "Can't send EOF to printer");

      if ((show_input || intty) && outtty)
	{
	  show("\n", 1, DFLT_COLOR, DFLT_COLOR);
	  show(" EOT sent ", 10, 46, 0);
	  show("\n", 1, DFLT_COLOR, DFLT_COLOR);
	}
    }

  sleep(intty ? 2 : 10);
}

char helpmsg[] =
  "\nOptions: \n"
  "\t-b\tShow read breaks\n"
  "\t-c\tShow control characters\n"
  "\t-h\tPrint this help\n"
  "\t-n\tShow null read marker following non-null read\n"
  "\t-N\tShow null read count following non-null read\n"
  "\t-p\tSuppress PostScript introducer\n"
  "\t-q\tDon't echo input if from a file.\n"
  "\t-r\tAttempt to recognize status printback\n"
  "\t-s\tEnable PJL shortcuts\n"
  "\t-w\tShow write sizes\n" "\t-z\tDon't squeeze <CR> from input\n";

int main(int argc, char *argv[])
{
  int opt;
  char *usage =
    "Usage: %s "
    "[-b] [-c] [-h] [-nN] [-p] [-q] [-r] [-s] [-w] [-z] /dev/lpfoo\n";

  while ((opt = getopt(argc, argv, "bchnNpqrRsuwz")) != -1)
    switch (opt)
      {
      case 'b':
	show_read_breaks = 1;
	break;
      case 'c':
	show_control = 1;
	break;
      case 'h':
	fprintf(stderr, usage, argv[0]);
	fprintf(stderr, "%s", helpmsg);
	exit(0);
      case 'n':
	show_null = 1;
	break;
      case 'N':
	show_null = 2;
	break;
      case 'p':
	postscript = 0;
	break;
      case 'q':
	show_input = 0;
	break;
      case 's':
	pjl_short_cut = 1;
	break;
      case 'r':
	recognizer = 1;
	break;
      case 'R':
	soft_reset = 1;
	break;
      case 'u':
	show_usb_ids = 1;
	break;
      case 'w':
	show_write_size = 1;
	break;
      case 'z':
	no_squeeze = 1;
	break;
      default:
	fprintf(stderr, usage, argv[0]);
	exit(1);
      }

  if (optind != argc - 1)
    {
      fprintf(stderr, "%s: Invalid argument\n", argv[0]);
      fprintf(stderr, usage, argv[0]);
      exit(1);
    }

  writer(argv[optind]);
  puts("");
  return 0;
}
