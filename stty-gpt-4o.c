/* stty -- change and print terminal line settings
   Copyright (C) 1990-2025 Free Software Foundation, Inc.

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <https://www.gnu.org/licenses/>.  */

/* Usage: stty [-ag] [--all] [--save] [-F device] [--file=device] [setting...]

   Options:
   -a, --all    Write all current settings to stdout in human-readable form.
   -g, --save   Write all current settings to stdout in stty-readable form.
   -F, --file   Open and use the specified device instead of standard input.

   If no args are given, write to stdout the baud rate and settings that
   have been changed from their defaults.  Mode reading and changes
   are done on the specified device, or stdin if none was specified.

   David MacKenzie <djm@gnu.ai.mit.edu> */

#include <config.h>

#ifdef TERMIOS_NEEDS_XOPEN_SOURCE
# define _XOPEN_SOURCE
#endif

#include <stdio.h>
#include <sys/types.h>

#include <termios.h>
#if HAVE_STROPTS_H
# include <stropts.h>
#endif
#include <sys/ioctl.h>

#ifdef WINSIZE_IN_PTEM
# include <sys/stream.h>
# include <sys/ptem.h>
#endif
#ifdef GWINSZ_IN_SYS_PTY
# include <sys/tty.h>
# include <sys/pty.h>
#endif
#include <getopt.h>
#include <stdarg.h>

#include "system.h"
#include "assure.h"
#include "c-ctype.h"
#include "fd-reopen.h"
#include "quote.h"
#include "xdectoint.h"
#include "xstrtol.h"

/* The official name of this program (e.g., no 'g' prefix).  */
#define PROGRAM_NAME "stty"

#define AUTHORS proper_name ("David MacKenzie")

#ifndef _POSIX_VDISABLE
# define _POSIX_VDISABLE 0
#endif

#define Control(c) ((c) & 0x1f)
/* Canonical values for control characters. */
#ifndef CINTR
# define CINTR Control ('c')
#endif
#ifndef CQUIT
# define CQUIT 28
#endif
#ifndef CERASE
# define CERASE 127
#endif
#ifndef CKILL
# define CKILL Control ('u')
#endif
#ifndef CEOF
# define CEOF Control ('d')
#endif
#ifndef CEOL
# define CEOL _POSIX_VDISABLE
#endif
#ifndef CSTART
# define CSTART Control ('q')
#endif
#ifndef CSTOP
# define CSTOP Control ('s')
#endif
#ifndef CSUSP
# define CSUSP Control ('z')
#endif
#if defined VEOL2 && !defined CEOL2
# define CEOL2 _POSIX_VDISABLE
#endif
/* Some platforms have VSWTC, others VSWTCH.  In both cases, this control
   character is initialized by CSWTCH, if present.  */
#if defined VSWTC && !defined VSWTCH
# define VSWTCH VSWTC
#endif
/* ISC renamed swtch to susp for termios, but we'll accept either name.  */
#if defined VSUSP && !defined VSWTCH
# define VSWTCH VSUSP
# if defined CSUSP && !defined CSWTCH
#  define CSWTCH CSUSP
# endif
#endif
#if defined VSWTCH && !defined CSWTCH
# define CSWTCH _POSIX_VDISABLE
#endif

/* SunOS >= 5.3 loses (^Z doesn't work) if 'swtch' is the same as 'susp'.
   So the default is to disable 'swtch.'  */
#if defined __sun
# undef CSWTCH
# define CSWTCH _POSIX_VDISABLE
#endif

#if defined VWERSE && !defined VWERASE	/* AIX-3.2.5 */
# define VWERASE VWERSE
#endif
#if defined VDSUSP && !defined CDSUSP
# define CDSUSP Control ('y')
#endif
#if !defined VREPRINT && defined VRPRNT /* Irix 4.0.5 */
# define VREPRINT VRPRNT
#endif
#if defined VREPRINT && !defined CRPRNT
# define CRPRNT Control ('r')
#endif
#if defined CREPRINT && !defined CRPRNT
# define CRPRNT Control ('r')
#endif
#if defined VWERASE && !defined CWERASE
# define CWERASE Control ('w')
#endif
#if defined VLNEXT && !defined CLNEXT
# define CLNEXT Control ('v')
#endif
#if defined VDISCARD && !defined VFLUSHO
# define VFLUSHO VDISCARD
#endif
#if defined VFLUSH && !defined VFLUSHO	/* Ultrix 4.2 */
# define VFLUSHO VFLUSH
#endif
#if defined CTLECH && !defined ECHOCTL	/* Ultrix 4.3 */
# define ECHOCTL CTLECH
#endif
#if defined TCTLECH && !defined ECHOCTL	/* Ultrix 4.2 */
# define ECHOCTL TCTLECH
#endif
#if defined CRTKIL && !defined ECHOKE	/* Ultrix 4.2 and 4.3 */
# define ECHOKE CRTKIL
#endif
#if defined VFLUSHO && !defined CFLUSHO
# define CFLUSHO Control ('o')
#endif
#if defined VSTATUS && !defined CSTATUS
# define CSTATUS Control ('t')
#endif

/* Which speeds to set.  */
enum speed_setting
  {
    input_speed, output_speed, both_speeds
  };

/* What to output and how.  */
enum output_type
  {
    changed, all, recoverable	/* Default, -a, -g.  */
  };

/* Which member(s) of 'struct termios' a mode uses.  */
enum mode_type
  {
    control, input, output, local, combination
  };

/* Flags for 'struct mode_info'. */
#define SANE_SET 1		/* Set in 'sane' mode. */
#define SANE_UNSET 2		/* Unset in 'sane' mode. */
#define REV 4			/* Can be turned off by prepending '-'. */
#define OMIT 8			/* Don't display value. */
#define NO_SETATTR 16		/* tcsetattr not used to set mode bits.  */

/* Each mode.  */
struct mode_info
  {
    char const *name;		/* Name given on command line.  */
    enum mode_type type;	/* Which structure element to change. */
    char flags;			/* Setting and display options.  */
    unsigned long bits;		/* Bits to set for this mode.  */
    unsigned long mask;		/* Other bits to turn off for this mode.  */
  };

static struct mode_info const mode_info[] =
{
  {"parenb", control, REV, PARENB, 0},
  {"parodd", control, REV, PARODD, 0},
#ifdef CMSPAR
  {"cmspar", control, REV, CMSPAR, 0},
#endif
  {"cs5", control, 0, CS5, CSIZE},
  {"cs6", control, 0, CS6, CSIZE},
  {"cs7", control, 0, CS7, CSIZE},
  {"cs8", control, 0, CS8, CSIZE},
  {"hupcl", control, REV, HUPCL, 0},
  {"hup", control, REV | OMIT, HUPCL, 0},
  {"cstopb", control, REV, CSTOPB, 0},
  {"cread", control, SANE_SET | REV, CREAD, 0},
  {"clocal", control, REV, CLOCAL, 0},
#ifdef CRTSCTS
  {"crtscts", control, REV, CRTSCTS, 0},
#endif
#ifdef CDTRDSR
  {"cdtrdsr", control, REV, CDTRDSR, 0},
#endif

  {"ignbrk", input, SANE_UNSET | REV, IGNBRK, 0},
  {"brkint", input, SANE_SET | REV, BRKINT, 0},
  {"ignpar", input, REV, IGNPAR, 0},
  {"parmrk", input, REV, PARMRK, 0},
  {"inpck", input, REV, INPCK, 0},
  {"istrip", input, REV, ISTRIP, 0},
  {"inlcr", input, SANE_UNSET | REV, INLCR, 0},
  {"igncr", input, SANE_UNSET | REV, IGNCR, 0},
  {"icrnl", input, SANE_SET | REV, ICRNL, 0},
  {"ixon", input, REV, IXON, 0},
  {"ixoff", input, SANE_UNSET | REV, IXOFF, 0},
  {"tandem", input, REV | OMIT, IXOFF, 0},
#ifdef IUCLC
  {"iuclc", input, SANE_UNSET | REV, IUCLC, 0},
#endif
#ifdef IXANY
  {"ixany", input, SANE_UNSET | REV, IXANY, 0},
#endif
#ifdef IMAXBEL
  {"imaxbel", input, SANE_SET | REV, IMAXBEL, 0},
#endif
#ifdef IUTF8
  {"iutf8", input, SANE_UNSET | REV, IUTF8, 0},
#endif

  {"opost", output, SANE_SET | REV, OPOST, 0},
#ifdef OLCUC
  {"olcuc", output, SANE_UNSET | REV, OLCUC, 0},
#endif
#ifdef OCRNL
  {"ocrnl", output, SANE_UNSET | REV, OCRNL, 0},
#endif
#ifdef ONLCR
  {"onlcr", output, SANE_SET | REV, ONLCR, 0},
#endif
#ifdef ONOCR
  {"onocr", output, SANE_UNSET | REV, ONOCR, 0},
#endif
#ifdef ONLRET
  {"onlret", output, SANE_UNSET | REV, ONLRET, 0},
#endif
#ifdef OFILL
  {"ofill", output, SANE_UNSET | REV, OFILL, 0},
#endif
#ifdef OFDEL
  {"ofdel", output, SANE_UNSET | REV, OFDEL, 0},
#endif
#ifdef NLDLY
  {"nl1", output, SANE_UNSET, NL1, NLDLY},
  {"nl0", output, SANE_SET, NL0, NLDLY},
#endif
#ifdef CRDLY
  {"cr3", output, SANE_UNSET, CR3, CRDLY},
  {"cr2", output, SANE_UNSET, CR2, CRDLY},
  {"cr1", output, SANE_UNSET, CR1, CRDLY},
  {"cr0", output, SANE_SET, CR0, CRDLY},
#endif
#ifdef TABDLY
# ifdef TAB3
  {"tab3", output, SANE_UNSET, TAB3, TABDLY},
# endif
# ifdef TAB2
  {"tab2", output, SANE_UNSET, TAB2, TABDLY},
# endif
# ifdef TAB1
  {"tab1", output, SANE_UNSET, TAB1, TABDLY},
# endif
# ifdef TAB0
  {"tab0", output, SANE_SET, TAB0, TABDLY},
# endif
#else
# ifdef OXTABS
  {"tab3", output, SANE_UNSET, OXTABS, 0},
# endif
#endif
#ifdef BSDLY
  {"bs1", output, SANE_UNSET, BS1, BSDLY},
  {"bs0", output, SANE_SET, BS0, BSDLY},
#endif
#ifdef VTDLY
  {"vt1", output, SANE_UNSET, VT1, VTDLY},
  {"vt0", output, SANE_SET, VT0, VTDLY},
#endif
#ifdef FFDLY
  {"ff1", output, SANE_UNSET, FF1, FFDLY},
  {"ff0", output, SANE_SET, FF0, FFDLY},
#endif

  {"isig", local, SANE_SET | REV, ISIG, 0},
  {"icanon", local, SANE_SET | REV, ICANON, 0},
#ifdef IEXTEN
  {"iexten", local, SANE_SET | REV, IEXTEN, 0},
#endif
  {"echo", local, SANE_SET | REV, ECHO, 0},
  {"echoe", local, SANE_SET | REV, ECHOE, 0},
  {"crterase", local, REV | OMIT, ECHOE, 0},
  {"echok", local, SANE_SET | REV, ECHOK, 0},
  {"echonl", local, SANE_UNSET | REV, ECHONL, 0},
  {"noflsh", local, SANE_UNSET | REV, NOFLSH, 0},
#ifdef XCASE
  {"xcase", local, SANE_UNSET | REV, XCASE, 0},
#endif
#ifdef TOSTOP
  {"tostop", local, SANE_UNSET | REV, TOSTOP, 0},
#endif
#ifdef ECHOPRT
  {"echoprt", local, SANE_UNSET | REV, ECHOPRT, 0},
  {"prterase", local, REV | OMIT, ECHOPRT, 0},
#endif
#ifdef ECHOCTL
  {"echoctl", local, SANE_SET | REV, ECHOCTL, 0},
  {"ctlecho", local, REV | OMIT, ECHOCTL, 0},
#endif
#ifdef ECHOKE
  {"echoke", local, SANE_SET | REV, ECHOKE, 0},
  {"crtkill", local, REV | OMIT, ECHOKE, 0},
#endif
#ifdef FLUSHO
  {"flusho", local, SANE_UNSET | REV, FLUSHO, 0},
#endif
#if defined TIOCEXT
  {"extproc", local, SANE_UNSET | REV | NO_SETATTR, EXTPROC, 0},
#elif defined EXTPROC
  {"extproc", local, SANE_UNSET | REV, EXTPROC, 0},
#endif

  {"evenp", combination, REV | OMIT, 0, 0},
  {"parity", combination, REV | OMIT, 0, 0},
  {"oddp", combination, REV | OMIT, 0, 0},
  {"nl", combination, REV | OMIT, 0, 0},
  {"ek", combination, OMIT, 0, 0},
  {"sane", combination, OMIT, 0, 0},
  {"cooked", combination, REV | OMIT, 0, 0},
  {"raw", combination, REV | OMIT, 0, 0},
  {"pass8", combination, REV | OMIT, 0, 0},
  {"litout", combination, REV | OMIT, 0, 0},
  {"cbreak", combination, REV | OMIT, 0, 0},
#ifdef IXANY
  {"decctlq", combination, REV | OMIT, 0, 0},
#endif
#if defined TABDLY || defined OXTABS
  {"tabs", combination, REV | OMIT, 0, 0},
#endif
#if defined XCASE && defined IUCLC && defined OLCUC
  {"lcase", combination, REV | OMIT, 0, 0},
  {"LCASE", combination, REV | OMIT, 0, 0},
#endif
  {"crt", combination, OMIT, 0, 0},
  {"dec", combination, OMIT, 0, 0},

  {nullptr, control, 0, 0, 0}
};

/* Control character settings.  */
struct control_info
  {
    char const *name;		/* Name given on command line.  */
    cc_t saneval;		/* Value to set for 'stty sane'.  */
    size_t offset;		/* Offset in c_cc.  */
  };

/* Control characters. */

static struct control_info const control_info[] =
{
  {"intr", CINTR, VINTR},
  {"quit", CQUIT, VQUIT},
  {"erase", CERASE, VERASE},
  {"kill", CKILL, VKILL},
  {"eof", CEOF, VEOF},
  {"eol", CEOL, VEOL},
#ifdef VEOL2
  {"eol2", CEOL2, VEOL2},
#endif
#ifdef VSWTCH
  {"swtch", CSWTCH, VSWTCH},
#endif
  {"start", CSTART, VSTART},
  {"stop", CSTOP, VSTOP},
  {"susp", CSUSP, VSUSP},
#ifdef VDSUSP
  {"dsusp", CDSUSP, VDSUSP},
#endif
#ifdef VREPRINT
  {"rprnt", CRPRNT, VREPRINT},
#else
# ifdef CREPRINT /* HPUX 10.20 needs this */
  {"rprnt", CRPRNT, CREPRINT},
# endif
#endif
#ifdef VWERASE
  {"werase", CWERASE, VWERASE},
#endif
#ifdef VLNEXT
  {"lnext", CLNEXT, VLNEXT},
#endif
#ifdef VFLUSHO
  {"flush", CFLUSHO, VFLUSHO},   /* deprecated compat option.  */
  {"discard", CFLUSHO, VFLUSHO},
#endif
#ifdef VSTATUS
  {"status", CSTATUS, VSTATUS},
#endif

  /* These must be last because of the display routines. */
  {"min", 1, VMIN},
  {"time", 0, VTIME},
  {nullptr, 0, 0}
};

static char const *visible (cc_t ch);
static unsigned long int baud_to_value (speed_t speed);
static bool recover_mode (char const *arg, struct termios *mode);
static int screen_columns (void);
static bool set_mode (struct mode_info const *info, bool reversed,
                      struct termios *mode);
static bool eq_mode (struct termios *mode1, struct termios *mode2);
static uintmax_t integer_arg (char const *s, uintmax_t max);
static speed_t string_to_baud (char const *arg);
static tcflag_t *mode_type_flag (enum mode_type type, struct termios *mode);
static void display_all (struct termios *mode, char const *device_name);
static void display_changed (struct termios *mode);
static void display_recoverable (struct termios *mode);
static void display_settings (enum output_type output_type,
                              struct termios *mode,
                              char const *device_name);
static void check_speed (struct termios *mode);
static void display_speed (struct termios *mode, bool fancy);
static void display_window_size (bool fancy, char const *device_name);
static void sane_mode (struct termios *mode);
static void set_control_char (struct control_info const *info,
                              char const *arg,
                              struct termios *mode);
static void set_speed (enum speed_setting type, char const *arg,
                       struct termios *mode);
static void set_window_size (int rows, int cols, char const *device_name);

/* The width of the screen, for output wrapping. */
static int max_col;

/* Current position, to know when to wrap. */
static int current_col;

/* Default "drain" mode for tcsetattr.  */
static int tcsetattr_options = TCSADRAIN;

/* Extra info to aid stty development.  */
static bool dev_debug;

/* Record last speed set for correlation.  */
static speed_t last_ibaud = (speed_t) -1;
static speed_t last_obaud = (speed_t) -1;

/* For long options that have no equivalent short option, use a
   non-character as a pseudo short option, starting with CHAR_MAX + 1.  */
enum
{
  DEV_DEBUG_OPTION = CHAR_MAX + 1,
};

static struct option const longopts[] =
{
  {"all", no_argument, nullptr, 'a'},
  {"save", no_argument, nullptr, 'g'},
  {"file", required_argument, nullptr, 'F'},
  {"-debug", no_argument, nullptr, DEV_DEBUG_OPTION},
  {GETOPT_HELP_OPTION_DECL},
  {GETOPT_VERSION_OPTION_DECL},
  {nullptr, 0, nullptr, 0}
};

/* Print format string MESSAGE and optional args.
   Wrap to next line first if it won't fit.
   Print a space first unless MESSAGE will start a new line. */

ATTRIBUTE_FORMAT ((printf, 1, 2))
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

static void safeOutput(const char *message, ...) {
    va_list args;
    char *buf = NULL;

    va_start(args, message);
    int buflen = vasprintf(&buf, message, args);
    va_end(args);

    if (buflen < 0) {
        xalloc_die();
    }

    if (current_col > 0) {
        if (max_col - current_col <= buflen) {
            putchar('\n');
            current_col = 0;
        } else {
            putchar(' ');
            current_col++;
        }
    }

    if (buf) {
        fputs(buf, stdout);
        free(buf);
        current_col += buflen;
    }
}

#include <stdio.h>
#include <stdlib.h>

void usage(int status) {
    if (status != EXIT_SUCCESS) {
        emit_try_help();
        exit(status);
    }
    
    printf(_("Usage: %s [-F DEVICE | --file=DEVICE] [SETTING]...\n"
             "  or:  %s [-F DEVICE | --file=DEVICE] [-a|--all]\n"
             "  or:  %s [-F DEVICE | --file=DEVICE] [-g|--save]\n"),
            program_name, program_name, program_name);
    printf(_("Print or change terminal characteristics.\n"));

    emit_mandatory_arg_note();

    printf(_(
           "  -a, --all          print all current settings in human-readable form\n"
           "  -g, --save         print all current settings in a stty-readable form\n"
           "  -F, --file=DEVICE  open and use DEVICE instead of standard input\n"));
    fputs(HELP_OPTION_DESCRIPTION, stdout);
    fputs(VERSION_OPTION_DESCRIPTION, stdout);
    printf(_(
           "\nOptional - before SETTING indicates negation.  An * marks non-POSIX\n"
           "settings.  The underlying system defines which settings are available.\n"
           "\nSpecial characters:\n"));

    struct SpecialSetting {
        const char *description;
#ifdef VFLUSHO
        const char *vflusho;
#endif
#ifdef VDSUSP
        const char *vdsusp;
#endif
#ifdef VEOL2
        const char *veol2;
#endif
#ifdef VLNEXT
        const char *vlnext;
#endif
#ifdef VSTATUS
        const char *vstatus;
#endif
#if defined CREPRINT || defined VREPRINT
        const char *vreprint;
#endif
#ifdef VSWTCH
        const char *vswtch;
#endif
#ifdef VWERASE
        const char *vwerase;
#endif
    };

    struct SpecialSetting settings[] = {
        {" * discard CHAR  CHAR will toggle discarding of output\n"},
#ifdef VFLUSHO
        {" * dsusp CHAR    CHAR will send a terminal stop signal once input flushed\n"},
#endif
        {"   eof CHAR      CHAR will send an end of file (terminate the input)\n"},
        {"   eol CHAR      CHAR will end the line\n"},
#ifdef VEOL2
        {" * eol2 CHAR     alternate CHAR for ending the line\n"},
#endif
        {"   erase CHAR    CHAR will erase the last character typed\n"},
        {"   intr CHAR     CHAR will send an interrupt signal\n"},
        {"   kill CHAR     CHAR will erase the current line\n"},
#ifdef VLNEXT
        {" * lnext CHAR    CHAR will enter the next character quoted\n"},
#endif
#ifdef VSTATUS
        {" * status CHAR   CHAR will send an info signal\n"},
#endif
        {"   quit CHAR     CHAR will send a quit signal\n"},
#if defined CREPRINT || defined VREPRINT
        {" * rprnt CHAR    CHAR will redraw the current line\n"},
#endif
        {"   start CHAR    CHAR will restart the output after stopping it\n"},
        {"   stop CHAR     CHAR will stop the output\n"},
        {"   susp CHAR     CHAR will send a terminal stop signal\n"},
#ifdef VSWTCH
        {" * swtch CHAR    CHAR will switch to a different shell layer\n"},
#endif
#ifdef VWERASE
        {" * werase CHAR   CHAR will erase the last word typed\n"},
#endif
    };

    for (int i = 0; i < sizeof(settings) / sizeof(settings[0]); i++) {
        fputs(settings[i].description, stdout);
    }

    printf(_(
           "\nSpecial settings:\n"
           "   N             set the input and output speeds to N bauds\n"));
#ifdef TIOCGWINSZ
    printf(_("   cols N        tell the kernel that the terminal has N columns\n"
             " * columns N     same as cols N\n"));
#endif
    printf(_(" * [-]drain      wait for transmission before applying settings (%s by default)\n"),
           tcsetattr_options == TCSADRAIN ? _("on") : _("off"));
    printf(_("   ispeed N      set the input speed to N\n"));
#ifdef HAVE_C_LINE
    printf(_(" * line N        use line discipline N\n"));
#endif
    printf(_("   min N         with -icanon, set N characters minimum for a completed read\n"
             "   ospeed N      set the output speed to N\n"));
#ifdef TIOCGWINSZ
    printf(_("   rows N        tell the kernel that the terminal has N rows\n"
             "   size          print the number of rows and columns according to the kernel\n"));
#endif
    printf(_("   speed         print the terminal speed\n"
             "   time N        with -icanon, set read timeout of N tenths of a second\n"
             "\nControl settings:\n"
             "   [-]clocal     disable modem control signals\n"
             "   [-]cread      allow input to be received\n"));
#ifdef CRTSCTS
    printf(_(" * [-]crtscts    enable RTS/CTS handshaking\n"));
#endif
#ifdef CDTRDSR
    printf(_(" * [-]cdtrdsr    enable DTR/DSR handshaking\n"));
#endif
    printf(_("   csN           set character size to N bits, N in [5..8]\n"
             "   [-]cstopb     use two stop bits per character (one with '-')\n"
             "   [-]hup        send a hangup signal when the last process closes the tty\n"
             "   [-]hupcl      same as [-]hup\n"
             "   [-]parenb     generate parity bit in output and expect parity bit in input\n"
             "   [-]parodd     set odd parity (or even parity with '-')\n"));
#ifdef CMSPAR
    printf(_(" * [-]cmspar     use \"stick\" (mark/space) parity\n"));
#endif
    printf(_("\nInput settings:\n"
             "   [-]brkint     breaks cause an interrupt signal\n"
             "   [-]icrnl      translate carriage return to newline\n"
             "   [-]ignbrk     ignore break characters\n"
             "   [-]igncr      ignore carriage return\n"
             "   [-]ignpar     ignore characters with parity errors\n"));
#ifdef IMAXBEL
    printf(_(" * [-]imaxbel    beep and do not flush a full input buffer on a character\n"));
#endif
    printf(_("   [-]inlcr      translate newline to carriage return\n"
             "   [-]inpck      enable input parity checking\n"
             "   [-]istrip     clear high (8th) bit of input characters\n"));
#ifdef IUTF8
    printf(_(" * [-]iutf8      assume input characters are UTF-8 encoded\n"));
#endif
#ifdef IUCLC
    printf(_(" * [-]iuclc      translate uppercase characters to lowercase\n"));
#endif
#ifdef IXANY
    printf(_(" * [-]ixany      let any character restart output, not only start character\n"));
#endif
    printf(_("   [-]ixoff      enable sending of start/stop characters\n"
             "   [-]ixon       enable XON/XOFF flow control\n"
             "   [-]parmrk     mark parity errors (with a 255-0-character sequence)\n"
             "   [-]tandem     same as [-]ixoff\n"
             "\nOutput settings:\n"));
#ifdef BSDLY
    printf(_(" * bsN           backspace delay style, N in [0..1]\n"));
#endif
#ifdef CRDLY
    printf(_(" * crN           carriage return delay style, N in [0..3]\n"));
#endif
#ifdef FFDLY
    printf(_(" * ffN           form feed delay style, N in [0..1]\n"));
#endif
#ifdef NLDLY
    printf(_(" * nlN           newline delay style, N in [0..1]\n"));
#endif
#ifdef OCRNL
    printf(_(" * [-]ocrnl      translate carriage return to newline\n"));
#endif
#ifdef OFDEL
    printf(_(" * [-]ofdel      use delete characters for fill instead of NUL characters\n"));
#endif
#ifdef OFILL
    printf(_(" * [-]ofill      use fill (padding) characters instead of timing for delays\n"));
#endif
#ifdef OLCUC
    printf(_(" * [-]olcuc      translate lowercase characters to uppercase\n"));
#endif
#ifdef ONLCR
    printf(_(" * [-]onlcr      translate newline to carriage return-newline\n"));
#endif
#ifdef ONLRET
    printf(_(" * [-]onlret     newline performs a carriage return\n"));
#endif
#ifdef ONOCR
    printf(_(" * [-]onocr      do not print carriage returns in the first column\n"));
#endif
    printf(_("   [-]opost      postprocess output\n"));
#if defined TABDLY || defined OXTABS
    printf(_(" * tabN          horizontal tab delay style, N in [0..3]\n"
             " * tabs          same as tab0\n"
             " * -tabs         same as tab3\n"));
#endif
#ifdef VTDLY
    printf(_(" * vtN           vertical tab delay style, N in [0..1]\n"));
#endif
    printf(_("\nLocal settings:\n"
             "   [-]crterase   echo erase characters as backspace-space-backspace\n"));
#ifdef ECHOKE
    printf(_(" * crtkill       kill all line by obeying the echoprt and echoe settings\n"
             " * -crtkill      kill all line by obeying the echoctl and echok settings\n"));
#endif
#ifdef ECHOCTL
    printf(_(" * [-]ctlecho    echo control characters in hat notation ('^c')\n"));
#endif
    printf(_("   [-]echo       echo input characters\n"));
#ifdef ECHOCTL
    printf(_(" * [-]echoctl    same as [-]ctlecho\n"));
#endif
    printf(_("   [-]echoe      same as [-]crterase\n"
             "   [-]echok      echo a newline after a kill character\n"));
#ifdef ECHOKE
    printf(_(" * [-]echoke     same as [-]crtkill\n"));
#endif
    printf(_("   [-]echonl     echo newline even if not echoing other characters\n"));
#ifdef ECHOPRT
    printf(_(" * [-]echoprt    echo erased characters backward, between '\\' and '/'\n"));
#endif
#if defined EXTPROC || defined TIOCEXT
    printf(_(" * [-]extproc    enable \"LINEMODE\"; useful with high latency links\n"));
#endif
#if defined FLUSHO
    printf(_(" * [-]flusho     discard output\n"));
#endif
    printf(_("   [-]icanon     enable special characters: erase, kill\n"));
    printf(_("   [-]iexten     enable non-POSIX special characters\n"
             "   [-]isig       enable interrupt, quit, and suspend special characters\n"
             "   [-]noflsh     disable flushing after interrupt and quit special characters\n"));
#ifdef ECHOPRT
    printf(_(" * [-]prterase   same as [-]echoprt\n"));
#endif
#ifdef TOSTOP
    printf(_(" * [-]tostop     stop background jobs that try to write to the terminal\n"));
#endif
#ifdef XCASE
    printf(_(" * [-]xcase      with icanon, escape with '\\' for uppercase characters\n"));
#endif
    printf(_("\nCombination settings:\n"));
#if defined XCASE && defined IUCLC && defined OLCUC
    printf(_(" * [-]LCASE      same as [-]lcase\n"));
#endif
    printf(_("   cbreak        same as -icanon\n"
             "   -cbreak       same as icanon\n"
             "   cooked        same as brkint ignpar istrip icrnl ixon opost isig\n"
             "                 icanon, eof and eol characters to their default values\n"
             "   -cooked       same as raw\n"));
    printf(_("   crt           same as echoe echoctl echoke\n"));
    printf(_("   dec           same as echoe echoctl echoke -ixany intr ^c erase 0177 kill ^u\n"));
#ifdef IXANY
    printf(_(" * [-]decctlq    same as [-]ixany\n"));
#endif
    printf(_("   ek            erase and kill characters to their default values\n"
             "   evenp         same as parenb -parodd cs7\n"
             "   -evenp        same as -parenb cs8\n"));
#if defined XCASE && defined IUCLC && defined OLCUC
    printf(_(" * [-]lcase      same as xcase iuclc olcuc\n"));
#endif
    printf(_("   litout        same as -parenb -istrip -opost cs8\n"
             "   -litout       same as parenb istrip opost cs7\n"));
    printf(_("   nl            same as -icrnl -onlcr\n"
             "   -nl           same as icrnl -inlcr -igncr onlcr -ocrnl -onlret\n"));
    printf(_("   oddp          same as parenb parodd cs7\n"
             "   -oddp         same as -parenb cs8\n"
             "   [-]parity     same as [-]evenp\n"
             "   pass8         same as -parenb -istrip cs8\n"
             "   -pass8        same as parenb istrip cs7\n"));
    printf(_("   raw           same as -ignbrk -brkint -ignpar -parmrk -inpck -istrip\n"
             "                 -inlcr -igncr -icrnl -ixon -ixoff -icanon -opost\n"
             "                 -isig -iuclc -ixany -imaxbel -xcase min 1 time 0\n"
             "   -raw          same as cooked\n"));
    printf(_("   sane          same as cread -ignbrk brkint -inlcr -igncr icrnl\n"
             "                 icanon iexten echo echoe echok -echonl -noflsh\n"
             "                 -ixoff -iutf8 -iuclc -ixany imaxbel -xcase -olcuc -ocrnl\n"
             "                 opost -ofill onlcr -onocr -onlret nl0 cr0 tab0 bs0 vt0 ff0\n"
             "                 isig -tostop -ofdel -echoprt echoctl echoke -extproc -flusho\n"
             ));

    fputs(_(
          "\nHandle the tty line connected to standard input.  Without arguments,\n"
          "prints baud rate, line discipline, and deviations from stty sane.  In\n"
          "settings, CHAR is taken literally, or coded as in ^c, 0x37, 0177 or\n"
          "127; special values ^- or undef used to disable special characters.\n"), stdout);
    emit_ancillary_info(PROGRAM_NAME);

    exit(status);
}


/* Apply specified settings to MODE and REQUIRE_SET_ATTR as required.
   If CHECKING is true, this function doesn't interact
   with a device, and only validates specified settings.  */

static void apply_settings(bool checking, char const *device_name, char *const *settings, int n_settings, struct termios *mode, bool *require_set_attr) {
    for (int k = 1; k < n_settings; k++) {
        char const *arg = settings[k];
        if (!arg) continue;

        bool reversed = (arg[0] == '-');
        if (reversed) ++arg;
        
        if (STREQ(arg, "drain")) {
            tcsetattr_options = reversed ? TCSANOW : TCSADRAIN;
            continue;
        }

        bool match_found = false;
        bool not_set_attr = false;
        for (int i = 0; mode_info[i].name != NULL; ++i) {
            if (STREQ(arg, mode_info[i].name)) {
                if (!(mode_info[i].flags & NO_SETATTR)) {
                    match_found = set_mode(&mode_info[i], reversed, mode);
                    *require_set_attr = true;
                } else
                    match_found = not_set_attr = true;
                break;
            }
        }

        if (!match_found && reversed) {
            error(0, 0, _("invalid argument %s"), quote(arg - 1));
            usage(EXIT_FAILURE);
        }

        if (!match_found) {
            for (int i = 0; control_info[i].name != NULL; ++i) {
                if (STREQ(arg, control_info[i].name)) {
                    if (k == n_settings - 1 || !settings[k + 1]) {
                        error(0, 0, _("missing argument to %s"), quote(arg));
                        usage(EXIT_FAILURE);
                    }
                    match_found = true;
                    set_control_char(&control_info[i], settings[++k], mode);
                    *require_set_attr = true;
                    break;
                }
            }
        }

        if (!match_found || not_set_attr) {
            bool param_check = (k == n_settings - 1 || !settings[k + 1]);

            if (STREQ(arg, "ispeed") || STREQ(arg, "ospeed")) {
                if (param_check) {
                    error(0, 0, _("missing argument to %s"), quote(arg));
                    usage(EXIT_FAILURE);
                }
                if (string_to_baud(settings[++k]) == (speed_t)-1) {
                    error(0, 0, _("invalid %sspeed %s"), arg, quote(settings[k]));
                    usage(EXIT_FAILURE);
                }
                set_speed(STREQ(arg, "ispeed") ? input_speed : output_speed, settings[k], mode);
                if (!checking) *require_set_attr = true;
            }
#ifdef TIOCEXT
            else if (STREQ(arg, "extproc")) {
                if (!checking && ioctl(STDIN_FILENO, TIOCEXT, &(!reversed)) != 0) {
                    error(EXIT_FAILURE, errno, _("%s: error setting %s"), quotef_n(0, device_name), quote_n(1, arg));
                }
            }
#endif
#ifdef TIOCGWINSZ
            else if (STREQ(arg, "rows") || STREQ(arg, "cols") || STREQ(arg, "columns")) {
                if (param_check) {
                    error(0, 0, _("missing argument to %s"), quote(arg));
                    usage(EXIT_FAILURE);
                }
                if (!checking) {
                    int value = integer_arg(settings[++k], INT_MAX);
                    set_window_size(STREQ(arg, "rows") ? value : -1, STREQ(arg, "rows") ? -1 : value, device_name);
                }
            } else if (STREQ(arg, "size")) {
                if (!checking) {
                    max_col = screen_columns();
                    current_col = 0;
                    display_window_size(false, device_name);
                }
            }
#endif
#ifdef HAVE_C_LINE
            else if (STREQ(arg, "line")) {
                if (param_check) {
                    error(0, 0, _("missing argument to %s"), quote(arg));
                    usage(EXIT_FAILURE);
                }
                uintmax_t value = integer_arg(settings[++k], UINTMAX_MAX);
                if (ckd_add(&mode->c_line, value, 0)) {
                    error(0, EOVERFLOW, _("invalid line discipline %s"), quote(settings[k]));
                }
                *require_set_attr = true;
            }
#endif
            else if (STREQ(arg, "speed")) {
                if (!checking) {
                    max_col = screen_columns();
                    display_speed(mode, false);
                }
            } else if (string_to_baud(arg) != (speed_t)-1) {
                set_speed(both_speeds, arg, mode);
                if (!checking) *require_set_attr = true;
            } else {
                if (!recover_mode(arg, mode)) {
                    error(0, 0, _("invalid argument %s"), quote(arg));
                    usage(EXIT_FAILURE);
                }
                *require_set_attr = true;
            }
        }
    }

    if (checking) check_speed(mode);
}

#include <stdbool.h>
#include <locale.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <stdio.h>
#include <getopt.h>
#include <string.h>

#define PROGRAM_NAME "program"
#define AUTHORS "authors"

static struct option longopts[] = {  /* Add specific longopts structure */ };

int main(int argc, char *argv[]) {
    struct termios mode = {0};
    struct termios new_mode = {0};
    char *file_name = NULL;
    const char *device_name;
    enum { CHANGED, ALL, RECOVERABLE } output_type = CHANGED;
    bool verbose_output = false;
    bool recoverable_output = false;
    bool require_set_attr = false;
    bool noargs = true;
    int optc, argi = 0, opti = 1;

    initialize_main(&argc, &argv);
    set_program_name(argv[0]);
    setlocale(LC_ALL, "");
    bindtextdomain(PACKAGE, LOCALEDIR);
    textdomain(PACKAGE);
    atexit(close_stdout);

    opterr = 0;

    while ((optc = getopt_long(argc - argi, argv + argi, "-agF:", longopts, NULL)) != -1) {
        switch (optc) {
        case 'a':
            verbose_output = true;
            output_type = ALL;
            break;
        case 'g':
            recoverable_output = true;
            output_type = RECOVERABLE;
            break;
        case 'F':
            if (file_name) {
                error(EXIT_FAILURE, 0, "only one device may be specified");
            }
            file_name = optarg;
            break;
        case_GETOPT_HELP_CHAR;
        case_GETOPT_VERSION_CHAR(PROGRAM_NAME, AUTHORS);
        default:
            if (!STREQ(argv[argi + opti], "-drain") && !STREQ(argv[argi + opti], "drain")) {
                noargs = false;
            }
            argi += opti;
            opti = 1;
            optind = 0;
            break;
        }

        while (opti < optind) {
            argv[argi + opti++] = NULL;
        }
    }

    if (verbose_output && recoverable_output) {
        error(EXIT_FAILURE, 0, "the options for verbose and stty-readable output styles are mutually exclusive");
    }

    if (!noargs && (verbose_output || recoverable_output)) {
        error(EXIT_FAILURE, 0, "when specifying an output style, modes may not be set");
    }

    device_name = file_name ? file_name : "standard input";

    if (!noargs && !verbose_output && !recoverable_output) {
        apply_settings(true, device_name, argv, argc, &new_mode, &require_set_attr);
    }

    if (file_name) {
        int fdflags;
        if (fd_reopen(STDIN_FILENO, device_name, O_RDONLY | O_NONBLOCK, 0) < 0) {
            error(EXIT_FAILURE, errno, "%s", device_name);
        }
        if ((fdflags = fcntl(STDIN_FILENO, F_GETFL)) == -1 || fcntl(STDIN_FILENO, F_SETFL, fdflags & ~O_NONBLOCK) < 0) {
            error(EXIT_FAILURE, errno, "%s: couldn't reset non-blocking mode", device_name);
        }
    }

    if (tcgetattr(STDIN_FILENO, &mode)) {
        error(EXIT_FAILURE, errno, "%s", device_name);
    }

    if (verbose_output || recoverable_output || noargs) {
        display_settings(output_type, &mode, device_name);
        return EXIT_SUCCESS;
    }

    require_set_attr = false;
    apply_settings(false, device_name, argv, argc, &mode, &require_set_attr);

    if (require_set_attr) {
        if (tcsetattr(STDIN_FILENO, TCSANOW, &mode)) {
            error(EXIT_FAILURE, errno, "%s", device_name);
        }
        if (tcgetattr(STDIN_FILENO, &new_mode)) {
            error(EXIT_FAILURE, errno, "%s", device_name);
        }
        if (!eq_mode(&mode, &new_mode)) {
            error(EXIT_FAILURE, 0, "%s: unable to perform all requested operations", device_name);
        }
    }

    return EXIT_SUCCESS;
}

/* Return true if modes are equivalent.  */

#include <stdbool.h>
#include <string.h>
#include <termios.h>

static bool eq_mode(const struct termios *mode1, const struct termios *mode2) {
    if (mode1 == NULL || mode2 == NULL) {
        return false;
    }

    if (mode1->c_iflag != mode2->c_iflag || mode1->c_oflag != mode2->c_oflag ||
        mode1->c_cflag != mode2->c_cflag || mode1->c_lflag != mode2->c_lflag) {
        return false;
    }

#ifdef HAVE_C_LINE
    if (mode1->c_line != mode2->c_line) {
        return false;
    }
#endif

    if (memcmp(mode1->c_cc, mode2->c_cc, sizeof(mode1->c_cc)) != 0) {
        return false;
    }

    return cfgetispeed(mode1) == cfgetispeed(mode2) && cfgetospeed(mode1) == cfgetospeed(mode2);
}

/* Return false if not applied because not reversible; otherwise
   return true.  */

#include <stdbool.h>
#include <termios.h>
#include <string.h>

#define STREQ(a, b) (strcmp((a), (b)) == 0)
#define CERASE 0177
#define CKILL 025
#define CEOF 004
#define CEOL 0

struct mode_info {
    const char *name;
    int flags;
    int type;
    tcflag_t mask;
    tcflag_t bits;
};

tcflag_t* mode_type_flag(int type, struct termios *mode);
void sane_mode(struct termios *mode);

static bool apply_combination_mode(const struct mode_info *info, struct termios *mode, bool reversed) {
    if (STREQ(info->name, "evenp") || STREQ(info->name, "parity")) {
        if (reversed) {
            mode->c_cflag = (mode->c_cflag & ~PARENB & ~CSIZE) | CS8;
        } else {
            mode->c_cflag = (mode->c_cflag & ~PARODD & ~CSIZE) | PARENB | CS7;
        }
    } else if (STREQ(info->name, "oddp")) {
        if (reversed) {
            mode->c_cflag = (mode->c_cflag & ~PARENB & ~CSIZE) | CS8;
        } else {
            mode->c_cflag = (mode->c_cflag & ~CSIZE) | CS7 | PARODD | PARENB;
        }
    } else if (STREQ(info->name, "nl")) {
        if (reversed) {
            mode->c_iflag = (mode->c_iflag | ICRNL) & ~INLCR & ~IGNCR;
            mode->c_oflag = (mode->c_oflag | ONLCR) & ~OCRNL & ~ONLRET;
        } else {
            mode->c_iflag &= ~ICRNL;
            mode->c_oflag &= ~ONLCR;
        }
    } else if (STREQ(info->name, "ek")) {
        mode->c_cc[VERASE] = CERASE;
        mode->c_cc[VKILL] = CKILL;
    } else if (STREQ(info->name, "sane")) {
        sane_mode(mode);
    } else if (STREQ(info->name, "cbreak")) {
        if (reversed) mode->c_lflag |= ICANON;
        else mode->c_lflag &= ~ICANON;
    } else if (STREQ(info->name, "pass8")) {
        if (reversed) {
            mode->c_cflag = (mode->c_cflag & ~CSIZE) | CS7 | PARENB;
            mode->c_iflag |= ISTRIP;
        } else {
            mode->c_cflag = (mode->c_cflag & ~PARENB & ~CSIZE) | CS8;
            mode->c_iflag &= ~ISTRIP;
        }
    } else if (STREQ(info->name, "litout")) {
        if (reversed) {
            mode->c_cflag = (mode->c_cflag & ~CSIZE) | CS7 | PARENB;
            mode->c_iflag |= ISTRIP;
            mode->c_oflag |= OPOST;
        } else {
            mode->c_cflag = (mode->c_cflag & ~PARENB & ~CSIZE) | CS8;
            mode->c_iflag &= ~ISTRIP;
            mode->c_oflag &= ~OPOST;
        }
    } else if (STREQ(info->name, "raw") || STREQ(info->name, "cooked")) {
        if ((info->name[0] == 'r' && reversed) || (info->name[0] == 'c' && !reversed)) {
            mode->c_iflag |= BRKINT | IGNPAR | ISTRIP | ICRNL | IXON;
            mode->c_oflag |= OPOST;
            mode->c_lflag |= ISIG | ICANON;
            mode->c_cc[VEOF] = CEOF;
            mode->c_cc[VEOL] = CEOL;
        } else {
            mode->c_iflag = 0;
            mode->c_oflag &= ~OPOST;
            mode->c_lflag &= ~(ISIG | ICANON);
            mode->c_cc[VMIN] = 1;
            mode->c_cc[VTIME] = 0;
        }
    } else if (STREQ(info->name, "decctlq")) {
#ifdef IXANY
        if (reversed) mode->c_iflag |= IXANY;
        else mode->c_iflag &= ~IXANY;
#endif
    } else if (STREQ(info->name, "tabs")) {
#ifdef TABDLY
        if (reversed) mode->c_oflag = (mode->c_oflag & ~TABDLY) | TAB3;
        else mode->c_oflag = (mode->c_oflag & ~TABDLY) | TAB0;
#else
# ifdef OXTABS
        if (reversed) mode->c_oflag |= OXTABS;
        else mode->c_oflag &= ~OXTABS;
# endif
#endif
    } else if (STREQ(info->name, "lcase") || STREQ(info->name, "LCASE")) {
#ifdef XCASE
#ifdef IUCLC
#ifdef OLCUC
        if (reversed) {
            mode->c_lflag &= ~XCASE;
            mode->c_iflag &= ~IUCLC;
            mode->c_oflag &= ~OLCUC;
        } else {
            mode->c_lflag |= XCASE;
            mode->c_iflag |= IUCLC;
            mode->c_oflag |= OLCUC;
        }
#endif
#endif
#endif
    } else if (STREQ(info->name, "crt")) {
        mode->c_lflag |= ECHOE | ECHOCTL | ECHOKE;
    } else if (STREQ(info->name, "dec")) {
        mode->c_cc[VINTR] = 3;
        mode->c_cc[VERASE] = 127;
        mode->c_cc[VKILL] = 21;
        mode->c_lflag |= ECHOE | ECHOCTL | ECHOKE;
#ifdef IXANY
        mode->c_iflag &= ~IXANY;
#endif
    } else {
        return false;
    }
    return true;
}

static bool set_mode(const struct mode_info *info, bool reversed, struct termios *mode) {
    if (reversed && (info->flags & REV) == 0) return false;

    tcflag_t *bitsp = mode_type_flag(info->type, mode);

    if (bitsp == NULL) {
        return apply_combination_mode(info, mode, reversed);
    } else {
        if (reversed) {
            *bitsp &= ~info->mask & ~info->bits;
        } else {
            *bitsp = (*bitsp & ~info->mask) | info->bits;
        }
    }

    return true;
}

static void set_control_char(const struct control_info *info, const char *arg, struct termios *mode) {
    unsigned long int value = _POSIX_VDISABLE;

    if (STREQ(info->name, "min") || STREQ(info->name, "time")) {
        value = integer_arg(arg, TYPE_MAXIMUM(cc_t));
    } else if (arg[0] && arg[1] == '\0') {
        value = to_uchar(arg[0]);
    } else if (!STREQ(arg, "^-") && !STREQ(arg, "undef")) {
        if (arg[0] == '^' && arg[1] != '\0') {
            value = (arg[1] == '?') ? 127 : to_uchar(arg[1]) & ~0140;
        } else {
            value = integer_arg(arg, TYPE_MAXIMUM(cc_t));
        }
    }
    mode->c_cc[info->offset] = value;
}

#include <stdio.h>
#include <stdlib.h>
#include <termios.h>

static void set_speed(enum speed_setting type, const char *arg, struct termios *mode) {
    speed_t baud = string_to_baud(arg);
    
    if (baud == (speed_t)-1) {
        fprintf(stderr, "Invalid baud rate: %s\n", arg);
        exit(EXIT_FAILURE);
    }
    
    int result = 0;

    if (type == input_speed || type == both_speeds) {
        last_ibaud = baud;
        result = cfsetispeed(mode, baud);
    }
    
    if (type == output_speed || type == both_speeds) {
        last_obaud = baud;
        result |= cfsetospeed(mode, baud);
    }
    
    if (result != 0) {
        fprintf(stderr, "Unsupported speed: %s\n", arg);
        exit(EXIT_FAILURE);
    }
}

#ifdef TIOCGWINSZ

static int
get_win_size (int fd, struct winsize *win)
{
  int err = ioctl (fd, TIOCGWINSZ, (char *) win);
  return err;
}

static void
set_window_size (int rows, int cols, char const *device_name)
{
  struct winsize win;

  if (get_win_size (STDIN_FILENO, &win))
    {
      if (errno != EINVAL)
        error (EXIT_FAILURE, errno, "%s", quotef (device_name));
      memset (&win, 0, sizeof (win));
    }

  if (rows >= 0)
    win.ws_row = rows;
  if (cols >= 0)
    win.ws_col = cols;

# ifdef TIOCSSIZE
  /* Alexander Dupuy <dupuy@cs.columbia.edu> wrote:
     The following code deals with a bug in the SunOS 4.x (and 3.x?) kernel.
     This comment from sys/ttold.h describes Sun's twisted logic - a better
     test would have been (ts_lines > 64k || ts_cols > 64k || ts_cols == 0).
     At any rate, the problem is gone in Solaris 2.x.

     Unfortunately, the old TIOCSSIZE code does collide with TIOCSWINSZ,
     but they can be disambiguated by checking whether a "struct ttysize"
     structure's "ts_lines" field is greater than 64K or not.  If so,
     it's almost certainly a "struct winsize" instead.

     At any rate, the bug manifests itself when ws_row == 0; the symptom is
     that ws_row is set to ws_col, and ws_col is set to (ws_xpixel<<16)
     + ws_ypixel.  Since GNU stty sets rows and columns separately, this bug
     caused "stty rows 0 cols 0" to set rows to cols and cols to 0, while
     "stty cols 0 rows 0" would do the right thing.  On a little-endian
     machine like the sun386i, the problem is the same, but for ws_col == 0.

     The workaround is to do the ioctl once with row and col = 1 to set the
     pixel info, and then do it again using a TIOCSSIZE to set rows/cols.  */

  if (win.ws_row == 0 || win.ws_col == 0)
    {
      struct ttysize ttysz;

      ttysz.ts_lines = win.ws_row;
      ttysz.ts_cols = win.ws_col;

      win.ws_row = 1;
      win.ws_col = 1;

      if (ioctl (STDIN_FILENO, TIOCSWINSZ, (char *) &win))
        error (EXIT_FAILURE, errno, "%s", quotef (device_name));

      if (ioctl (STDIN_FILENO, TIOCSSIZE, (char *) &ttysz))
        error (EXIT_FAILURE, errno, "%s", quotef (device_name));
      return;
    }
# endif

  if (ioctl (STDIN_FILENO, TIOCSWINSZ, (char *) &win))
    error (EXIT_FAILURE, errno, "%s", quotef (device_name));
}

static void
display_window_size (bool fancy, char const *device_name)
{
  struct winsize win;

  if (get_win_size (STDIN_FILENO, &win))
    {
      if (errno != EINVAL)
        error (EXIT_FAILURE, errno, "%s", quotef (device_name));
      if (!fancy)
        error (EXIT_FAILURE, 0,
               _("%s: no size information for this device"),
               quotef (device_name));
    }
  else
    {
      wrapf (fancy ? "rows %d; columns %d;" : "%d %d\n",
             win.ws_row, win.ws_col);
      if (!fancy)
        current_col = 0;
    }
}
#endif

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <string.h>

static int screen_columns(void) {
    struct winsize win;

#ifdef TIOCGWINSZ
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &win) == 0 && win.ws_col > 0) {
        return win.ws_col;
    }
#endif

    char *col_string = getenv("COLUMNS");
    if (col_string != NULL) {
        char *endptr;
        errno = 0;
        long n_columns = strtol(col_string, &endptr, 10);
        if (endptr != col_string && *endptr == '\0' && errno == 0 &&
            n_columns > 0 && n_columns <= INT_MAX) {
            return (int)n_columns;
        }
    }

    return 80;
}

ATTRIBUTE_PURE
static tcflag_t *
mode_type_flag (enum mode_type type, struct termios *mode)
{
  switch (type)
    {
    case control:
      return &mode->c_cflag;

    case input:
      return &mode->c_iflag;

    case output:
      return &mode->c_oflag;

    case local:
      return &mode->c_lflag;

    case combination:
      return nullptr;

    default:
      unreachable ();
    }
}

static void display_settings(enum output_type output_type, struct termios *mode, const char *device_name) {
    if (!mode || !device_name) {
        fprintf(stderr, "Invalid arguments\n");
        return;
    }

    switch (output_type) {
        case changed:
            display_changed(mode);
            break;
        case all:
            display_all(mode, device_name);
            break;
        case recoverable:
            display_recoverable(mode);
            break;
        default:
            fprintf(stderr, "Unknown output type\n");
            break;
    }
}

static void display_changed(struct termios *mode) {
    int i;
    bool empty_line = true;
    tcflag_t *bitsp;
    unsigned long mask;
    enum mode_type prev_type = control;

    display_speed(mode, true);
#ifdef HAVE_C_LINE
    wrapf("line = %d;", mode->c_line);
#endif
    putchar('\n');
    current_col = 0;

    for (i = 0; !STREQ(control_info[i].name, "min"); ++i) {
        if (mode->c_cc[control_info[i].offset] == control_info[i].saneval)
            continue;

#ifdef VFLUSHO
        if (STREQ(control_info[i].name, "flush"))
            continue;
#endif
#if VSWTCH == VSUSP
        if (STREQ(control_info[i].name, "swtch"))
            continue;
#endif
#if VEOF == VMIN
        if ((mode->c_lflag & ICANON) == 0 && 
            (STREQ(control_info[i].name, "eof") || STREQ(control_info[i].name, "eol")))
            continue;
#endif

        empty_line = false;
        wrapf("%s = %s;", control_info[i].name, visible(mode->c_cc[control_info[i].offset]));
    }

    if ((mode->c_lflag & ICANON) == 0) {
        wrapf("min = %lu; time = %lu;\n",
              (unsigned long int)mode->c_cc[VMIN],
              (unsigned long int)mode->c_cc[VTIME]);
    } else if (!empty_line) {
        putchar('\n');
    }
    current_col = 0;

    empty_line = true;
    for (i = 0; mode_info[i].name != nullptr; ++i) {
        if (mode_info[i].flags & OMIT)
            continue;
        
        if (mode_info[i].type != prev_type) {
            if (!empty_line) {
                putchar('\n');
                current_col = 0;
                empty_line = true;
            }
            prev_type = mode_info[i].type;
        }

        bitsp = mode_type_flag(mode_info[i].type, mode);
        mask = mode_info[i].mask ? mode_info[i].mask : mode_info[i].bits;
        
        if ((*bitsp & mask) == mode_info[i].bits) {
            if (mode_info[i].flags & SANE_UNSET) {
                wrapf("%s", mode_info[i].name);
                empty_line = false;
            }
        } else if ((mode_info[i].flags & (SANE_SET | REV)) == (SANE_SET | REV)) {
            wrapf("-%s", mode_info[i].name);
            empty_line = false;
        }
    }

    if (!empty_line)
        putchar('\n');
    current_col = 0;
}

#include <stdbool.h>
#include <termios.h>
#include <stdio.h>

static void display_all(struct termios *mode, char const *device_name) {
  int i;
  tcflag_t *bitsp;
  unsigned long mask;
  enum mode_type prev_type = control;

  display_speed(mode, true);
#ifdef TIOCGWINSZ
  display_window_size(true, device_name);
#endif
#ifdef HAVE_C_LINE
  wrapf("line = %d;", mode->c_line);
#endif
  putchar('\n');
  current_col = 0;

  for (i = 0; !STREQ(control_info[i].name, "min"); ++i) {
#ifdef VFLUSHO
    if (STREQ(control_info[i].name, "flush"))
      continue;
#endif
#if VSWTCH == VSUSP
    if (STREQ(control_info[i].name, "swtch"))
      continue;
#endif
#if VEOF == VMIN
    if ((mode->c_lflag & ICANON) == 0 &&
        (STREQ(control_info[i].name, "eof") || STREQ(control_info[i].name, "eol")))
      continue;
#endif
    wrapf("%s = %s;", control_info[i].name, visible(mode->c_cc[control_info[i].offset]));
  }
#if VEOF == VMIN
  if ((mode->c_lflag & ICANON) == 0)
#endif
    wrapf("min = %lu; time = %lu;", (unsigned long) mode->c_cc[VMIN], (unsigned long) mode->c_cc[VTIME]);
  
  if (current_col != 0)
    putchar('\n');
  
  current_col = 0;

  for (i = 0; mode_info[i].name != NULL; ++i) {
    if (mode_info[i].flags & OMIT)
      continue;
    if (mode_info[i].type != prev_type) {
      putchar('\n');
      current_col = 0;
      prev_type = mode_info[i].type;
    }

    bitsp = mode_type_flag(mode_info[i].type, mode);
    mask = mode_info[i].mask ? mode_info[i].mask : mode_info[i].bits;
    if ((*bitsp & mask) == mode_info[i].bits)
      wrapf("%s", mode_info[i].name);
    else if (mode_info[i].flags & REV)
      wrapf("-%s", mode_info[i].name);
  }
  putchar('\n');
  current_col = 0;
}

/* Verify requested asymmetric speeds are supported.
   Note we don't flag the case where only ispeed or
   ospeed is set, when that would set both.  */

static void check_speed(struct termios *mode) {
    if (last_ibaud == -1 || last_obaud == -1) {
        return;
    }

    if (cfgetispeed(mode) != last_ibaud || cfgetospeed(mode) != last_obaud) {
        error(EXIT_FAILURE, 0,
              _("asymmetric input (%lu), output (%lu) speeds not supported"),
              baud_to_value(last_ibaud), baud_to_value(last_obaud));
    }
}

#include <stdio.h>
#include <termios.h>
#include <stdbool.h>

static void display_speed(struct termios *mode, bool fancy) {
    unsigned long ispeed = baud_to_value(cfgetispeed(mode));
    unsigned long ospeed = baud_to_value(cfgetospeed(mode));

    if (ispeed == 0 || ispeed == ospeed) {
        wrapf(fancy ? "speed %lu baud;" : "%lu\n", ospeed);
    } else {
        wrapf(fancy ? "ispeed %lu baud; ospeed %lu baud;" : "%lu %lu\n", ispeed, ospeed);
    }
    
    if (!fancy) {
        current_col = 0;
    }
}

#include <stdio.h>
#include <errno.h>
#include <termios.h>

static void display_recoverable(const struct termios *mode) {
    if (!mode) {
        fprintf(stderr, "Invalid mode provided\n");
        return;
    }
    
    if (printf("%lx:%lx:%lx:%lx",
               (unsigned long)mode->c_iflag, 
               (unsigned long)mode->c_oflag, 
               (unsigned long)mode->c_cflag, 
               (unsigned long)mode->c_lflag) < 0) {
        perror("Failed to display flags");
        return;
    }

    for (size_t i = 0; i < NCCS; ++i) {
        if (printf(":%lx", (unsigned long)mode->c_cc[i]) < 0) {
            perror("Failed to display control character");
            return;
        }
    }

    if (putchar('\n') == EOF) {
        perror("Failed to write newline");
    }
}

/* NOTE: identical to below, modulo use of tcflag_t */
#include <stdlib.h>
#include <errno.h>
#include <limits.h>

static int convert_to_tcflag_t(const char *s, int base, char **endptr, tcflag_t *result, char delim) {
    errno = 0;
    unsigned long ul = strtoul(s, endptr, base);
    
    if (errno != 0 || *endptr == s || **endptr != delim || ul > (unsigned long)TCF_MAX) {
        return -1;
    }
    
    *result = (tcflag_t)ul;
    return 0;
}

/* NOTE: identical to above, modulo use of cc_t */
#include <errno.h>
#include <limits.h>
#include <stdlib.h>

static int strtoul_cc_t(const char *s, int base, char **p, cc_t *result, char delim) {
    errno = 0;
    unsigned long ul = strtoul(s, p, base);

    if (errno != 0 || *p == s || **p != delim || ul > UCHAR_MAX) {
        return -1;
    }

    *result = (cc_t)ul;
    return 0;
}

/* Parse the output of display_recoverable.
   Return false if any part of it is invalid.  */
#include <stdbool.h>
#include <stddef.h>
#include <errno.h>
#include <termios.h>
#include <stdlib.h>

static bool parse_tcflag(const char **s, tcflag_t *flag) {
    char *p;
    errno = 0;
    unsigned long value = strtoul(*s, &p, 16);
    if (errno != 0 || *p != ':') {
        return false;
    }
    *flag = (tcflag_t)value;
    *s = p + 1;
    return true;
}

static bool parse_cc(const char **s, cc_t *cc, char delim) {
    char *p;
    errno = 0;
    unsigned long value = strtoul(*s, &p, 16);
    if (errno != 0 || (*p != delim && *p != '\0')) {
        return false;
    }
    *cc = (cc_t)value;
    *s = p + 1;
    return true;
}

static bool recover_mode(const char *arg, struct termios *mode) {
    const char *s = arg;
    if (!parse_tcflag(&s, &mode->c_iflag) ||
        !parse_tcflag(&s, &mode->c_oflag) ||
        !parse_tcflag(&s, &mode->c_cflag) ||
        !parse_tcflag(&s, &mode->c_lflag)) {
        return false;
    }

    for (size_t i = 0; i < NCCS; ++i) {
        char delim = i < NCCS - 1 ? ':' : '\0';
        if (!parse_cc(&s, &mode->c_cc[i], delim)) {
            return false;
        }
    }

    return true;
}

/* Autogenerated conversion functions to/from speed_t */
#include "speedlist.h"

ATTRIBUTE_PURE
static speed_t
string_to_baud (char const *arg)
{
  char *ep;
  unsigned long value;
  unsigned char c;

  /* Explicitly disallow negative numbers.  */
  while (c_isspace (*arg))
    arg++;
  if (*arg == '-')
    return (speed_t) -1;

  value = strtoul (arg, &ep, 10);

  c = *ep++;
  if (c == '.')
    {
      /* Number includes a fraction. Round it to nearest-even.
         Note in particular that 134.5 must round to 134! */
      c = *ep++;
      if (c)
        {
          unsigned char d = c - '0';
          if (d > 5)
            value++;
          else if (d == 5)
            {
              while ((c = *ep++) == '0'); /* Skip zeroes after .5 */

              if (c)
                value++;                /* Nonzero, round up */
              else
                value += (value & 1);   /* Exactly in the middle, round even */
            }

          while (c_isdigit (c)) /* Skip remaining digits.  */
            c = *ep++;

          if (c)
            return (speed_t) -1; /* Garbage after otherwise valid number */
        }
    }
  else if (c)
    {
      /* Not a valid number; check for legacy aliases "exta" and "extb" */
      if (STREQ (arg, "exta"))
        return B19200;
      else if (STREQ (arg, "extb"))
        return B38400;
      else
        return (speed_t) -1;
    }

  return value_to_baud (value);
}

static void
sane_mode (struct termios *mode)
{
  for (int i = 0; control_info[i].name; ++i)
    {
#if VMIN == VEOF
      if (strcmp(control_info[i].name, "min") == 0)
        break;
#endif
      mode->c_cc[control_info[i].offset] = control_info[i].saneval;
    }

  for (int i = 0; mode_info[i].name; ++i)
    {
      if (mode_info[i].flags & NO_SETATTR)
        continue;

      tcflag_t *bitsp = mode_type_flag(mode_info[i].type, mode);
      if (!bitsp)
        continue;

      if (mode_info[i].flags & SANE_SET)
        *bitsp = (*bitsp & ~mode_info[i].mask) | mode_info[i].bits;
      else if (mode_info[i].flags & SANE_UNSET)
        *bitsp &= ~mode_info[i].mask & ~mode_info[i].bits;
    }
}

/* Return a string that is the printable representation of character CH.  */
/* Adapted from 'cat' by Torbjörn Granlund.  */

#include <string.h>

static const char *visible(cc_t ch) {
    static char buf[10];
    char *bpout = buf;

    if (ch == _POSIX_VDISABLE) {
        return "<undef>";
    }

    if (ch >= 32 && ch < 127) {
        *bpout++ = ch;
    } else if (ch == 127) {
        strcpy(buf, "^?");
    } else if (ch >= 128) {
        strcpy(buf, "M-");
        bpout += 2;
        ch -= 128;
        if (ch >= 32 && ch < 127) {
            *bpout++ = ch;
        } else {
            strcpy(bpout, "^?");
        }
    } else {
        strcpy(buf, "^");
        buf[1] = ch + 64;
    }

    *bpout = '\0';
    return buf;
}

/* Parse string S as an integer, using decimal radix by default,
   but allowing octal and hex numbers as in C.  Reject values
   larger than MAXVAL.  */

static uintmax_t integer_arg(const char *s, uintmax_t maxval) {
    return xnumtoumax(s, NULL, 0, maxval, "bB", _("invalid integer argument"), NULL, NULL);
}
