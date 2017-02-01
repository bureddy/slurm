#include <stdio.h>
#include <poll.h>
#include <ucp/api/ucp.h>
#include <ucp/api/ucp_def.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>


#define SLURM_UCX_SUCCESS (0)
#define SLURM_UCX_ERROR (-1)


typedef enum slurm_ucx_type {
	SLURM_UCX_SERVER,
	SLURM_UCX_CLIENT,
} clurm_ucx_type_t;

enum slurm_ucx_ep_status {
	SLURM_UCX_EP_IN_USE,
	SLURM_UCX_EP_FREE,
};

#define SLURM_UCX_EP_LIST_LEN (1024)

struct slurm_ucx_ep_table {
	int id;
	int status;
	ucp_ep_h ep;
};

struct slurm_ucx_sever_conn_cache_item {
	char sname[256];
	ucp_ep_h ep;
	struct slurm_ucx_sever_conn_cache_item *next;
};


extern int errno;
#include <errno.h>

int slurm_ucx_init_server();
int slurm_ucx_init_client();
int slurm_ucx_open_connection(char *name);
int slurm_ucx_open_conn_address(char *saddr, int saddr_len);
int slurm_ucx_close_connection(int fd);
char *slurm_ucx_hostname();
int slurm_ucx_whoami();
unsigned long  slurm_ucx_addr_len();
char * slurm_ucx_get_addr();
int slurm_ucx_reachable(char *name);
void slurm_ucx_progress();
int slurm_ucx_send(int fd, void *buffer, size_t len);

