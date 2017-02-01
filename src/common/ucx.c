#include "ucx.h"

unsigned long my_ucx_addr_len;
char *my_ucx_addr;
char my_ucx_hostname[1024];
int  my_slurm_ucx_type;
/* UCP handler objects */
ucp_context_h ucp_context;
ucp_worker_h ucp_worker;

struct slurm_ucx_ep_table _slurm_ucx_ep_table[SLURM_UCX_EP_LIST_LEN];
struct slurm_ucx_sever_conn_cache_item *_slurm_ucx_sever_conn_cache_list;


struct ucx_context {
	int completed;
};

struct ucx_context request;

static void request_init(void *request)
{
	struct ucx_context *ctx = (struct ucx_context *) request;
	ctx->completed = 0;
}

static void send_handle(void *request, ucs_status_t status)
{
	struct ucx_context *context = (struct ucx_context *) request;
	context->completed = 1;
}

static void recv_handle(void *request, ucs_status_t status,
			ucp_tag_recv_info_t *info)
{
	struct ucx_context *context = (struct ucx_context *) request;
	context->completed = 1;
}


static int slurm_ucx_init()
{
	ucp_config_t *config;
	ucs_status_t status;
	ucp_params_t ucp_params;
	ucp_worker_params_t worker_params;
	ucp_address_t *local_addr;
	unsigned long local_addr_len;
	int i;


	status = ucp_config_read(NULL, NULL, &config);
	if (status != UCS_OK) {
		fprintf(stderr, "Failed ucp_config_read\n");
		exit(-1);
	}

	ucp_params.features = UCP_FEATURE_TAG | UCP_FEATURE_WAKEUP;
	ucp_params.request_size    = sizeof(struct ucx_context);
	ucp_params.request_init    = request_init;
	ucp_params.request_cleanup = NULL;
	ucp_params.field_mask      = UCP_PARAM_FIELD_FEATURES |
		UCP_PARAM_FIELD_REQUEST_SIZE |
		UCP_PARAM_FIELD_REQUEST_INIT |
		UCP_PARAM_FIELD_REQUEST_CLEANUP;

	status = ucp_init(&ucp_params, config, &ucp_context);
	ucp_config_release(config);
	if (status != UCS_OK) {
		fprintf(stderr, "Failed ucp_config_release\n");
		exit(-1);
	}

	worker_params.field_mask  = UCP_WORKER_PARAM_FIELD_THREAD_MODE;
	worker_params.thread_mode = UCS_THREAD_MODE_SINGLE;

	status = ucp_worker_create(ucp_context, &worker_params, &ucp_worker);
	if (status != UCS_OK) {
		fprintf(stderr, "ucp_worker_create Failed\n");
		exit(-1);
	}

	status = ucp_worker_get_address(ucp_worker, &local_addr, &local_addr_len);
	if (status != UCS_OK) {
		fprintf(stderr, "ucp_worker_get_address  Failed\n");
		exit(-1);
	}
	my_ucx_addr_len = local_addr_len;
	my_ucx_addr = (char *) local_addr;
	gethostname(my_ucx_hostname, sizeof(my_ucx_hostname));


	for (i = 0; i < SLURM_UCX_EP_LIST_LEN; i++) {
		_slurm_ucx_ep_table[i].id = i;
		_slurm_ucx_ep_table[i].status = SLURM_UCX_EP_FREE;
		_slurm_ucx_ep_table[i].ep = 0;
	}
	_slurm_ucx_sever_conn_cache_list = NULL;

	return SLURM_UCX_SUCCESS;

}

void slurm_ucx_cleanup()
{
	ucp_worker_release_address(ucp_worker, (ucp_address_t *)my_ucx_addr);
	ucp_worker_destroy(ucp_worker);
	ucp_cleanup(ucp_context);
}

static int slurm_ucx_save_server_address()
{

	char fname[256];
	FILE *f;
	int i;


	sprintf(fname, "/tmp/slurm-ucx-%s.txt", my_ucx_hostname);
	f = fopen(fname, "w");
	if (f == NULL)
	{
		printf("Error opening file!\n");
		exit(1);
	}

	fprintf(f, "%s\n",my_ucx_hostname);
	fprintf(f, "%d\n",my_ucx_addr_len);
	for (i = 0; i <my_ucx_addr_len; i++)
		fprintf(f, "%hhx ", my_ucx_addr[i]);
	fprintf(f, "\n");

	fclose(f);
}

static int slurm_ucx_get_server_address(char *server_name, char **server_addr, int *server_addr_len)
{
	char fname[256];
	char sname[256];
	char *saddr;
	FILE *f;
	int i, rc;

	sprintf(fname, "/tmp/slurm-ucx-%s.txt", server_name);
	f = fopen(fname, "r");
	if (f == NULL)
	{
		printf("Error opening file!\n");
		return SLURM_UCX_ERROR;
	}

	printf("Opening file: %s \n", fname);

	fscanf(f, "%s\n", sname);
	fscanf(f, "%d\n", server_addr_len);
	printf("server name:%s addr_len:%d \n", sname, *server_addr_len);

	saddr = malloc (*server_addr_len);
	printf("server addr :  ");
	for (i = 0; i < *server_addr_len; i++) {
		fscanf(f, "%hhx", &saddr[i]);
		fprintf(stdout, "%hhx ", saddr[i]);
	}
	fprintf(stdout, "\n");
	fclose(f);

	*server_addr = saddr;

	return SLURM_UCX_SUCCESS;

}

int slurm_ucx_reachable(char *sname)
{
	char fname[256];
	FILE *f;

	sprintf(fname, "/tmp/slurm-ucx-%s.txt", sname);
	f = fopen(fname, "r");
	if (f == NULL) {
		return 0;
	}

	fclose(f);

	return 1;
}

int slurm_ucx_init_server()
{
	my_slurm_ucx_type = SLURM_UCX_SERVER;
	slurm_ucx_init();

	/* save server addr to file */
	slurm_ucx_save_server_address();

	return SLURM_UCX_SUCCESS;

}


int slurm_ucx_init_client()
{
	my_slurm_ucx_type = SLURM_UCX_CLIENT;
	slurm_ucx_init();
	return SLURM_UCX_SUCCESS;
}

static struct slurm_ucx_sever_conn_cache_item *find_conn_from_cached_list(char *name)
{
	struct slurm_ucx_sever_conn_cache_item *item = _slurm_ucx_sever_conn_cache_list;
	while(item != NULL) {
		if (strcmp(item->sname, name) == 0) {
			break;
		}
		item = item->next;
	}
	return item;

}

int slurm_ucx_open_conn_address(char *saddr, int addr_len)
{
	ucp_ep_params_t ep_params;
	ucp_ep_h server_ep;
	char *saddr;
	int slen;
	int rc, fd;

	item = find_conn_from_cached_list(name);
	if (item == NULL) {
		rc = slurm_ucx_get_server_address(name, &saddr, &slen);
		if (rc != SLURM_UCX_SUCCESS) {
			return SLURM_UCX_ERROR;
		}

		ep_params.field_mask = UCP_EP_PARAM_FIELD_REMOTE_ADDRESS;
		ep_params.address    = (ucp_address_t *) saddr;

		rc = ucp_ep_create(ucp_worker, &ep_params, &server_ep);
		if (rc != UCS_OK) {
			return SLURM_UCX_ERROR;
		}

		printf ("connection is opened to :%s\n", name);

		/* add connection to the cached list */
		item = (struct slurm_ucx_sever_conn_cache_item *) malloc(sizeof(struct slurm_ucx_sever_conn_cache_item));
		strcpy(item->sname, name);
		item->ep = server_ep;
		item->next = _slurm_ucx_sever_conn_cache_list;
		_slurm_ucx_sever_conn_cache_list = item;
	} else {
		printf(" connection found in the cache\n");
	}

	/* assign ID to the endpoint */
	for (fd = 0; fd < SLURM_UCX_EP_LIST_LEN; fd++) {
		if (_slurm_ucx_ep_table[fd].status == SLURM_UCX_EP_FREE)
			break;
	}

	if (fd >= SLURM_UCX_EP_LIST_LEN) {
		printf("No free ep slot avaliable \n");
		return SLURM_UCX_ERROR;
	}

	_slurm_ucx_ep_table[fd].status = SLURM_UCX_EP_IN_USE;
	_slurm_ucx_ep_table[fd].ep = item->ep;

int slurm_ucx_open_connection(char *name)
{
	ucp_ep_params_t ep_params;
	ucp_ep_h server_ep;
	char *saddr;
	int slen;
	int rc, fd;
	struct slurm_ucx_sever_conn_cache_item *item;

	item = find_conn_from_cached_list(name);
	if (item == NULL) {
		rc = slurm_ucx_get_server_address(name, &saddr, &slen);
		if (rc != SLURM_UCX_SUCCESS) {
			return SLURM_UCX_ERROR;
		}

		ep_params.field_mask = UCP_EP_PARAM_FIELD_REMOTE_ADDRESS;
		ep_params.address    = (ucp_address_t *) saddr;

		rc = ucp_ep_create(ucp_worker, &ep_params, &server_ep);
		if (rc != UCS_OK) {
			return SLURM_UCX_ERROR;
		}

		printf ("connection is opened to :%s\n", name);

		/* add connection to the cached list */
		item = (struct slurm_ucx_sever_conn_cache_item *) malloc(sizeof(struct slurm_ucx_sever_conn_cache_item));
		strcpy(item->sname, name);
		item->ep = server_ep;
		item->next = _slurm_ucx_sever_conn_cache_list;
		_slurm_ucx_sever_conn_cache_list = item;
	} else {
		printf(" connection found in the cache\n");
	}

	/* assign ID to the endpoint */
	for (fd = 0; fd < SLURM_UCX_EP_LIST_LEN; fd++) {
		if (_slurm_ucx_ep_table[fd].status == SLURM_UCX_EP_FREE)
			break;
	}

	if (fd >= SLURM_UCX_EP_LIST_LEN) {
		printf("No free ep slot avaliable \n");
		return SLURM_UCX_ERROR;
	}

	_slurm_ucx_ep_table[fd].status = SLURM_UCX_EP_IN_USE;
	_slurm_ucx_ep_table[fd].ep = item->ep;

	return fd;

}


int slurm_ucx_close_connextion(int fd)
{
	_slurm_ucx_ep_table[fd].status = SLURM_UCX_EP_FREE;

}


int slurm_ucx_whoami()
{
	return my_slurm_ucx_type;
}


char*  slurm_ucx_hostname()
{
	return my_ucx_hostname;
}

unsigned long  slurm_ucx_addr_len()
{
	return my_ucx_addr_len;
}

char * slurm_ucx_get_addr()
{
	return  my_ucx_addr;
}

void slurm_ucx_progress()
{
	ucp_worker_progress(ucp_worker);
}


int slurm_ucx_send(int fd, void *buffer, size_t len)
{
	ucp_ep_h remote_ep;



}


