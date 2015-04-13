/*****************************************************************************\
 **  pmix_info.c - PMIx various environment information
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

#include <string.h>
#include "pmixp_common.h"
#include "pmixp_debug.h"
#include "pmixp_info.h"

// Client communication
static int _cli_fd = -1;

// Server communication
static char *_server_addr = NULL;
static int _server_fd = -1;

pmix_jobinfo_t _pmixp_job_info  = { 0 };

// Collective tree description
char *_pmix_job_nodes_list = NULL;
char *_pmix_step_nodes_list = NULL;
int _pmix_child_num = -1;
int *_pmix_child_list = NULL;

// Client contact information
void pmixp_info_cli_contacts(int fd)
{
	_cli_fd = fd;
}

int pmix_info_cli_fd()
{
	// Check that client fd was created
	xassert( _cli_fd >= 0  );
	return _cli_fd;
}

// stepd global contact information
void pmixp_info_srv_contacts(char *path, int fd)
{
	_server_addr = xstrdup(path);
	_server_fd = fd;
}

const char *pmixp_info_srv_addr()
{
	// Check that Server address was initialized
	xassert( _server_addr != NULL );
	return _server_addr;
}

int pmixp_info_srv_fd()
{
	// Check that Server fd was created
	xassert( _server_fd >= 0  );
	return _server_fd;
}

int pmixp_info_set(const stepd_step_rec_t *job, char ***env)
{
	int i, rc;
#ifndef NDEBUG
	_pmixp_job_info.magic = PMIX_INFO_MAGIC;
#endif

	// This node info
	_pmixp_job_info.jobid      = job->jobid;
	_pmixp_job_info.stepid     = job->stepid;
	_pmixp_job_info.node_id    = job->nodeid;
	_pmixp_job_info.node_tasks = job->node_tasks;

	// Global info
	_pmixp_job_info.ntasks     = job->ntasks;
	_pmixp_job_info.nnodes     = job->nnodes;
	_pmixp_job_info.task_cnts  = xmalloc( sizeof(*_pmixp_job_info.task_cnts) * _pmixp_job_info.nnodes);
	for(i = 0; i < _pmixp_job_info.nnodes; i++){
		_pmixp_job_info.task_cnts[i] = job->task_cnts[i];
	}

	_pmixp_job_info.gtids = xmalloc(_pmixp_job_info.node_tasks * sizeof(uint32_t));
	for (i = 0; i < job->node_tasks; i ++) {
		_pmixp_job_info.gtids[i] = job->task[i]->gtid;
	}

	// Setup hostnames and job-wide info
	if( (rc = pmixp_info_resources_set(env)) ){
		return rc;
	}

	snprintf(_pmixp_job_info.nspace, PMIX_MAX_NSLEN, "slurm.pmix.%d%d",
		   pmixp_info_jobid(), pmixp_info_stepid());

	return SLURM_SUCCESS;
}

static eio_handle_t *_io_handle = NULL;

void pmixp_info_io_set(eio_handle_t *h)
{
	_io_handle = h;
}

eio_handle_t *pmixp_info_io()
{
	xassert( _io_handle != NULL );
	return _io_handle;
}

/*
 * Job and step nodes/tasks count and hostname extraction routines
 */

/*
 * Derived from src/srun/libsrun/opt.c
 * _get_task_count()
 *
 * FIXME: original _get_task_count has some additinal ckeck
 * for opt.ntasks_per_node & opt.cpus_set
 * Should we care here?
 *//*
static int _get_task_count(char ***env, uint32_t *tasks, uint32_t *cpus)
{
	pmixp_debug_hang(1);
	char *cpus_per_node = NULL, *cpus_per_task_env = NULL, *end_ptr = NULL;
	int cpus_per_task = 1, cpu_count, node_count, task_count;
	int total_tasks = 0, total_cpus = 0;

	cpus_per_node = getenvp(*env, PMIX_CPUS_PER_NODE_ENV);
	if( cpus_per_node == NULL ){
		PMIXP_ERROR_NO(0,"Cannot find %s environment variable", PMIX_CPUS_PER_NODE_ENV);
		return SLURM_ERROR;
	}
	cpus_per_task_env = getenvp(*env, PMIX_CPUS_PER_TASK);
	if( cpus_per_task_env != NULL ){
		cpus_per_task = strtol(cpus_per_task_env, &end_ptr, 10);
	}

	cpu_count = strtol(cpus_per_node, &end_ptr, 10);
	task_count = cpu_count / cpus_per_task;
	while (1) {
		if ((end_ptr[0] == '(') && (end_ptr[1] == 'x')) {
			end_ptr += 2;
			node_count = strtol(end_ptr, &end_ptr, 10);
			task_count *= node_count;
			total_tasks += task_count;
			cpu_count *= node_count;
			total_cpus += cpu_count;
			if (end_ptr[0] == ')')
				end_ptr++;
		} else if ((end_ptr[0] == ',') || (end_ptr[0] == 0))
			total_tasks += task_count;
		else {
			PMIXP_ERROR_NO(0,"Invalid value for environment variable %s (%s)",
						  PMIX_CPUS_PER_NODE_ENV, cpus_per_node);
			return SLURM_ERROR;
		}
		if (end_ptr[0] == ',')
			end_ptr++;
		if (end_ptr[0] == 0)
			break;
	}
	*tasks = total_tasks;
	*cpus = total_cpus;
	return 0;
}
*/


int pmixp_info_resources_set(char ***env)
{
	char *p = NULL;

	memset(&_pmixp_job_info, 0, sizeof(_pmixp_job_info));
	// Initialize all memory pointers that would be allocated to NULL
	// So in case of error exit we will know what to xfree
	_pmixp_job_info.job_hl = hostlist_create("");
	_pmixp_job_info.step_hl = hostlist_create("");
	_pmixp_job_info.hostname = NULL;

	// Save step host list
	p = getenvp(*env, PMIX_STEP_NODES_ENV);
	if (!p) {
		PMIXP_ERROR_NO(ENOENT, "Environment variable %s not found", PMIX_STEP_NODES_ENV);
		goto err_exit;
	}
	hostlist_push(_pmixp_job_info.step_hl, p);

	// Extract our node name
	p = hostlist_nth(_pmixp_job_info.step_hl, _pmixp_job_info.node_id);
	_pmixp_job_info.hostname = xstrdup(p);
	free(p);

	// Determine job-wide node id and job-wide node count
	p = getenvp(*env, PMIX_JOB_NODES_ENV);
	if( p == NULL ){
		// shouldn't happen if we are under SLURM!
		PMIXP_ERROR_NO(ENOENT, "No %s environment variable found!", PMIX_JOB_NODES_ENV);
		goto err_exit;
	}
	hostlist_push(_pmixp_job_info.job_hl, p);
	_pmixp_job_info.nnodes_job = hostlist_count(_pmixp_job_info.job_hl);
	_pmixp_job_info.node_id_job = hostlist_find(_pmixp_job_info.job_hl,
						    _pmixp_job_info.hostname);

	// FIXME!! -------------------------------------------------------------
	/* TODO: _get_task_count not always works well.
	if( _get_task_count(env, &_pmixp_job_info.ntasks_job, &_pmixp_job_info.ncpus_job ) < 0 ){
		_pmixp_job_info.ntasks_job  = _pmixp_job_info.ntasks;
		_pmixp_job_info.ncpus_job  = _pmixp_job_info.ntasks;
	}
	xassert( _pmixp_job_info.ntasks <= _pmixp_job_info.ntasks_job );
	*/
	_pmixp_job_info.ntasks_job  = _pmixp_job_info.ntasks;
	_pmixp_job_info.ncpus_job  = _pmixp_job_info.ntasks;

	//---------------------------------------------------------------------

	// Get and parse task-to-node mapping
	p = getenvp(*env, PMIX_SLURM_MAPPING_ENV);
	if( p == NULL ){
		// Direct modex won't work
		PMIXP_ERROR_NO(ENOENT, "No %s environment variable found!", PMIX_SLURM_MAPPING_ENV);
		goto err_exit;
	}

	_pmixp_job_info.task_map_packed = xstrdup(p);

	return SLURM_SUCCESS;
err_exit:
	hostlist_destroy(_pmixp_job_info.job_hl);
	hostlist_destroy(_pmixp_job_info.step_hl);
	if( NULL != _pmixp_job_info.hostname ){
		xfree(_pmixp_job_info.hostname);
	}
	return SLURM_ERROR;
}
