/* Python/gdb header for generic use in gdb

   Copyright (C) 2008-2019 Free Software Foundation, Inc.

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

#ifndef PYTHON_PYTHON_H
#define PYTHON_PYTHON_H

#include "extension.h"

/* This is all that python exports to gdb.  */
extern const struct extension_language_defn extension_language_python;

/* Command element for the 'python' command.  */
extern cmd_list_element *python_cmd_element;

extern void run_python_script (int argc, char **argv);

#endif /* PYTHON_PYTHON_H */
