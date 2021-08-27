#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <getopt.h>

int main(int argc, char *argv[])
{
	int rc, sched_sock, port = 0;
	struct sockaddr_in sched_sockaddr;
	char *ip_addr = NULL;

	rc = getopt(argc, argv, "hi:p:");
	while (rc != -1) {
		switch(rc) {
		case 'h':
			printf("Usage: %s -i <ip_address> -p <port>\n", argv[0]);
			return 0;
		case 'i':
			ip_addr = optarg;
			break;
		case 'p':
			port = atoi(optarg);
			break;
		case '?':
			fprintf(stderr,"Unknown option %c\n", optopt);
			exit(EXIT_FAILURE);
		}
		rc = getopt(argc, argv, "hi:p:");
	}
	if (!port || !ip_addr) {
		printf("Usage: %s -i <ip_address> -p <port>\n", argv[0]);
		exit(EXIT_FAILURE);
	}
	
	sched_sock = socket(AF_INET, SOCK_STREAM, 0);
	if (sched_sock == -1) {
		fprintf(stderr, "socket error %d\n", errno);
	}

	rc = inet_pton(AF_INET, ip_addr, &sched_sockaddr.sin_addr);
	if (rc <=0) {
		fprintf(stderr, "Wrong IPv4 address format\n");
		exit(EXIT_FAILURE);
	}
	sched_sockaddr.sin_family = AF_INET;
	sched_sockaddr.sin_port = htons(port);
	rc = connect(sched_sock, (struct sockaddr *) &sched_sockaddr,
		  sizeof(struct sockaddr_in));
	if (rc == -1) {
		perror("socket connect");
		goto out_soc;
	}

	return 0;

out_soc:
	close(sched_sock);
	exit(EXIT_FAILURE);
}
