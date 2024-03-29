/* C language support definitions for GDB, the GNU debugger.

   Copyright (C) 1992-2019 Free Software Foundation, Inc.

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


#if !defined (C_LANG_H)
#define C_LANG_H 1

struct ui_file;
struct language_arch_info;
struct type_print_options;
struct parser_state;

#include "value.h"
#include "macroexp.h"
#include "parser-defs.h"
#include "common/enum-flags.h"


/* The various kinds of C string and character.  Note that these
   values are chosen so that they may be or'd together in certain
   ways.  */
enum c_string_type_values : unsigned
  {
    /* An ordinary string: "value".  */
    C_STRING = 0,
    /* A wide string: L"value".  */
    C_WIDE_STRING = 1,
    /* A 16-bit Unicode string: u"value".  */
    C_STRING_16 = 2,
    /* A 32-bit Unicode string: U"value".  */
    C_STRING_32 = 3,
    /* An ordinary char: 'v'.  This can also be or'd with one of the
       above to form the corresponding CHAR value from a STRING
       value.  */
    C_CHAR = 4,
    /* A wide char: L'v'.  */
    C_WIDE_CHAR = 5,
    /* A 16-bit Unicode char: u'v'.  */
    C_CHAR_16 = 6,
    /* A 32-bit Unicode char: U'v'.  */
    C_CHAR_32 = 7
  };

DEF_ENUM_FLAGS_TYPE (enum c_string_type_values, c_string_type);

/* Defined in c-exp.y.  */

extern int c_parse (struct parser_state *);

extern int c_parse_escape (const char **, struct obstack *);

/* Defined in c-typeprint.c */
extern void c_print_type (struct type *, const char *,
			  struct ui_file *, int, int,
			  const struct type_print_options *);

/* Print a type but allow the precise language to be specified.  */

extern void c_print_type (struct type *, const char *,
			  struct ui_file *, int, int,
			  enum language,
			  const struct type_print_options *);

extern void c_print_typedef (struct type *,
			     struct symbol *,
			     struct ui_file *);

extern void c_val_print (struct type *,
			 LONGEST, CORE_ADDR,
			 struct ui_file *, int,
			 struct value *,
			 const struct value_print_options *);

extern void c_value_print (struct value *, struct ui_file *,
			   const struct value_print_options *);

/* These are in c-lang.c: */

extern struct value *evaluate_subexp_c (struct type *expect_type,
					struct expression *exp,
					int *pos,
					enum noside noside);

extern void c_printchar (int, struct type *, struct ui_file *);

extern void c_printstr (struct ui_file * stream,
			struct type *elttype,
			const gdb_byte *string,
			ULONGEST length,
			const char *user_encoding,
			int force_ellipses,
			const struct value_print_options *options);

extern void c_language_arch_info (struct gdbarch *gdbarch,
				  struct language_arch_info *lai);

extern const struct exp_descriptor exp_descriptor_c;

extern void c_emit_char (int c, struct type *type,
			 struct ui_file *stream, int quoter);

extern const struct op_print c_op_print_tab[];

extern gdb::unique_xmalloc_ptr<char> c_watch_location_expression
     (struct type *type, CORE_ADDR addr);

/* These are in c-typeprint.c: */

extern void c_type_print_base (struct type *, struct ui_file *,
			       int, int, const struct type_print_options *);

/* These are in cp-valprint.c */

extern void cp_print_class_member (const gdb_byte *, struct type *,
				   struct ui_file *, const char *);

extern void cp_print_value_fields (struct type *, struct type *,
				   LONGEST, CORE_ADDR,
				   struct ui_file *, int,
				   struct value *,
				   const struct value_print_options *,
				   struct type **, int);

extern void cp_print_value_fields_rtti (struct type *,
					const gdb_byte *, LONGEST, CORE_ADDR,
					struct ui_file *, int,
					struct value *,
					const struct value_print_options *,
					struct type **, int);

extern int cp_is_vtbl_ptr_type (struct type *);

extern int cp_is_vtbl_member (struct type *);

/* These are in c-valprint.c.  */

extern int c_textual_element_type (struct type *, char);

/* Create a new instance of the C compiler and return it.  The new
   compiler is owned by the caller and must be freed using the destroy
   method.  This function never returns NULL, but rather throws an
   exception on failure.  This is suitable for use as the
   la_get_compile_instance language method.  */

extern compile_instance *c_get_compile_context (void);

/* Create a new instance of the C++ compiler and return it.  The new
   compiler is owned by the caller and must be freed using the destroy
   method.  This function never returns NULL, but rather throws an
   exception on failure.  This is suitable for use as the
   la_get_compile_instance language method.  */

extern compile_instance *cplus_get_compile_context ();

/* This takes the user-supplied text and returns a new bit of code to
   compile.

   This is used as the la_compute_program language method; see that
   for a description of the arguments.  */

extern std::string c_compute_program (compile_instance *inst,
				      const char *input,
				      struct gdbarch *gdbarch,
				      const struct block *expr_block,
				      CORE_ADDR expr_pc);

/* This takes the user-supplied text and returns a new bit of code to compile.

   This is used as the la_compute_program language method; see that
   for a description of the arguments.  */

extern std::string cplus_compute_program (compile_instance *inst,
					  const char *input,
					  struct gdbarch *gdbarch,
					  const struct block *expr_block,
					  CORE_ADDR expr_pc);

#endif /* !defined (C_LANG_H) */
