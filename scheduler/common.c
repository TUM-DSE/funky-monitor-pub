#include <sys/epoll.h>
#include <sys/socket.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/un.h>
#include <errno.h>
#include <arpa/inet.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sendfile.h>

#include "common.h"

int setup_socket(int epollfd, struct sockaddr *saddr, uint8_t tobind)
{
	int sock, rc;

	sock = socket(saddr->sa_family, SOCK_STREAM, 0);
	if (sock == -1) {
		fprintf(stderr, "socket error %d\n", errno);
		return -1;;
	}

	if (saddr->sa_family == AF_UNIX) {
		rc = bind(sock, saddr, sizeof(struct sockaddr_un));
		if (!tobind)
			rc = connect(sock, saddr, sizeof(struct sockaddr_un));
	} else if (saddr->sa_family == AF_INET) {
		if (tobind)
			rc = bind(sock, saddr, sizeof(struct sockaddr_in));
		else
			rc = connect(sock, saddr, sizeof(struct sockaddr_in));
	} else {
		fprintf(stderr, "Invalid domain\n");
		goto err_set;
	}

	if (rc == -1) {
		perror("socket bind/connect");
		goto err_set;
	}

	if (tobind) {
		rc = listen(sock, MAX_EVENTS - 1);
		if (rc == -1) {
			perror("Socket listen");
			goto err_set;
		}
	}

	if (epollfd) {
		struct epoll_event ev;

		ev.events = EPOLLIN | EPOLLET;
		ev.data.fd = sock;
		if (epoll_ctl(epollfd, EPOLL_CTL_ADD, sock, &ev) == -1) {
			perror("epoll_ctl: frontend socket");
			goto err_set;
		}
	}

	return sock;

err_set:
	close(sock);
	return -1;
}

/*
 * Send a file over a socket
 */
int send_file(int socket, const char *filename, enum mnode_type msg_type)
{
	int rc = 0, fd;
	struct stat st;
	ssize_t n, count = 0;
	struct com_nod node_com;

	fd = open(filename, O_RDONLY);
	if (fd < 0) {
		perror("Opening file to send");
		return -1;
	}

	/*
	 * Get size of file
	 */
	rc = stat(filename, &st);
	if (rc < 0) {
		perror("Getting file size");
		goto err_send;
	}

	node_com.type = msg_type;
	node_com.size = st.st_size;
	rc = write(socket, &node_com, sizeof(struct com_nod));
	if (rc < sizeof(struct com_nod)) {
		if (rc < 0)
			perror("Sending file info");
		else
			err_print("Short send of file info\n");
		goto err_send;
	}

	/*
	 * Make sure the whole file is sent.
	 */
	while (count < st.st_size) {
		n = sendfile(socket, fd, NULL, st.st_size - count);
		if (n < 0) {
			perror("Sending file");
			goto err_send;
		}
		count += n;
	}
	return rc;

err_send:
	close(fd);
	return -1;
}
