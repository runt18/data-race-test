
/*--------------------------------------------------------------------*/
/*--- DEBUG/RELEASE conditional compilation directives.            ---*/
/*---                                               hg_debugonly.h ---*/
/*--------------------------------------------------------------------*/

/*
   This file is part of Helgrind, a Valgrind tool for detecting errors
   in threaded programs.

   Copyright (C) 2008-2008 Google Inc
       opensource@google.com 

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 of the
   License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
   02111-1307, USA.

   The GNU General Public License is contained in the file COPYING.
*/

#ifndef __HG_DEBUGONLY_H
#define __HG_DEBUGONLY_H

//TODO: check? better way?
#ifdef HG_DEBUG_OPTION
#define DEBUG_ONLY(a)      a
#define RELEASE_ONLY(a)
#define tl_debug_assert(a) tl_assert(a)
#else
#define DEBUG_ONLY(a)
#define RELEASE_ONLY(a)    a
#define tl_debug_assert(a)  
#endif

#endif /* ! __HG_DEBUGONLY_H */
