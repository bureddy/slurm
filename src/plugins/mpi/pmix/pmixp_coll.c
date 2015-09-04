/*****************************************************************************\
 **  pmix_coll.c - PMIx collective primitives
 *****************************************************************************
 *  Copyright (C) 2014-2015 Artem Polyakov. All rights reserved.
 *  Copyright (C) 2015      Mellanox Technologies. All rights reserved.
 *  Written by Artem Polyakov <artpol84@gmail.com, artemp@mellanox.com>.
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

#include "pmixp_common.h"
#include "src/slurmd/common/reverse_tree_math.h"
#include "src/common/slurm_protocol_api.h"
#include "pmixp_coll.h"
#include "pmixp_nspaces.h"
#include "pmixp_server.h"

static void _progress_fan_in(pmixp_coll_t *coll);
static int _is_child_no(pmixp_coll_t *coll, int nodeid);

static int
_hostset_from_ranges(const pmix_proc_t *procs, size_t nprocs,
		     hostlist_t *hl_out)
{
	int i;
	hostlist_t hl = hostlist_create("");
	pmixp_namespace_t *nsptr = NULL;
	for(i=0; i<nprocs; i++){
		char *node = NULL;
		hostlist_t tmp;
		nsptr = pmixp_nspaces_find(procs[i].nspace);
		if( NULL == nsptr ){
			goto err_exit;
		}
		if( procs[i].rank == PMIX_RANK_WILDCARD ){
			tmp = hostlist_copy(nsptr->hl);
		} else {
			tmp = pmixp_nspace_rankhosts(nsptr, &procs[i].rank, 1);
		}
		while( NULL != (node = hostlist_pop(tmp)) ){
			hostlist_push(hl, node);
			free(node);
		}
		hostlist_destroy(tmp);
	}
	hostlist_uniq(hl);
	*hl_out = hl;
	return SLURM_SUCCESS;
err_exit:
	hostlist_destroy(hl);
	return SLURM_ERROR;
}

static int _pack_ranges(pmixp_coll_t *coll)
{
	pmix_proc_t *procs = coll->procs;
	size_t nprocs = coll->nprocs;
	uint32_t size;
	int i;

	// 1. store the type of collective
	size = coll->type;
	pack32(size,coll->buf);

	// 2. Put the number of ranges
	pack32(nprocs, coll->buf);
	for(i=0; i< (int)nprocs; i++){
		// Pack namespace
		packmem(procs->nspace, strlen(procs->nspace) + 1, coll->buf);
		pack32(procs->rank, coll->buf);
	}

	return SLURM_SUCCESS;
}

static void _reset_coll(pmixp_coll_t *coll)
{
	switch( coll->state ){
	case PMIXP_COLL_SYNC:
		/* already reset */
		break;
	case PMIXP_COLL_FAN_IN:
		set_buf_offset(coll->buf, 0);
	case PMIXP_COLL_FAN_OUT:
		coll->state = PMIXP_COLL_SYNC;
		memset(coll->ch_contribs, 0, sizeof(int) * coll->children_cnt);
		coll->seq++; /* move to the next collective */
		coll->contrib_cntr = 0;
		coll->cbdata = NULL;
		coll->cbfunc = NULL;
		break;
	default:
		PMIXP_ERROR("Bad collective state = %d", coll->state);
	}
}

int pmixp_coll_unpack_ranges(Buf buf, pmixp_coll_type_t *type,
			     pmix_proc_t **r, size_t *nr)
{
	pmix_proc_t *procs = NULL;
	uint32_t nprocs = 0;
	uint32_t tmp;
	int i, rc;

	// 1. extract the type of collective
	if( SLURM_SUCCESS != ( rc = unpack32(&tmp, buf))){
		PMIXP_ERROR("Cannot unpack collective type");
		return rc;
	}
	*type = tmp;

	// 2. get the number of ranges
	if( SLURM_SUCCESS != ( rc = unpack32(&nprocs, buf))){
		PMIXP_ERROR("Cannot unpack collective type");
		return rc;
	}
	*nr = nprocs;

	procs = xmalloc(sizeof(pmix_proc_t) * (nprocs));
	*r = procs;

	for(i=0; i< (int)nprocs; i++){
		// 3. get namespace/rank of particular process
		rc = unpackmem(procs[i].nspace, &tmp, buf);
		if( SLURM_SUCCESS != rc ){
			PMIXP_ERROR("Cannot unpack namespace for process #%d", i);
			return rc;
		}
		procs[i].nspace[tmp] = '\0';

		unsigned int tmp;
		rc = unpack32(&tmp, buf);
		procs[i].rank = tmp;
		if( SLURM_SUCCESS != rc ){
			PMIXP_ERROR("Cannot unpack ranks for process #%d, nsp=%s",
				    i, procs[i].nspace);
			return rc;
		}
	}
	return SLURM_SUCCESS;
}

int pmixp_coll_belong_chk(pmixp_coll_type_t type, const pmix_proc_t *procs,
			  size_t nprocs)
{
	int i;
	pmixp_namespace_t *nsptr = pmixp_nspaces_local();
	// Find my namespace in the range
	for(i=0; i < nprocs; i++){
		if( 0 != strcmp(procs[i].nspace, nsptr->name) ){
			continue;
		}
		if( (procs[i].rank == PMIX_RANK_WILDCARD) )
			return 0;
		if( 0 <= pmixp_info_taskid2localid(procs[i].rank) ){
			return 0;
		}
	}
	// we don't participate in this collective!
	PMIXP_ERROR("Have collective that doesn't include this job's namespace");
	return -1;
}


/*
 * Based on ideas provided by Hongjia Cao <hjcao@nudt.edu.cn> in PMI2 plugin
 */
int pmixp_coll_init(pmixp_coll_t *coll, const pmix_proc_t *procs, size_t nprocs,
		    pmixp_coll_type_t type)
{
	hostlist_t hl;
	uint32_t nodeid = 0, nodes = 0;
	int parent_id, depth, max_depth, tmp;
	int width, my_nspace = -1;
	char *p;

#ifndef NDEBUG
	coll->magic = PMIXP_COLL_STATE_MAGIC;
#endif
	coll->type = type;
	coll->state = PMIXP_COLL_SYNC;
	coll->procs = xmalloc( sizeof(*procs) * nprocs);
	memcpy(coll->procs, procs, sizeof(*procs) * nprocs);
	coll->nprocs = nprocs;
	coll->my_nspace = my_nspace;

	if( SLURM_SUCCESS != _hostset_from_ranges(procs, nprocs, &hl) ){
		// TODO: provide ranges output routine
		PMIXP_ERROR("Bad ranges information");
		goto err_exit;
	}

	width = slurm_get_tree_width();
	nodes = hostlist_count(hl);
	nodeid = hostlist_find(hl,pmixp_info_hostname());
	reverse_tree_info(nodeid, nodes, width, &parent_id, &tmp,
			  &depth, &max_depth);
	coll->children_cnt = tmp;
	coll->nodeid = nodeid;

	// We interested in amount of direct childs
	coll->seq = 0;
	coll->contrib_cntr = 0;
	coll->contrib_local = false;
	coll->ch_nodeids = xmalloc( sizeof(int) * width );
	coll->ch_contribs = xmalloc( sizeof(int) * width );
	coll->children_cnt = reverse_tree_direct_children(nodeid, nodes, width,
							  depth, coll->ch_nodeids);

	if( parent_id == -1 ){
		coll->parent_host = NULL;
	} else if( parent_id >= 0 ){
		p = hostlist_nth(hl, parent_id);
		coll->parent_host = xstrdup(p);
		free(p);
	}
	coll->all_children = hl;

	/* Collective data */
	coll->buf = pmixp_server_new_buf();

	if( SLURM_SUCCESS != _pack_ranges(coll) ){
		PMIXP_ERROR("Cannot pack ranges to coll message header!");
		goto err_exit;
	}

	/* Callback information */
	coll->cbdata = NULL;
	coll->cbfunc = NULL;

	/* init fine grained lock */
	pthread_mutexattr_t attr;
	pthread_mutexattr_init(&attr);
	pthread_mutex_init(&coll->lock, &attr);
	pthread_mutexattr_destroy(&attr);

	return SLURM_SUCCESS;
err_exit:
	return SLURM_ERROR;
}

int pmixp_coll_contrib_local(pmixp_coll_t *coll, char *data, size_t size)
{
	PMIXP_DEBUG("%s:%d: get local contribution",
		    pmixp_info_namespace(), pmixp_info_nodeid() );

	/* sanity check */
	pmixp_coll_sanity_check(coll);

	/* lock the structure */
	pthread_mutex_lock(&coll->lock);

	/* change the collective state if need */
	if( PMIXP_COLL_SYNC == coll->state ){
		PMIXP_DEBUG("%s:%d: get local contribution: switch to PMIXP_COLL_FAN_IN",
			    pmixp_info_namespace(), pmixp_info_nodeid() );
		coll->state = PMIXP_COLL_FAN_IN;
		coll->ts = time(NULL);
	}
	xassert( PMIXP_COLL_FAN_IN == coll->state);

	/* save & mark local contribution */
	coll->contrib_local = true;
	grow_buf(coll->buf,size);
	memcpy(get_buf_data(coll->buf) + get_buf_offset(coll->buf), data, size);
	set_buf_offset(coll->buf, get_buf_offset(coll->buf) + size);

	/* unlock the structure */
	pthread_mutex_unlock(&coll->lock);

	/* check if the collective is ready to progress */
	_progress_fan_in(coll);

	PMIXP_DEBUG("%s:%d: get local contribution: finish",
		    pmixp_info_namespace(), pmixp_info_nodeid() );

	return SLURM_SUCCESS;
}

int pmixp_coll_contrib_node(pmixp_coll_t *coll, char *nodename, Buf buf)
{
	int idx;
	int nodeid;
	char *data = NULL;
	uint32_t size;


	PMIXP_DEBUG("%s:%d: get contribution from node %s",
		    pmixp_info_namespace(), pmixp_info_nodeid(), nodename);

	/* lock the structure */
	pthread_mutex_lock(&coll->lock);

	pmixp_coll_sanity_check(coll);

	/* fix the collective status if need */
	if( PMIXP_COLL_SYNC == coll->state ){
		PMIXP_DEBUG("%s:%d: get contribution from node %s: switch to PMIXP_COLL_FAN_IN",
			    pmixp_info_namespace(), pmixp_info_nodeid(), nodename);
		coll->state = PMIXP_COLL_FAN_IN;
		coll->ts = time(NULL);
	}
	xassert( PMIXP_COLL_FAN_IN == coll->state);

	/* Because of possible timeouts/delays in transmission we
	 * can receive a contribution second time. Avoid duplications
	 * by checking our records. */
	nodeid = hostlist_find(coll->all_children, nodename);
	idx = _is_child_no(coll, nodeid);
	xassert( 0 <= idx );
	if( 0 < coll->ch_contribs[idx]){
		/* shouldn't be grater than 1!
		 * it's OK - may happend because of retransmissions.
		 * just ignore */
		xassert(1 == coll->ch_contribs[idx]);
		PMIXP_DEBUG("Multiple contributions from nodeid=%d, hostname=%s",
			    nodeid, nodename);
		/* this is duplication, skip. */
		goto proceed;
	}

	data = get_buf_data(buf) + get_buf_offset(buf);
	size = remaining_buf(buf);
	grow_buf(coll->buf,size);
	memcpy(get_buf_data(coll->buf) + get_buf_offset(coll->buf), data, size);
	set_buf_offset(coll->buf, get_buf_offset(coll->buf) + size);

	/* increase number of individual contributions */
	coll->ch_contribs[idx]++;

	/* increase number of total contributions */
	coll->contrib_cntr++;

proceed:
	/* unlock the structure */
	pthread_mutex_unlock(&coll->lock);

	/* make a progress */
	_progress_fan_in(coll);

	PMIXP_DEBUG("%s:%d: get contribution from node %s: finish",
		    pmixp_info_namespace(), pmixp_info_nodeid(), nodename);

	return SLURM_SUCCESS;
}

static int _is_child_no(pmixp_coll_t *coll, int nodeid)
{
	// Check for initialization
	int i;
	for(i=0;i< coll->children_cnt; i++){
		if( nodeid == coll->ch_nodeids[i] ){
			return i;
		}
	}
	return -1;
}

void _progress_fan_in(pmixp_coll_t *coll)
{
	pmixp_srv_cmd_t type;
	const char *addr = pmixp_info_srv_addr();
	char *hostlist = NULL;
	int rc;

	PMIXP_DEBUG("%s:%d: start, local=%d, child_cntr=%d",
		    pmixp_info_namespace(), pmixp_info_nodeid(),
		    coll->contrib_local, coll->contrib_cntr);

	/* lock the collective*/
	pthread_mutex_lock(&coll->lock);

	pmixp_coll_sanity_check(coll);

	if( PMIXP_COLL_FAN_IN != coll->state ){
		/* In case of race condition between libpmix and
		 * slurm threads progress_fan_in can be called
		 * after we moved to the next step. */
		goto unlock;
	}

	if( !coll->contrib_local || coll->contrib_cntr != coll->children_cnt ){
		/* Not yet ready to go to the next step */
		goto unlock;
	}

	/* The root of the collective will have parent_host == NULL */
	if( NULL != coll->parent_host ){
		hostlist = xstrdup(coll->parent_host);
		type = PMIXP_MSG_FAN_IN;
	} else {
		hostlist = hostlist_ranged_string_xmalloc(coll->all_children);
		type = PMIXP_MSG_FAN_OUT;
	}

	PMIXP_DEBUG("%s:%d: send data to %s",
		    pmixp_info_namespace(), pmixp_info_nodeid(),
		    hostlist);

	rc = pmixp_server_send(hostlist, type, coll->seq, addr,
			       get_buf_data(coll->buf), get_buf_offset(coll->buf));
	xfree(hostlist);

	if( SLURM_SUCCESS != rc ){
		PMIXP_ERROR("Cannot send database (size = %lu), to hostlist:\n%s",
			    (uint64_t)get_buf_offset(coll->buf), hostlist);
		// TODO: abort the whole application here?
		// Currently it will just hang!
		goto unlock;
	}

	coll->state = PMIXP_COLL_FAN_OUT;

	/* reset the old buffer */
	set_buf_offset(coll->buf, 0);

	PMIXP_DEBUG("%s:%d: switch to PMIXP_COLL_FAN_OUT state",
		    pmixp_info_namespace(), pmixp_info_nodeid() );
unlock:
	/* lock the */
	pthread_mutex_unlock(&coll->lock);
}

void pmixp_coll_fan_out_data(pmixp_coll_t *coll, Buf buf)
{
	PMIXP_DEBUG("%s:%d: start", pmixp_info_namespace(), pmixp_info_nodeid());

	/* lock the structure */
	pthread_mutex_lock(&coll->lock);

	pmixp_coll_sanity_check(coll);

	xassert( PMIXP_COLL_FAN_OUT == coll->state );

	// update the database
	if( NULL != coll->cbfunc ){
		void *data = get_buf_data(buf) + get_buf_offset(buf);
		size_t size = remaining_buf(buf);
		PMIXP_DEBUG("%s:%d: use the callback",
			    pmixp_info_namespace(), pmixp_info_nodeid());
		coll->cbfunc(PMIX_SUCCESS, data, size, coll->cbdata, pmixp_free_Buf, (void*)buf);
	}

	// Prepare for the next collective operation
	_reset_coll(coll);

	PMIXP_DEBUG("%s:%d: collective is prepared for the next use",
		    pmixp_info_namespace(), pmixp_info_nodeid());

	/* unlock the structure */
	pthread_mutex_unlock(&coll->lock);
}

void pmixp_coll_reset_if_to(pmixp_coll_t *coll, time_t ts)
{
	/* lock the */
	pthread_mutex_lock(&coll->lock);

	if( PMIXP_COLL_SYNC == coll->state ){
		goto unlock;
	}

	if( ts - coll->ts > pmixp_info_timeout() ){
		/* respond to the libpmix */
		coll->cbfunc(PMIX_ERR_TIMEOUT, NULL, 0, coll->cbdata, NULL, NULL);
		/* drop the collective */
		_reset_coll(coll);
		/* report the timeout event */
		PMIXP_ERROR("Collective timeout!");
	}
unlock:
	/* unlock the structure */
	pthread_mutex_unlock(&coll->lock);
}

