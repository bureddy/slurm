#include "ucx.h"
int main(int argc, char **argv)
{
	int server=1;
	char *server_name = 0;
	int fd1, fd2;

	if (argv[1] != NULL) {
		server = 0;
		server_name = argv[1];
	}

	if (server) {
		slurm_ucx_init_server();
	} else {
		slurm_ucx_init_client();
		fd1 = slurm_ucx_open_connection(server_name);
		if (fd1 >= 0) {
			printf("connection fd to server:%d\n", fd1);
		} else {
			printf("couldn't create connection to server\n");
		}

		fd2 = slurm_ucx_open_connextion(server_name);
		if (fd2 >= 0) {
			printf("connection fd to server:%d\n", fd2);
		} else {
			printf("couldn't create connection to server\n");
		}

		slurm_ucx_close_connextion(fd2);

	}

	while(1) {
		slurm_ucx_progress();
		usleep(100);
	}
	slurm_ucx_cleanup();


	return 0;


}

