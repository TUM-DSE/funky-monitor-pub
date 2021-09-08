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
#include <sys/un.h>
#include <sys/sendfile.h>

#include <signal.h>

#define BIN_PATH_LEN	254
#define UKVM_BIN	"/home/cmainas/workspace/fpga_uni/funky-solo5/ukvm/ukvm-bin"
#define UKVM_SOC	"/tmp/ukvm_socket"
#define PORT_NODES	1742

struct com_nod {
	uint8_t type;
	union {
		off_t size;
		struct in_addr rcv_ip;
	};
};

static struct in_addr to_node;

static uint64_t write_file_n(uint8_t *buf, off_t size, char *file)
{
	int fd;
	off_t offset = 0;
	size_t n;
	static uint64_t ref = 0;

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

static int start_migration()
{
	int sockfd;
	struct sockaddr_un addr;
	int rc;

	sockfd = socket(PF_UNIX, SOCK_STREAM, 0);
	if (sockfd < 0) {
		perror("Create socket");
		return -1;
	}

	memset(&addr, 0, sizeof(struct sockaddr_un));
	addr.sun_family = AF_UNIX;
	strcpy(addr.sun_path, UKVM_SOC);
	rc = connect(sockfd, (struct sockaddr *)&addr, sizeof(addr));
	if (rc < 0) {
		perror("Connect to ukvm socket");
		goto mig_fail;
	}

	rc = write(sockfd, "savevm /tmp/file.mig", 20);
	if (rc < 20) {
		if (rc < 0)
			perror("write to ukvm socket");
		else
			fprintf(stderr, "Short write to ukvm socket\n");
		goto mig_fail;
	}

	return 0;
mig_fail:
	close(sockfd);
	return -1;
}

static ssize_t receive_a_file(int socket, char *file)
{
	ssize_t rc;
	off_t file_sz;
	uint8_t *buf;

	rc = read(socket, &file_sz, sizeof(off_t));
	if (rc < 0) {
		perror("Read file\n");
		return -1;
	} else if (rc == 0) {
		fprintf(stderr, "Lost connection with sender\n");
		return -1;
	} else if (rc < sizeof(off_t)) {
		fprintf(stderr, "Short read on file size\n");
		return -1;
	}
	printf("size of binary is %ld\n", file_sz);

	buf = read_file_n(socket, file_sz);
	if (!buf)
		return -1;

	rc = write_file_n(buf, file_sz, file);
	free(buf);
	if (rc < 0)
		return -1;

	return 0;
}

static ssize_t send_binary(int socket, char *file)
{
	int fd;
	struct stat st;
	ssize_t n, count = 0;
	ssize_t rc = 0;

	fd = open(file, O_RDONLY);
	if (fd < 0) {
		perror("Opening file to send");
		return -1;
	}

	rc = stat(file, &st);
	if (rc < 0) {
		perror("binary size");
		goto err_send;
	}

	rc = write(socket, &st.st_size, sizeof(off_t));
	if (rc < sizeof(off_t)) {
		if (rc < 0)
			perror("Send file size");
		else
			fprintf(stderr, "Short send of binary size\n");
		goto err_send;
	}

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

static void send_migration()
{
	int sockfd, rc;
	struct sockaddr_in saddr;

	sockfd = socket(AF_INET, SOCK_STREAM, 0);
	if (sockfd == -1) {
		fprintf(stderr, "socket error %d\n", errno);
		return;
	}

	memcpy(&saddr.sin_addr, &to_node, sizeof(struct in_addr));
	saddr.sin_family = AF_INET;
	saddr.sin_port = htons(PORT_NODES);
	rc = connect(sockfd, (struct sockaddr *) &saddr,
		  sizeof(struct sockaddr_in));
	if (rc == -1) {
		perror("socket connect");
		close(sockfd);
		return;
	}

	rc = send_binary(sockfd, "/tmp/binary.ukvm");
	if (rc < 0)
		return;

	rc = send_binary(sockfd, "/tmp/file.mig");
	if (rc < 0)
		return;
}

static int handle_epoll(int epollfd, int socket, int sigfd, int server_soc)
{
	int rc;
	int epoll_ret, i;
	struct epoll_event events[2];
	uint8_t *buf;

	epoll_ret = epoll_wait(epollfd, events, 2, -1);
	if (epoll_ret == -1) {
		perror("epoll wait");
		return -1;;
	}

	for (i = 0; i < epoll_ret; i++) {
		if (events[i].data.fd == socket) {
			struct com_nod node_com;
			
			rc = read(socket, &node_com, sizeof(struct com_nod));
			if (rc < 0) {
				perror("Read message from primary\n");
				return -1;
			} else if (rc == 0) {
				fprintf(stderr, "Lost connection with primary scheduler\n");
				return -1;
			} else if (rc < sizeof(off_t)) {
				fprintf(stderr, "Short read on primary's message\n");
				return -1;
			}
			if (node_com.type == 0) {
				printf("size of binary is %ld\n", node_com.size);

				buf = read_file_n(socket, node_com.size);
				if (!buf)
					return -1;

				rc = write_file_n(buf, node_com.size, "/tmp/binary.ukvm");
				free(buf);
				if (rc < 0)
					return -1;
			} else if (node_com.type == 1) {
				char myIP[16];

				inet_ntop(AF_INET, &node_com.rcv_ip, myIP, sizeof(myIP));

				printf("I will have to migrate to %s\n", myIP);
				rc =start_migration();
				to_node = node_com.rcv_ip;
				if (rc < 0)
					return -1;
				return 2;
			}
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
				if (sinfo.ssi_status == 0) {
					return 4;
				} else if (sinfo.ssi_status == 7) {
					send_migration();
					return 7;
				} else {
					return 5;
				}
			}

		} else if (events[i].data.fd == server_soc) {
			int mig_soc;

			mig_soc = accept(server_soc, NULL, NULL);
			if (mig_soc == -1) {
				perror("frontend socket accept");
				return -1;
			}
			printf("Connection established\n");

			rc = receive_a_file(mig_soc, "/tmp/rcvd_file.ukvm");
			if (rc < 0)
				return -1;

			rc = receive_a_file(mig_soc, "/tmp/rcvd_file.mig");
			if (rc < 0)
				return -1;
			return 3;
		}
	}
	return 0;
}

static int start_guest(uint8_t mig)
{
	int pid;

	pid  = fork();
	if (pid == 0) {
		/*
		 * child
		 */
		int fd;

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
		if (!mig) {
			char *const  e_args[] = {UKVM_BIN, "--net=tap0", "--disk=/tmp/binary.ukvm", "--mon="UKVM_SOC, "/tmp/binary.ukvm", NULL};
			execv(UKVM_BIN, e_args);
		} else {
			char *const  e_args[] = {UKVM_BIN, "--net=tap0", "--disk=/tmp/binary.ukvm", "--mon="UKVM_SOC, "--load=/tmp/file.mig", "/tmp/binary.ukvm", NULL};
			execv(UKVM_BIN, e_args);
		}
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

static int setup_socket(int domain, int epollfd)
{
	int sock, rc;
	struct epoll_event ev;

	sock = socket(domain, SOCK_STREAM, 0);
	if (sock == -1) {
		fprintf(stderr, "socket error %d\n", errno);
		return -1;;
	}

	if (domain == AF_UNIX) {
		struct sockaddr_un saddr_un = {0};

		saddr_un.sun_family = AF_UNIX;
		strcpy(saddr_un.sun_path, UKVM_SOC);
		unlink(UKVM_SOC);
		rc = bind(sock, (struct sockaddr *) &saddr_un,
				sizeof(struct sockaddr_un));
	} else if (domain == AF_INET) {
		struct sockaddr_in saddr_in = {0};

		saddr_in.sin_family = AF_INET;
		saddr_in.sin_addr.s_addr = htonl(INADDR_ANY);
		saddr_in.sin_port = htons(PORT_NODES);
		rc = bind(sock, (struct sockaddr *) &saddr_in,
				sizeof(struct sockaddr_in));
	} else {
		fprintf(stderr, "Invalid domain\n");
		goto err_set;
	}

	if (rc == -1) {
		perror("socket bind");
		goto err_set;
	}

	rc = listen(sock, 2);
	if (rc == -1) {
		perror("Socket listen");
		goto err_set;
	}

	ev.events = EPOLLIN | EPOLLET;
	ev.data.fd = sock;
	if (epoll_ctl(epollfd, EPOLL_CTL_ADD, sock, &ev) == -1) {
		perror("epoll_ctl: frontend socket");
		goto err_set;
	}

	return sock;

err_set:
	close(sock);
	return -1;
}

int main(int argc, char *argv[])
{
	int rc, sched_sock, port = 0, epollfd, sfd, server_soc;
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

	server_soc = setup_socket(AF_INET, epollfd);
	if (server_soc < 0) {
		fprintf(stderr, "Could not setup_socket for inter node communication\n");
		goto out_pol;
	}

	sigemptyset(&schld_set);
	sigaddset(&schld_set, SIGCHLD);
	if (sigprocmask(SIG_BLOCK, &schld_set, NULL) == -1) {
		perror("Change handling of SIGCHLD");
		goto out_socs;
	}

	sfd = signalfd(-1, &schld_set, 0);
	if (sfd < 0) {
		perror("Create signalfd");
		goto out_socs;
	}
	ev.events = EPOLLIN;
	ev.data.fd = sfd;
	if (epoll_ctl(epollfd, EPOLL_CTL_ADD, sfd, &ev) == -1) {
		perror("epoll_ctl: socket for scheduler");
		goto out_socs;
	}

	while(1)
	{
		rc = handle_epoll(epollfd, sched_sock, sfd, server_soc);
		if (rc < 0) {
			goto out_socs;
		} else if (rc == 1) {
			if (start_guest(0) < 0)
				goto out_socs;
			printf("Started ukvm with guest\n");
		} else if (rc == 3) {
			if (start_guest(1) < 0)
				goto out_socs;
			printf("Started ukvm with migrated guest\n");
		} else if (rc == 2) {
			continue;
		} else {
			if (send_deploy_res(sched_sock, rc) < 0)
				goto out_socs;
		}

	}

	return 0;

out_socs:
	close(server_soc);
out_pol:
	close(epollfd);
out_soc:
	close(sched_sock);
	exit(EXIT_FAILURE);
}
