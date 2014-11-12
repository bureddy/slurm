/*****************************************************************************\
 **  pmix_db.h - PMIx KVS database
 *****************************************************************************
 *  Copyright (C) 2014 Institude of Semiconductor Physics Siberian Branch of
 *                     Russian Academy of Science
 *  Written by Artem Polyakov <artpol84@gmail.com>.
 *  All rights reserved.
 *
 *  This file is part of SLURM, a resource management program.
 *  For details, see <http://slurm.schedmd.com/>.
 *  Please also read the included file: DISCLAIMER.
 *
 *  SLURM is free software; you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or (at your option)
 *  any later version.
 *
 *  In addition, as a special exception, the copyright holders give permission
 *  to link the code of portions of this program with the OpenSSL library under
 *  certain conditions as described in each individual source file, and
 *  distribute linked combinations including the two. You must obey the GNU
 *  General Public License in all respects for all of the code used other than
 *  OpenSSL. If you modify file(s) with this exception, you may extend this
 *  exception to your version of the file(s), but you are not obligated to do
 *  so. If you do not wish to do so, delete this exception statement from your
 *  version.  If you delete this exception statement from all source files in
 *  the program, then also delete it here.
 *
 *  SLURM is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *  details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with SLURM; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA.
\*****************************************************************************/

#ifndef PMIX_DB_H
#define PMIX_DB_H

#include "pmix_common.h"
#include "pmix_info.h"
#include "pmix_debug.h"

typedef struct {
#ifndef NDEBUG
#       define PMIX_MSGSTATE_MAGIC 0xdeadbeef
	int  magic;
#endif
	int *upd;
	void **blobs;
	int *blob_sizes;
} pmix_db_t;

extern pmix_db_t pmix_db;

static inline void pmix_db_init()
{
	int i;
	pmix_db.magic = PMIX_MSGSTATE_MAGIC;
	uint32_t tasks = pmix_info_tasks();
	pmix_db.upd = xmalloc(sizeof(int)*tasks);
	pmix_db.blobs = xmalloc( sizeof(int*) * tasks );
	pmix_db.blob_sizes = xmalloc( sizeof(int) * tasks );
	for(i=0;i<tasks;i++){
		pmix_db.upd[i] = 0;
	}
}

static inline void pmix_db_update_init()
{
	// Mark everybody as non-reported
	int i;
	for(i=0;i < pmix_info_tasks();i++){
		pmix_db.upd[i] = 0;
	}
}

static inline void pmix_db_update_verify()
{
	int i;

	xassert(pmix_db.magic == PMIX_MSGSTATE_MAGIC);
	// Everybody has to report
	for(i=0;i < pmix_info_tasks(); i++){
		if( !pmix_db.upd[i] ){
			PMIX_ERROR("Task %d have not reported!", i);
			xassert( pmix_db.upd[i] );
		}
	}
}

static inline void pmix_db_add_blob(int taskid, void *blob, int size)
{
	xassert(pmix_db.magic == PMIX_MSGSTATE_MAGIC);
	pmix_db.upd[taskid] = taskid;
	pmix_db.blobs[taskid] = blob;
	pmix_db.blob_sizes[taskid] = size;
	pmix_db.upd[taskid] = 1;
}

static inline int pmix_db_get_blob(int taskid, void **blob)
{
	xassert(pmix_db.magic == PMIX_MSGSTATE_MAGIC);
	pmix_db.upd[taskid] = taskid;
	*blob = pmix_db.blobs[taskid];
	return pmix_db.blob_sizes[taskid];
}

#endif // PMIX_DB_H