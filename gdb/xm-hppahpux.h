/* Parameters for execution on a Hewlett-Packard PA-RISC machine, running 
   HPUX or BSD.
   Copyright (C) 1986, 1987, 1989, 1990, 1991 Free Software Foundation, Inc. 

   Contributed by the Center for Software Science at the
   University of Utah (pa-gdb-bugs@cs.utah.edu).

This file is part of GDB.

GDB is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 1, or (at your option)
any later version.

GDB is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with GDB; see the file COPYING.  If not, write to
the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.  */

/* Describe the endian nature of this machine.  */
#define BITS_BIG_ENDIAN 1
#define BYTES_BIG_ENDIAN 1
#define WORDS_BIG_ENDIAN 1

/* Avoid "INT_MIN redefined" warnings -- by defining it here, exactly
   the same as in the system <machine/machtypes.h> file.  */
#undef  INT_MIN
#define INT_MIN         0x80000000

#ifndef hp800
#define USG
#endif

#define HAVE_TERMIO

#define PUSH_ARGUMENTS(nargs, args, sp, struct_return, struct_addr) \
    sp = hp_push_arguments(nargs, args, sp, struct_return, struct_addr)

#define KERNEL_U_ADDR 0

/* What a coincidence! */
#define REGISTER_U_ADDR(addr, blockend, regno)				\
{ addr = (int)(blockend) + REGISTER_BYTE (regno);}


#define U_REGS_OFFSET 0
