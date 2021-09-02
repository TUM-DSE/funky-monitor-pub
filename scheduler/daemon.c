#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <getopt.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#define BIN_PATH_LEN	254

static int write_file_n(uint8_t *buf, off_t size)
{
	int fd;
	off_t offset = 0;
	char file[BIN_PATH_LEN];
	size_t n;
	static int ref = 0;

	sprintf(file, "/tmp/binary_%d.ukvm", ref);
	fd = open(file, O_WRONLY | O_CREAT, 0644);
	if (fd < 0) {
		perror("Creating new file");
		return -1;
	}
	ref++;

	while (offset < size) {
		n = write(fd, buf + offset, size - offset);
		if (n < 0) {
			perror("Writing new file");
			close(fd);
			return -1;
		}
		offset += n;
	}

	return 0;
}

static uint8_t *read_file_n(int soc, off_t size)
{
	int n;
	uint8_t *buf = NULL;
	off_t offset = 0;

	buf = malloc(size);
	if (!buf) {
		fprintf(stderr, "Out of memory receive buf");
		return NULL;
	}

	while (offset < size) {
		n = read(soc, buf + offset, size - offset);
		if (n < 0) {
			perror("receiving binary");
			goto err_read_f;;
		} else if (n == 0) {
			fprintf(stderr, "Connection lost while receiving binary\n");
			goto err_read_f;
		}
		offset += n;
	}

	return buf;
err_read_f:
	free(buf);
	return NULL;
}

int main(int argc, char *argv[])
{
	int rc, sched_sock, port = 0;
	struct sockaddr_in sched_sockaddr;
	char *ip_addr = NULL;
	off_t bin_size;
	uint8_t *buf;

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

	rc = read(sched_sock, &bin_size, sizeof(off_t));
	if (rc == 0) {
		perror("Read binary size\n");
		goto out_soc;
	} else if (rc == 0) {
		fprintf(stderr, "Loast connection with primary scheduler\n");
		goto out_soc;
	} else if (rc < sizeof(off_t)) {
		fprintf(stderr, "Short read on binary size\n");
		goto out_soc;
	}
	printf("size of binary is %ld\n", bin_size);

	buf = read_file_n(sched_sock, bin_size);
	if (!buf)
		goto out_soc;

	rc = write_file_n(buf, bin_size);
	if (rc < 0)
		goto out_soc;
	return 0;

out_soc:
	close(sched_sock);
	exit(EXIT_FAILURE);
}
