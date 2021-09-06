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
#include <sys/epoll.h>
#include <sys/signalfd.h>

#include <signal.h>

#define BIN_PATH_LEN	254
#define UKVM_BIN	"/home/cmainas/workspace/fpga_uni/funky-solo5/ukvm/ukvm-bin"

static uint64_t write_file_n(uint8_t *buf, off_t size)
{
	int fd;
	off_t offset = 0;
	char file[BIN_PATH_LEN];
	size_t n;
	static uint64_t ref = 0;

	//sprintf(file, "/tmp/binary.ukvm", ref);
	sprintf(file, "/tmp/binary.ukvm");
	fd = open(file, O_WRONLY | O_CREAT, S_IRUSR | S_IWUSR);
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

	close(fd);
	return ref;
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

static int handle_epoll(int epollfd, int socket, int sigfd)
{
	int rc;
	int epoll_ret, i;
	struct epoll_event events[2];
	off_t bin_size;
	uint8_t *buf;

	epoll_ret = epoll_wait(epollfd, events, 2, -1);
	if (epoll_ret == -1) {
		perror("epoll wait");
		return -1;;
	}

	for (i = 0; i < epoll_ret; i++) {
		if (events[i].data.fd == socket) {
			
			rc = read(socket, &bin_size, sizeof(off_t));
			if (rc < 0) {
				perror("Read binary size\n");
				return -1;
			} else if (rc == 0) {
				fprintf(stderr, "Lost connection with primary scheduler\n");
				return -1;
			} else if (rc < sizeof(off_t)) {
				fprintf(stderr, "Short read on binary size\n");
				return -1;
			}
			printf("size of binary is %ld\n", bin_size);

			buf = read_file_n(socket, bin_size);
			if (!buf)
				return -1;

			rc = write_file_n(buf, bin_size);
			free(buf);
			if (rc < 0)
				return -1;
			return 1;
		} else if (events[i].data.fd == sigfd) {
			struct signalfd_siginfo sinfo;

			rc = read(sigfd, &sinfo, sizeof(struct signalfd_siginfo));
			if (rc < 0) {
				perror("Read signalfd\n");
				return -1;
			} else if (rc < sizeof(struct signalfd_siginfo)) {
				fprintf(stderr, "Short read on signalfd\n");
				return -1;
			}
			if (sinfo.ssi_signo == SIGCHLD) {
				printf("My child died? with code %d\n", sinfo.ssi_status);
				if (sinfo.ssi_status == 0)
					return 4;
				else return 5;
			}

		}
	}
	return 0;
}

static int start_guest()
{
	int pid;

	pid  = fork();
	if (pid == 0) {
		/*
		 * child
		 */
		int fd;
		char *const  e_args[] = {UKVM_BIN, "--net=tap0", "--disk=/tmp/binary.ukvm", "/tmp/binary.ukvm", NULL};

		fd = open("/tmp/guest_output", O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
		if (fd < 0) {
			perror("Opening redirected file\n");
			exit(1);
		}
		if (dup2(fd, 1) < 0) {
			perror("Redirecting stdout\n");
			close(fd);
			exit(1);
		}
		if (dup2(fd, 2) < 0) {
			perror("Redirecting stderr\n");
			close(fd);
			exit(1);
		}
		execv(UKVM_BIN, e_args);
	} else if (pid == -1) {
		perror("fork");
		return -1;
	}

	return pid;
}

static int send_deploy_res(int socket, int res)
{
	ssize_t rc;

	rc = write(socket, &res, sizeof(int));
	if (rc < 0) {
		perror("Send result to primary\n");
		return -1;
	} else if (rc < sizeof(int)) {
		fprintf(stderr, "Short write on execution result\n");
		return -1;
	}
	return 0;
}

int main(int argc, char *argv[])
{
	int rc, sched_sock, port = 0, epollfd, sfd;
	struct sockaddr_in sched_sockaddr;
	char *ip_addr = NULL;
	struct epoll_event ev;
	sigset_t schld_set;

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

	epollfd = epoll_create1(0);
	if (epollfd == -1) {
		perror("epoll_create1");
		goto out_soc;
	}

	ev.events = EPOLLIN;
	ev.data.fd = sched_sock;
	if (epoll_ctl(epollfd, EPOLL_CTL_ADD, sched_sock, &ev) == -1) {
		perror("epoll_ctl: socket for scheduler");
		goto out_pol;
	}

	sigemptyset(&schld_set);
	sigaddset(&schld_set, SIGCHLD);
	if (sigprocmask(SIG_BLOCK, &schld_set, NULL) == -1) {
		perror("Change handling of SIGCHLD");
		goto out_pol;
	}

	sfd = signalfd(-1, &schld_set, 0);
	if (sfd < 0) {
		perror("Create signalfd");
		goto out_pol;
	}
	ev.events = EPOLLIN;
	ev.data.fd = sfd;
	if (epoll_ctl(epollfd, EPOLL_CTL_ADD, sfd, &ev) == -1) {
		perror("epoll_ctl: socket for scheduler");
		goto out_pol;
	}

	while(1)
	{
		rc = handle_epoll(epollfd, sched_sock, sfd);
		if (rc < 0)
			goto out_pol;
		else if (rc == 1) {
			if (start_guest() < 0)
				goto out_pol;
			printf("Started ukvm with guest\n");
		} else {
			if (send_deploy_res(sched_sock, rc) , 0)
				goto out_pol;
		}

	}

	return 0;

out_pol:
	close(epollfd);
out_soc:
	close(sched_sock);
	exit(EXIT_FAILURE);
}
