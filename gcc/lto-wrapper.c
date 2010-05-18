/* Wrapper to call lto.  Used by collect2 and the linker plugin.
   Copyright (C) 2009, 2010 Free Software Foundation, Inc.

   Factored out of collect2 by Rafael Espindola <espindola@google.com>

This file is part of GCC.

GCC is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free
Software Foundation; either version 3, or (at your option) any later
version.

GCC is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or
FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
for more details.

You should have received a copy of the GNU General Public License
along with GCC; see the file COPYING3.  If not see
<http://www.gnu.org/licenses/>.  */


/* This program is passed a gcc, a list of gcc arguments and a list of
   object files containing IL. It scans the argument list to check if
   we are in whopr mode or not modifies the arguments and needed and
   prints a list of output files on stdout.

   Example:

   $ lto-wrapper gcc/xgcc -B gcc a.o b.o -o test -flto

   The above will print something like
   /tmp/ccwbQ8B2.lto.o

   If -fwhopr is used instead, more than one file might be produced
   ./ccXj2DTk.lto.ltrans.o
   ./ccCJuXGv.lto.ltrans.o
*/

#include "config.h"
#include "system.h"
#include "coretypes.h"
#include "tm.h"
#include "intl.h"
#include "libiberty.h"
#include "obstack.h"

int debug;				/* true if -save-temps.  */
int verbose;				/* true if -v.  */

enum lto_mode_d {
  LTO_MODE_NONE,			/* Not doing LTO.  */
  LTO_MODE_LTO,				/* Normal LTO.  */
  LTO_MODE_WHOPR			/* WHOPR.  */
};

/* Current LTO mode.  */
static enum lto_mode_d lto_mode = LTO_MODE_NONE;

static char *ltrans_output_file;
static char *flto_out;
static char *args_name;

static void maybe_unlink_file (const char *);

/* Delete tempfiles and exit function.  */

static void
lto_wrapper_exit (int status)
{
  static bool cleanup_done = false;
  if (!cleanup_done)
    {
      /* Setting cleanup_done prevents an infinite loop if one of the
         calls to maybe_unlink_file fails. */
      cleanup_done = true;

      if (ltrans_output_file)
        maybe_unlink_file (ltrans_output_file);
      if (flto_out)
        maybe_unlink_file (flto_out);
      if (args_name)
        maybe_unlink_file (args_name);
    }
  exit (status);
}

/* Just die. CMSGID is the error message. */

static void __attribute__ ((format (printf, 1, 2)))
fatal (const char * cmsgid, ...)
{
  va_list ap;

  va_start (ap, cmsgid);
  fprintf (stderr, "lto-wrapper: ");
  vfprintf (stderr, _(cmsgid), ap);
  fprintf (stderr, "\n");
  va_end (ap);

  lto_wrapper_exit (FATAL_EXIT_CODE);
}


/* Die when sys call fails. CMSGID is the error message.  */

static void __attribute__ ((format (printf, 1, 2)))
fatal_perror (const char *cmsgid, ...)
{
  int e = errno;
  va_list ap;

  va_start (ap, cmsgid);
  fprintf (stderr, "lto-wrapper: ");
  vfprintf (stderr, _(cmsgid), ap);
  fprintf (stderr, ": %s\n", xstrerror (e));
  va_end (ap);

  lto_wrapper_exit (FATAL_EXIT_CODE);
}


/* Execute a program, and wait for the reply. ARGV are the arguments. The
   last one must be NULL. */

static struct pex_obj *
collect_execute (char **argv)
{
  struct pex_obj *pex;
  const char *errmsg;
  int err;

  if (verbose)
    {
      char **p_argv;
      const char *str;

      for (p_argv = argv; (str = *p_argv) != (char *) 0; p_argv++)
	fprintf (stderr, " %s", str);

      fprintf (stderr, "\n");
    }

  fflush (stdout);
  fflush (stderr);

  pex = pex_init (0, "lto-wrapper", NULL);
  if (pex == NULL)
    fatal_perror ("pex_init failed");

  errmsg = pex_run (pex, PEX_LAST | PEX_SEARCH, argv[0], argv, NULL,
		    NULL, &err);
  if (errmsg != NULL)
    {
      if (err != 0)
	{
	  errno = err;
	  fatal_perror (errmsg);
	}
      else
	fatal (errmsg);
    }

  return pex;
}


/* Wait for a process to finish, and exit if a nonzero status is found.
   PROG is the program name. PEX is the process we should wait for. */

static int
collect_wait (const char *prog, struct pex_obj *pex)
{
  int status;

  if (!pex_get_status (pex, 1, &status))
    fatal_perror ("can't get program status");
  pex_free (pex);

  if (status)
    {
      if (WIFSIGNALED (status))
	{
	  int sig = WTERMSIG (status);
	  if (WCOREDUMP (status))
	    fatal ("%s terminated with signal %d [%s], core dumped",
		   prog, sig, strsignal (sig));
	  else
	    fatal ("%s terminated with signal %d [%s]",
		   prog, sig, strsignal (sig));
	}

      if (WIFEXITED (status))
	fatal ("%s returned %d exit status", prog, WEXITSTATUS (status));
    }

  return 0;
}


/* Unlink a temporary LTRANS file unless requested otherwise.  */

static void
maybe_unlink_file (const char *file)
{
  if (! debug)
    {
      if (unlink_if_ordinary (file))
	fatal_perror ("deleting LTRANS file %s", file);
    }
  else
    fprintf (stderr, "[Leaving LTRANS %s]\n", file);
}


/* Execute program ARGV[0] with arguments ARGV. Wait for it to finish.  */

static void
fork_execute (char **argv)
{
  struct pex_obj *pex;
  char *new_argv[3];
  char *at_args;
  FILE *args;
  int status;

  args_name = make_temp_file (".args");
  at_args = concat ("@", args_name, NULL);
  args = fopen (args_name, "w");
  if (args == NULL)
    fatal ("failed to open %s", args_name);

  status = writeargv (&argv[1], args);

  if (status)
    fatal ("could not write to temporary file %s",  args_name);

  fclose (args);

  new_argv[0] = argv[0];
  new_argv[1] = at_args;
  new_argv[2] = NULL;

  pex = collect_execute (new_argv);
  collect_wait (new_argv[0], pex);

  maybe_unlink_file (args_name);
  free (at_args);
}

/* Template of LTRANS dumpbase suffix.  */
#define DUMPBASE_SUFFIX ".ltrans18446744073709551615"

/* Execute gcc. ARGC is the number of arguments. ARGV contains the arguments. */

static void
run_gcc (unsigned argc, char *argv[])
{
  unsigned i, j;
  const char **new_argv;
  const char **argv_ptr;
  char *list_option_full = NULL;
  const char *linker_output = NULL;
  const char *collect_gcc_options, *collect_gcc;
  struct obstack env_obstack;
  bool seen_o = false;

  /* Get the driver and options.  */
  collect_gcc = getenv ("COLLECT_GCC");
  if (!collect_gcc)
    fatal ("environment variable COLLECT_GCC must be set");

  /* Set the CFLAGS environment variable.  */
  collect_gcc_options = getenv ("COLLECT_GCC_OPTIONS");
  if (!collect_gcc_options)
    fatal ("environment variable COLLECT_GCC_OPTIONS must be set");

  /* Count arguments.  */
  i = 0;
  for (j = 0; collect_gcc_options[j] != '\0'; ++j)
    if (collect_gcc_options[j] == '\'')
      ++i;

  if (i % 2 != 0)
    fatal ("malformed COLLECT_GCC_OPTIONS");

  /* Initalize the common arguments for the driver.  */
  new_argv = (const char **) xmalloc ((15 + i / 2 + argc) * sizeof (char *));
  argv_ptr = new_argv;
  *argv_ptr++ = collect_gcc;
  *argv_ptr++ = "-xlto";
  *argv_ptr++ = "-c";
  for (j = 0; collect_gcc_options[j] != '\0'; ++j)
    if (collect_gcc_options[j] == '\'')
      {
	char *option;

	++j;
	i = j;
	while (collect_gcc_options[j] != '\'')
	  ++j;

	obstack_init (&env_obstack);
	obstack_grow (&env_obstack, &collect_gcc_options[i], j - i);
	obstack_1grow (&env_obstack, 0);
	option = XOBFINISH (&env_obstack, char *);
	if (seen_o)
	  {
	    linker_output = option;
	    seen_o = false;
	    continue;
	  }

	/* If we see -o, skip it and skip and record its argument.  */
	if (option[0] == '-' && option[1] == 'o')
	  {
	    if (option[2] == '\0')
	      seen_o = true;
	    else
	      linker_output = &option[2];
	    continue;
	  }

	if (strcmp (option, "-save-temps") == 0)
	  debug = 1;
	if (strcmp (option, "-v") == 0)
	  verbose = 1;

	/* We've handled these LTO options, do not pass them on.  */
	if (strcmp (option, "-flto") == 0)
	  lto_mode = LTO_MODE_LTO;
	else if (strcmp (option, "-fwhopr") == 0)
	  lto_mode = LTO_MODE_WHOPR;
	else
	  *argv_ptr++ = option;
      }

  if (linker_output)
    {
      char *output_dir, *base, *name;

      output_dir = xstrdup (linker_output);
      base = output_dir;
      for (name = base; *name; name++)
	if (IS_DIR_SEPARATOR (*name))
	  base = name + 1;
      *base = '\0';

      linker_output = &linker_output[base - output_dir];
      if (*output_dir == '\0')
	{
	  static char current_dir[] = { '.', DIR_SEPARATOR, '\0' };
	  output_dir = current_dir;
	}
      *argv_ptr++ = "-dumpdir";
      *argv_ptr++ = output_dir;

      *argv_ptr++ = "-dumpbase";
    }
  else
    argv_ptr--;

  if (lto_mode == LTO_MODE_LTO)
    {
      flto_out = make_temp_file (".lto.o");
      if (linker_output)
	argv_ptr[0] = linker_output;
      argv_ptr[1] = "-o";
      argv_ptr[2] = flto_out;
      argv_ptr[3] = "-combine";
    }
  else if (lto_mode == LTO_MODE_WHOPR)
    {
      const char *list_option = "-fltrans-output-list=";
      size_t list_option_len = strlen (list_option);
      char *tmp;

      if (linker_output)
	{
	  char *dumpbase = (char *) xmalloc (strlen (linker_output)
					     + sizeof(".wpa") + 1);
	  strcpy (dumpbase, linker_output);
	  strcat (dumpbase, ".wpa");
	  argv_ptr[0] = dumpbase;
	}

      ltrans_output_file = make_temp_file (".ltrans.out");
      list_option_full = (char *) xmalloc (sizeof (char) *
		         (strlen (ltrans_output_file) + list_option_len + 1));
      tmp = list_option_full;

      argv_ptr[1] = tmp;
      strcpy (tmp, list_option);
      tmp += list_option_len;
      strcpy (tmp, ltrans_output_file);

      argv_ptr[2] = "-fwpa";
      argv_ptr[3] = "-combine";
    }
  else
    fatal ("invalid LTO mode");

  /* Append the input objects and possible preceeding arguments.  */
  for (i = 1; i < argc; ++i)
    argv_ptr[3 + i] = argv[i];
  argv_ptr[3 + i] = NULL;

  fork_execute (CONST_CAST (char **, new_argv));

  if (lto_mode == LTO_MODE_LTO)
    {
      printf("%s\n", flto_out);
      free (flto_out);
      flto_out = NULL;
    }
  else if (lto_mode == LTO_MODE_WHOPR)
    {
      FILE *stream = fopen (ltrans_output_file, "r");
      int nr = 0;

      if (!stream)
	fatal_perror ("fopen: %s", ltrans_output_file);

      argv_ptr[1] = "-fltrans";

      for (;;)
	{
	  const unsigned piece = 32;
	  char *output_name;
	  char *buf, *input_name = (char *)xmalloc (piece);
	  size_t len;

	  buf = input_name;
cont:
	  if (!fgets (buf, piece, stream))
	    break;
	  len = strlen (input_name);
	  if (input_name[len - 1] != '\n')
	    {
	      input_name = (char *)xrealloc (input_name, len + piece);
	      buf = input_name + len;
	      goto cont;
	    }
	  input_name[len - 1] = '\0';

	  if (input_name[0] == '*')
	    {
	      continue;
	      output_name = &input_name[1];
	    }
	  else
	    {
	      /* Otherwise, add FILES[I] to lto_execute_ltrans command line
		 and add the resulting file to LTRANS output list.  */

	      /* Replace the .o suffix with a .ltrans.o suffix and write
		 the resulting name to the LTRANS output list.  */
	      obstack_init (&env_obstack);
	      obstack_grow (&env_obstack, input_name, strlen (input_name) - 2);
	      obstack_grow (&env_obstack, ".ltrans.o", sizeof (".ltrans.o"));
	      output_name = XOBFINISH (&env_obstack, char *);

	      if (linker_output)
		{
		  char *dumpbase
		    = (char *) xmalloc (strlen (linker_output)
					+ sizeof(DUMPBASE_SUFFIX) + 1);
		  snprintf (dumpbase,
			    strlen (linker_output) + sizeof(DUMPBASE_SUFFIX),
			    "%s.ltrans%d", linker_output, nr++);
		  argv_ptr[0] = dumpbase;
		}

	      argv_ptr[2] = "-o";
	      argv_ptr[3] = output_name;
	      argv_ptr[4] = input_name;
	      argv_ptr[5] = NULL;

	      fork_execute (CONST_CAST (char **, new_argv));

	      maybe_unlink_file (input_name);
	    }

	  fputs (output_name, stdout);
	  putc ('\n', stdout);
	}
      fclose (stream);
      maybe_unlink_file (ltrans_output_file);
      free (list_option_full);
    }
  else
    fatal ("invalid LTO mode");

  obstack_free (&env_obstack, NULL);
}


/* Entry point.  */

int
main (int argc, char *argv[])
{
  gcc_init_libintl ();

  /* We may be called with all the arguments stored in some file and
     passed with @file.  Expand them into argv before processing.  */
  expandargv (&argc, &argv);
  run_gcc (argc, argv);

  return 0;
}
