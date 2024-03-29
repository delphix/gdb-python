/* Shared general utility routines for GDB, the GNU debugger.

   Copyright (C) 1986-2019 Free Software Foundation, Inc.

   This file is part of GDB.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.  */

#include "common-defs.h"
#include "common-utils.h"
#include "host-defs.h"
#include <ctype.h>

/* The xmalloc() (libiberty.h) family of memory management routines.

   These are like the ISO-C malloc() family except that they implement
   consistent semantics and guard against typical memory management
   problems.  */

/* NOTE: These are declared using PTR to ensure consistency with
   "libiberty.h".  xfree() is GDB local.  */

PTR                            /* ARI: PTR */
xmalloc (size_t size)
{
  void *val;

  /* See libiberty/xmalloc.c.  This function need's to match that's
     semantics.  It never returns NULL.  */
  if (size == 0)
    size = 1;

  val = malloc (size);         /* ARI: malloc */
  if (val == NULL)
    malloc_failure (size);

  return val;
}

PTR                              /* ARI: PTR */
xrealloc (PTR ptr, size_t size)          /* ARI: PTR */
{
  void *val;

  /* See libiberty/xmalloc.c.  This function need's to match that's
     semantics.  It never returns NULL.  */
  if (size == 0)
    size = 1;

  if (ptr != NULL)
    val = realloc (ptr, size);	/* ARI: realloc */
  else
    val = malloc (size);	        /* ARI: malloc */
  if (val == NULL)
    malloc_failure (size);

  return val;
}

PTR                            /* ARI: PTR */           
xcalloc (size_t number, size_t size)
{
  void *mem;

  /* See libiberty/xmalloc.c.  This function need's to match that's
     semantics.  It never returns NULL.  */
  if (number == 0 || size == 0)
    {
      number = 1;
      size = 1;
    }

  mem = calloc (number, size);      /* ARI: xcalloc */
  if (mem == NULL)
    malloc_failure (number * size);

  return mem;
}

void *
xzalloc (size_t size)
{
  /* HACK: Round up to 8 bytes, fixes a problem with buffers of long double on
     32 bit (12 bytes) when filled from a 64 bit gdb (16 bytes).  Ugh.  */
  size = (size + 7) & ~(size_t)7;
  return xcalloc (1, size);
}

void
xmalloc_failed (size_t size)
{
  malloc_failure (size);
}

/* Like asprintf/vasprintf but get an internal_error if the call
   fails. */

char *
xstrprintf (const char *format, ...)
{
  char *ret;
  va_list args;

  va_start (args, format);
  ret = xstrvprintf (format, args);
  va_end (args);
  return ret;
}

char *
xstrvprintf (const char *format, va_list ap)
{
  char *ret = NULL;
  int status = vasprintf (&ret, format, ap);

  /* NULL is returned when there was a memory allocation problem, or
     any other error (for instance, a bad format string).  A negative
     status (the printed length) with a non-NULL buffer should never
     happen, but just to be sure.  */
  if (ret == NULL || status < 0)
    internal_error (__FILE__, __LINE__, _("vasprintf call failed"));
  return ret;
}

int
xsnprintf (char *str, size_t size, const char *format, ...)
{
  va_list args;
  int ret;

  va_start (args, format);
  ret = vsnprintf (str, size, format, args);
  gdb_assert (ret < size);
  va_end (args);

  return ret;
}

/* See documentation in common-utils.h.  */

std::string
string_printf (const char* fmt, ...)
{
  va_list vp;
  int size;

  va_start (vp, fmt);
  size = vsnprintf (NULL, 0, fmt, vp);
  va_end (vp);

  std::string str (size, '\0');

  /* C++11 and later guarantee std::string uses contiguous memory and
     always includes the terminating '\0'.  */
  va_start (vp, fmt);
  vsprintf (&str[0], fmt, vp);
  va_end (vp);

  return str;
}

/* See documentation in common-utils.h.  */

std::string
string_vprintf (const char* fmt, va_list args)
{
  va_list vp;
  size_t size;

  va_copy (vp, args);
  size = vsnprintf (NULL, 0, fmt, vp);
  va_end (vp);

  std::string str (size, '\0');

  /* C++11 and later guarantee std::string uses contiguous memory and
     always includes the terminating '\0'.  */
  vsprintf (&str[0], fmt, args);

  return str;
}


/* See documentation in common-utils.h.  */

void
string_appendf (std::string &str, const char *fmt, ...)
{
  va_list vp;

  va_start (vp, fmt);
  string_vappendf (str, fmt, vp);
  va_end (vp);
}


/* See documentation in common-utils.h.  */

void
string_vappendf (std::string &str, const char *fmt, va_list args)
{
  va_list vp;
  int grow_size;

  va_copy (vp, args);
  grow_size = vsnprintf (NULL, 0, fmt, vp);
  va_end (vp);

  size_t curr_size = str.size ();
  str.resize (curr_size + grow_size);

  /* C++11 and later guarantee std::string uses contiguous memory and
     always includes the terminating '\0'.  */
  vsprintf (&str[curr_size], fmt, args);
}

char *
savestring (const char *ptr, size_t len)
{
  char *p = (char *) xmalloc (len + 1);

  memcpy (p, ptr, len);
  p[len] = 0;
  return p;
}

/* The bit offset of the highest byte in a ULONGEST, for overflow
   checking.  */

#define HIGH_BYTE_POSN ((sizeof (ULONGEST) - 1) * HOST_CHAR_BIT)

/* True (non-zero) iff DIGIT is a valid digit in radix BASE,
   where 2 <= BASE <= 36.  */

static int
is_digit_in_base (unsigned char digit, int base)
{
  if (!isalnum (digit))
    return 0;
  if (base <= 10)
    return (isdigit (digit) && digit < base + '0');
  else
    return (isdigit (digit) || tolower (digit) < base - 10 + 'a');
}

static int
digit_to_int (unsigned char c)
{
  if (isdigit (c))
    return c - '0';
  else
    return tolower (c) - 'a' + 10;
}

/* As for strtoul, but for ULONGEST results.  */

ULONGEST
strtoulst (const char *num, const char **trailer, int base)
{
  unsigned int high_part;
  ULONGEST result;
  int minus = 0;
  int i = 0;

  /* Skip leading whitespace.  */
  while (isspace (num[i]))
    i++;

  /* Handle prefixes.  */
  if (num[i] == '+')
    i++;
  else if (num[i] == '-')
    {
      minus = 1;
      i++;
    }

  if (base == 0 || base == 16)
    {
      if (num[i] == '0' && (num[i + 1] == 'x' || num[i + 1] == 'X'))
	{
	  i += 2;
	  if (base == 0)
	    base = 16;
	}
    }

  if (base == 0 && num[i] == '0')
    base = 8;

  if (base == 0)
    base = 10;

  if (base < 2 || base > 36)
    {
      errno = EINVAL;
      return 0;
    }

  result = high_part = 0;
  for (; is_digit_in_base (num[i], base); i += 1)
    {
      result = result * base + digit_to_int (num[i]);
      high_part = high_part * base + (unsigned int) (result >> HIGH_BYTE_POSN);
      result &= ((ULONGEST) 1 << HIGH_BYTE_POSN) - 1;
      if (high_part > 0xff)
	{
	  errno = ERANGE;
	  result = ~ (ULONGEST) 0;
	  high_part = 0;
	  minus = 0;
	  break;
	}
    }

  if (trailer != NULL)
    *trailer = &num[i];

  result = result + ((ULONGEST) high_part << HIGH_BYTE_POSN);
  if (minus)
    return -result;
  else
    return result;
}

/* See documentation in common-utils.h.  */

char *
skip_spaces (char *chp)
{
  if (chp == NULL)
    return NULL;
  while (*chp && isspace (*chp))
    chp++;
  return chp;
}

/* A const-correct version of the above.  */

const char *
skip_spaces (const char *chp)
{
  if (chp == NULL)
    return NULL;
  while (*chp && isspace (*chp))
    chp++;
  return chp;
}

/* See documentation in common-utils.h.  */

const char *
skip_to_space (const char *chp)
{
  if (chp == NULL)
    return NULL;
  while (*chp && !isspace (*chp))
    chp++;
  return chp;
}

/* See documentation in common-utils.h.  */

char *
skip_to_space (char *chp)
{
  return (char *) skip_to_space ((const char *) chp);
}

/* See common/common-utils.h.  */

void
free_vector_argv (std::vector<char *> &v)
{
  for (char *el : v)
    xfree (el);

  v.clear ();
}

/* See common/common-utils.h.  */

std::string
stringify_argv (const std::vector<char *> &args)
{
  std::string ret;

  if (!args.empty () && args[0] != NULL)
    {
      for (auto s : args)
	if (s != NULL)
	  {
	    ret += s;
	    ret += ' ';
	  }

      /* Erase the last whitespace.  */
      ret.erase (ret.end () - 1);
    }

  return ret;
}

/* See common/common-utils.h.  */

ULONGEST
align_up (ULONGEST v, int n)
{
  /* Check that N is really a power of two.  */
  gdb_assert (n && (n & (n-1)) == 0);
  return (v + n - 1) & -n;
}

/* See common/common-utils.h.  */

ULONGEST
align_down (ULONGEST v, int n)
{
  /* Check that N is really a power of two.  */
  gdb_assert (n && (n & (n-1)) == 0);
  return (v & -n);
}
