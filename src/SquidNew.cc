
/*
 * $Id$
 *
 * DEBUG: none          Memory Allocation
 * AUTHOR: Robert Collins
 *
 * SQUID Web Proxy Cache          http://www.squid-cache.org/
 * ----------------------------------------------------------
 *
 *  Squid is the result of efforts by numerous individuals from
 *  the Internet community; see the CONTRIBUTORS file for full
 *  details.   Many organizations have provided support for Squid's
 *  development; see the SPONSORS file for full details.  Squid is
 *  Copyrighted (C) 2001 by the Regents of the University of
 *  California; see the COPYRIGHT file for full details.  Squid
 *  incorporates software developed and/or copyrighted by other
 *  sources; see the CREDITS file for full details.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111, USA.
 *
 */

#define _SQUID_EXTERNNEW_

#include "squid-old.h"


#ifdef __SUNPRO_CC

#include <new>
void *operator new(size_t size) throw (std::bad_alloc)
{
    return xmalloc(size);
}
void operator delete (void *address) throw()
{
    xfree (address);
}
void *operator new[] (size_t size) throw (std::bad_alloc)
{
    return xmalloc(size);
}
void operator delete[] (void *address) throw()
{
    xfree (address);
}

#endif /* __SUNPRO_CC */
