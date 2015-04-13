/*****************************************************************************\
 **  mpi_pmix.c - Main plugin callbacks for PMIx support in SLURM
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

#if     HAVE_CONFIG_H
#  include "config.h"
#endif

#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>

#include "pmixp_common.h"
#include "pmixp_server.h"
#include "pmixp_debug.h"
#include "pmixp_agent.h"
#include "pmixp_info.h"

#include <pmix_server.h>
#include <pmixp_client.h>

/*
 * These variables are required by the generic plugin interface.  If they
 * are not found in the plugin, the plugin loader will ignore it.
 *
 * plugin_name - a string giving a human-readable description of the
 * plugin.  There is no maximum length, but the symbol must refer to
 * a valid string.
 *
 * plugin_type - a string suggesting the type of the plugin or its
 * applicability to a particular form of data or method of data handling.
 * If the low-level plugin API is used, the contents of this string are
 * unimportant and may be anything.  SLURM uses the higher-level plugin
 * interface which requires this string to be of the form
 *
 *      <application>/<method>
 *
 * where <application> is a description of the intended application of
 * the plugin (e.g., "switch" for SLURM switch) and <method> is a description
 * of how this plugin satisfies that application.  SLURM will only load
 * a switch plugin if the plugin_type string has a prefix of "switch/".
 *
 * plugin_version - an unsigned 32-bit integer giving the version number
 * of the plugin.  If major and minor revisions are desired, the major
 * version number may be multiplied by a suitable magnitude constant such
 * as 100 or 1000.  Various SLURM versions will likely require a certain
 * minimum version for their plugins as this API matures.
 */
const char plugin_name[]        = "PMIx plugin";
const char plugin_type[]        = "mpi/pmix";
const uint32_t plugin_version   = 100;

int p_mpi_hook_slurmstepd_prefork(const stepd_step_rec_t *job, char ***env)
{
	int ret;
	PMIXP_DEBUG("slurmstepd initialization");

	if( ( ret = pmixp_stepd_init(job, env) ) ){
		return ret;
	}
	ret = pmix_agent_start();
	return ret;
}

int p_mpi_hook_slurmstepd_task(const mpi_plugin_task_info_t *job,
							   char ***env)
{
	char **tmp_env = NULL;
	pmixp_debug_hang(0);
	pmix_libpmix_task_set(job->gtaskid, &tmp_env);
	if( NULL != tmp_env ){
		int i;
		for(i=0; NULL != tmp_env[i]; i++){
			char *value = strchr(tmp_env[i],'=');
			if( NULL != value ){
				*value = '\0';
				value++;
				env_array_overwrite(env, (const char *)tmp_env[i], value);
			}
			free(tmp_env[i]);
		}
		free(tmp_env);
		tmp_env = NULL;
	}
	pmix_agent_task_cleanup();
	return SLURM_SUCCESS;
}

mpi_plugin_client_state_t *
p_mpi_hook_client_prelaunch(const mpi_plugin_client_info_t *job, char ***env)
{
	// pmixp_debug_hang(1);

	PMIXP_DEBUG("srun initialization");
	if( SLURM_SUCCESS != pmix_srun_init(job,env) ){
		return NULL;
	}
	if( SLURM_SUCCESS != pmix_agent_start() ){
		return NULL;
	}
	/* only return NULL on error */
	return (void *)0xdeadbeef;
}

int p_mpi_hook_client_single_task_per_node(void)
{
	PMIXP_DEBUG("Single task per node");
	return false;
}

int p_mpi_hook_client_fini()
{
	PMIXP_DEBUG("Cleanup client");
	return SLURM_SUCCESS;
}
