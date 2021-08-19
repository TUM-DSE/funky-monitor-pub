#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>

#define FRONT_SOCK "/tmp/front.sock"
#define BIN_PATH_LEN 24

static void *frontend_link()
{
	struct sockaddr_un front_sockaddr;
	int rc, front_sock;

	front_sock = socket(AF_UNIX, SOCK_STREAM, 0);
    	if (front_sock == -1) {
		fprintf(stderr, "socket error %d\n", errno);
    	}

    	front_sockaddr.sun_family = AF_UNIX;
    	strcpy(front_sockaddr.sun_path, FRONT_SOCK);
    	unlink(FRONT_SOCK);
    	rc = bind(front_sock, (struct sockaddr *) &front_sockaddr,
		  sizeof(struct sockaddr_un));
    	if (rc == -1) {
		perror("socket bind");
		rc = errno;
		close(front_sock);
		fprintf(stderr, "Socket bind error: %d\n", rc);
    	}

    	rc = listen(front_sock, 5);
    	if (rc == -1) {
		rc = errno;
		close(front_sock);
    		fprintf(stderr, "Socket listen error: %d\n", rc);
    	}

    	printf("listesning commands at %s\n", FRONT_SOCK);
    	for (;;) {
		int con;
		char bin_path[BIN_PATH_LEN];

    		con = accept(front_sock, NULL, NULL);
    		if (con == -1) {
			rc = errno;
    			close(front_sock);
    			fprintf(stderr, "Socket accept error: %d\n", rc);
    		}

    		rc = read(con, bin_path, BIN_PATH_LEN);
    		if (rc <= 0) {
    		        close(con);
    		        fprintf(stderr, "Read from peer failed");
    		}
    		/* Make sure we do not read rubbish, old commands etc */
    		bin_path[(rc > BIN_PATH_LEN) ? BIN_PATH_LEN - 1: rc] = '\0';

    		printf("New binary %s\n");;
    		close(con);
    	}
}

int main()
{
	pthread_t frontend_thr;
	int rc;

	rc = pthread_create(&frontend_thr, NULL, frontend_link, NULL);
	if (rc)
		fprintf(stderr, "Could not create fontend thread %d\n", rc);

	while(1)
	{
	}
}

