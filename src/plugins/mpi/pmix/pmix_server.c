/*****************************************************************************\
 **  pmix_server.c - PMIx server side functionality
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

#include "pmix_common.h"
#include "pmix_info.h"
#include "pmix_coll.h"
#include "pmix_debug.h"
#include "pmix_io.h"
#include "pmix_client.h"
#include "pmix_server.h"
#include "pmix_db.h"
#include "pmix_state.h"
#include "pmix_client.h"

#define PMIX_SERVER_MSG_MAGIC 0xdeadbeef
typedef struct {
	uint32_t magic;
	uint32_t gen;
	uint32_t nodeid;
	uint32_t paysize;
	uint8_t cmd;
} send_header_t;
#define SEND_HDR_SIZE (4*sizeof(uint32_t) + sizeof(uint8_t))

typedef struct {
	uint32_t size; // Has to be first (appended by SLURM API)
	send_header_t send_hdr;
} recv_header_t;
#define RCVD_HDR_SIZE (sizeof(uint32_t) + SEND_HDR_SIZE)

static uint32_t _recv_payload_size(void *buf);
static int _send_pack_hdr(void *host, void *net);
static int _recv_unpack_hdr(void *net, void *host);

static bool _serv_readable(eio_obj_t *obj);
static int _serv_read(eio_obj_t *obj, List objs);
static void _process_server_request(recv_header_t *_hdr, void *payload);

void _dmdx_reply_to_node(uint32_t localid, uint32_t nodeid);
void _process_dmdx_request(send_header_t *hdr, void *payload);
int _dmdx_response(send_header_t *hdr, void *payload);

static struct io_operations peer_ops = {
	.readable     = _serv_readable,
	.handle_read  = _serv_read
};

pmix_io_engine_header_t srv_rcvd_header = {
	.host_size = sizeof(recv_header_t),
	.net_size = RCVD_HDR_SIZE,
	.pack_hdr_cb = NULL,
	.unpack_hdr_cb = _recv_unpack_hdr,
	.pay_size_cb = _recv_payload_size
};

int pmix_stepd_init(const stepd_step_rec_t *job, char ***env)
{
	char path[MAX_USOCK_PATH];
	int fd, rc;


	// Create UNIX socket for slurmd communication
	sprintf(path, PMIX_STEPD_ADDR_FMT, job->jobid, job->stepid );
	if( (fd = pmix_usock_create_srv(path)) < 0 ){
		return SLURM_ERROR;
	}
	pmix_info_server_contacts_set(path, fd);

	// Create UNIX socket for client communication
	sprintf(path, PMIX_CLI_ADDR_FMT, job->jobid, job->stepid );
	if( (fd = pmix_usock_create_srv(path)) < 0 ){
		close( pmix_info_srv_fd() );
		return SLURM_ERROR;
	}
	pmix_info_cli_contacts_set(path, fd);

	if( ( rc = pmix_info_job_set_stepd(job, env) ) ){
		return rc;
	}
	if( ( rc = pmix_coll_init(env) ) ){
		return rc;
	}
	return SLURM_SUCCESS;
}

int pmix_srun_init(const mpi_plugin_client_info_t *job, char ***env)
{
	char path[MAX_USOCK_PATH];
	int fd, rc;
	uint16_t port;

	if (net_stream_listen(&fd, &port) < 0) {
		PMIX_ERROR("Failed to create tree socket");
		return SLURM_ERROR;
	}
	sprintf(path, PMIX_STEPD_ADDR_FMT, job->jobid, job->stepid );
	pmix_info_server_contacts_set(path, fd);
	PMIX_DEBUG("srun pmi port: %hu", port);
	env_array_overwrite_fmt(env, PMIX_SRUN_PORT_ENV, "%hu", port);
	pmix_info_job_set_srun(job, env);
	if( ( rc = pmix_coll_init(env) ) ){
		return rc;
	}
	return SLURM_SUCCESS;
}

void pmix_server_new_conn(int fd)
{
	eio_obj_t *obj;
	PMIX_DEBUG("Request from fd = %d", fd);

	// Set nonblocking
	fd_set_nonblocking(fd);
	fd_set_close_on_exec(fd);

	pmix_io_engine_t *me = xmalloc( sizeof(pmix_io_engine_t) );
	pmix_io_init(me, fd, srv_rcvd_header);
	if( pmix_info_is_stepd() ){
		// We use slurm_forward_data to send message to stepd's
		// SLURM will put user ID there. We need to skip it
		pmix_io_rcvd_padding(me, sizeof(uint32_t));
	}
	// TODO: in future try to process the request right here
	// use eio only in case of blocking operation
	// NOW: always defer to debug the blocking case
	obj = eio_obj_create(fd, &peer_ops, (void*)me);
	eio_new_obj(pmix_info_io(), obj);
}

/*
 *  Server message processing
 */


static uint32_t _recv_payload_size(void *buf)
{
	recv_header_t *ptr = (recv_header_t*)buf;
	send_header_t *hdr = &ptr->send_hdr;
	xassert( ptr->size == (SEND_HDR_SIZE + hdr->paysize) );
	xassert( hdr->magic == PMIX_SERVER_MSG_MAGIC );
	return hdr->paysize;
}

/*
 * Pack message header.
 * Returns packed size
 * Note: asymmetric to _recv_unpack_hdr because of additional SLURM header
 */
static int _send_pack_hdr(void *host, void *net)
{
	send_header_t *ptr = (send_header_t*)host;
	Buf packbuf = create_buf(net, sizeof(send_header_t));
	int size = 0;
	pack32(ptr->magic, packbuf);
	pack32(ptr->gen, packbuf);
	pack32(ptr->nodeid, packbuf);
	pack32(ptr->paysize, packbuf);
	pack8(ptr->cmd, packbuf);
	size = get_buf_offset(packbuf);
	xassert( size == (4*sizeof(uint32_t) + sizeof(uint8_t)) );
	// free the Buf packbuf, but not the memory to which it points
	packbuf->head = NULL;
	free_buf(packbuf);
	return size;
}

/*
 * Unpack message header.
 * Returns 0 on success and -errno on failure
 * Note: asymmetric to _send_pack_hdr because of additional SLURM header
 */
static int _recv_unpack_hdr(void *net, void *host)
{
	recv_header_t *ptr = (recv_header_t*)host;
	Buf packbuf = create_buf(net, sizeof(recv_header_t));
	if( unpack32(&ptr->size, packbuf) ){
		return -EINVAL;
	}
	if( unpack32(&ptr->send_hdr.magic, packbuf)){
		return -EINVAL;
	}
	xassert( ptr->send_hdr.magic == PMIX_SERVER_MSG_MAGIC );

	if( unpack32(&ptr->send_hdr.gen, packbuf)){
		return -EINVAL;
	}

	if( unpack32(&ptr->send_hdr.nodeid, packbuf)){
		return -EINVAL;
	}

	if( unpack32(&ptr->send_hdr.paysize, packbuf) ){
		return -EINVAL;
	}

	if( unpack8(&ptr->send_hdr.cmd, packbuf) ){
		return -EINVAL;
	}

	// free the Buf packbuf, but not the memory to which it points
	packbuf->head = NULL;
	free_buf(packbuf);
	return 0;
}

void *pmix_server_alloc_msg(uint32_t size, void **payload)
{
	uint32_t payload_offs = sizeof(send_header_t) + SEND_HDR_SIZE;
	// Allocate more space than need to save unpacked header too
	void *msg = xmalloc(payload_offs + size);
	send_header_t *hdr = msg;

	hdr->magic = PMIX_SERVER_MSG_MAGIC;
	hdr->gen = pmix_state_data_gen();
	hdr->nodeid = pmix_info_nodeid();
	hdr->paysize = size;
	*payload = (char*)msg + payload_offs;
	return msg;
}

void pmix_server_msg_setcmd(void *msg, pmix_srv_cmd_t cmd)
{
	xassert(msg != NULL);
	send_header_t *hdr = (send_header_t*)msg;
	hdr->cmd = cmd;
}

void pmix_server_msg_finalize(void *msg)
{
	send_header_t *uhdr = (send_header_t *)msg;
	void *phdr = (void*)(uhdr + 1);
	_send_pack_hdr(uhdr, phdr);
}

uint32_t pmix_server_msg_size(void *msg)
{
	xassert(msg != NULL);
	send_header_t *hdr = (send_header_t*)msg;
	return hdr->paysize + SEND_HDR_SIZE;
}

void *pmix_server_msg_start(void *msg)
{
	xassert(msg != NULL);
	send_header_t *uhdr = msg;
	return (void*)(uhdr + 1);
}

static bool _serv_readable(eio_obj_t *obj)
{
	// TEMP
	return !obj->shutdown;

	// We should delete connection right when it
	// was closed or failed
	xassert( obj->shutdown == false );
	return true;
}

static void _process_server_request(recv_header_t *_hdr, void *payload)
{
	send_header_t *hdr = &_hdr->send_hdr;
	switch( hdr->cmd ){
	case PMIX_FENCE:
		pmix_coll_node_contrib(hdr->gen, hdr->nodeid, payload, hdr->paysize);
		// keep the payload!
		return;
	case PMIX_FENCE_RESP:
		// Do not update DB if we are in direct modex mode.
		if( false == pmix_info_dmdx()){
			pmix_coll_update_db(payload, hdr->paysize);
		}
		pmix_client_fence_notify();
		break;
	case PMIX_DIRECT:
		_process_dmdx_request(hdr, payload);
		break;
	case PMIX_DIRECT_RESP:
		_dmdx_response(hdr, payload);
	default:
		PMIX_ERROR_NO(0,"Bad command %d", hdr->cmd);
	}
	xfree(payload);
}

static int _serv_read(eio_obj_t *obj, List objs)
{

	PMIX_DEBUG("fd = %d", obj->fd);
	pmix_io_engine_t *me = (pmix_io_engine_t *)obj->arg;

//	pmix_debug_hang(1);

	// Read and process all received messages
	while( 1 ){
		pmix_io_rcvd(me);
		if( pmix_io_finalized(me) ){
			obj->shutdown = true;
			//pmix_debug_hang(1);
			PMIX_DEBUG("Connection finalized fd = %d", obj->fd);
			eio_remove_obj(obj, objs);
			return 0;
		}
		if( pmix_io_rcvd_ready(me) ){
			recv_header_t hdr;
			void *msg = pmix_io_rcvd_extract(me, &hdr);
			_process_server_request(&hdr, msg);
		}else{
			// No more complete messages
			break;
		}
	}
	return 0;
}

void pmix_server_dmdx_request(uint32_t taskid)
{
	int size = sizeof(uint32_t);
	void *msg, *payload, *start;
	char *host;
	uint32_t nodeid;
	Buf packbuf;

	msg = pmix_server_alloc_msg(size, &payload);
	packbuf = create_buf(payload, size);
	pack32(taskid, packbuf);
	packbuf->head = NULL;
	free_buf(packbuf);
	pmix_server_msg_setcmd(msg, PMIX_DIRECT);
	pmix_server_msg_finalize(msg);
	nodeid = pmix_info_task_node(taskid);
	host = pmix_info_nth_host_name(nodeid);
	size = pmix_server_msg_size(msg);
	start = pmix_server_msg_start(msg);
	// TODO: Try to send several times in case of error!
	slurm_forward_data(host, (char*)pmix_info_srv_addr(), size, start);
	xfree(host);
	xfree(msg);
}

void pmix_server_dmdx_notify(uint32_t localid)
{
	uint32_t *nodeid;
	List requests;
	if( !pmix_state_remote_reqs_to_cnt(localid) ){
		// there was no requests to this task
		return;
	}
	requests = pmix_state_remote_reqs_to(localid);
	while( (nodeid = list_dequeue(requests)) ){
		_dmdx_reply_to_node(localid, *nodeid);
		xfree(nodeid);
	}

}

void _dmdx_reply_to_node(uint32_t localid, uint32_t nodeid)
{
	int size;
	void *blob, *msg, *start, *payload;
	char *host;
	uint32_t taskid, gen;

	taskid = pmix_info_task_id(localid);

	size = pmix_db_get_blob(taskid, &blob, &gen);
	if( !gen || gen < pmix_state_data_gen() ){
		// We didn't have information about this task or
		// the data is stalled
		pmix_state_defer_remote_req(localid, nodeid);
		return;
	}
	msg = pmix_server_alloc_msg(size + sizeof(uint32_t), &payload);
	*(uint32_t*)payload = taskid;
	memcpy((uint32_t*)payload + 1, blob, size);
	// Convert the header into network byte order
	pmix_server_msg_setcmd(msg, PMIX_DIRECT_RESP);
	pmix_server_msg_finalize(msg);
	host = pmix_info_nth_host_name(nodeid);
	size = pmix_server_msg_size(msg);
	start = pmix_server_msg_start(msg);
	// TODO: Try to send several times in case of error!
	slurm_forward_data(host, (char*)pmix_info_srv_addr(), size, start);
	xfree(host);
}

void _process_dmdx_request(send_header_t *hdr, void *payload)
{
	uint32_t taskid;
	uint32_t localid;

	Buf packbuf = create_buf(payload, hdr->paysize);
	if( unpack32(&taskid, packbuf) ){
		PMIX_ERROR_NO(EINVAL,"DMDX: Cannot unpack requested global task id");
		// TODO: respond with the error
		return;
	}

	if( (localid = pmix_info_lid2gid(taskid)) < 0 ){
		PMIX_ERROR_NO(ENOENT,"DMDX: Cannot find requested global task ID on this node");
		// TODO: respond with the error
		return;
	}
	_dmdx_reply_to_node(localid, hdr->nodeid);
}

int _dmdx_response(send_header_t *hdr, void *payload)
{
	Buf packbuf = create_buf(payload, hdr->paysize);
	void *blob, *blob_copy;
	uint32_t taskid;
	int size;

	if( unpack32(&taskid, packbuf) ){
		PMIX_ERROR_NO(EINVAL,"DMDX response: Cannut unpack task global id");
		return SLURM_ERROR;
	}
	blob = (char*)payload + get_buf_offset(packbuf);
	size = remaining_buf(packbuf);
	blob_copy = xmalloc( size );
	memcpy(blob_copy, blob, size);
	pmix_db_add_blob(taskid,blob_copy,size);
	// Reply to the clients
	pmix_client_taskid_reply(taskid);
	return SLURM_SUCCESS;
}