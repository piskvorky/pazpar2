/* This file is part of Pazpar2.
   Copyright (C) 2006-2013 Index Data

Pazpar2 is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free
Software Foundation; either version 2, or (at your option) any later
version.

Pazpar2 is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or
FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA

*/

#ifndef SERVICE_XSLT_H
#define SERVICE_XSLT_H

#include <libxslt/xsltutils.h>
#include <libxslt/transform.h>

struct conf_service;

xsltStylesheetPtr service_xslt_get(struct conf_service *service,
                                   const char *id);

void service_xslt_destroy(struct conf_service *service);

int service_xslt_config(struct conf_service *service, xmlNode *n);

#endif

/*
 * Local variables:
 * c-basic-offset: 4
 * c-file-style: "Stroustrup"
 * indent-tabs-mode: nil
 * End:
 * vim: shiftwidth=4 tabstop=8 expandtab
 */

