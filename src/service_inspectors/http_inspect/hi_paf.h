/****************************************************************************
 *
** Copyright (C) 2014 Cisco and/or its affiliates. All rights reserved.
 * Copyright (C) 2011-2013 Sourcefire, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License Version 2 as
 * published by the Free Software Foundation.  You may not use, modify or
 * distribute this program under any other version of the GNU General
 * Public License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 ****************************************************************************/

//--------------------------------------------------------------------
// hi stuff
//
// @file    hi_paf.h
// @author  Russ Combs <rcombs@sourcefire.com>
//--------------------------------------------------------------------

#ifndef HI_PAF_H
#define HI_PAF_H

#include "snort_types.h"
#include "stream/stream_api.h"

bool hi_paf_init(uint32_t cap);
void hi_paf_term(void);

int hi_paf_register_service(SnortConfig*, uint16_t service, bool client, bool server, bool auto_on);
bool hi_paf_simple_request(Flow*);

#endif

