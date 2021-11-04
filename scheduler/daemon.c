#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <getopt.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/un.h>
#include <sys/sendfile.h>

#include <signal.h>

#include "common.h"

#define BIN_PATH_LEN	254
#define UKVM_SOC	"/tmp/ukvm_socket"
#define GUEST_BIN_PATH	"/tmp/binary.ukvm"
#define PORT_NODES	1742
#define LOCALHOST	(((127&0xff) << 24) | ((0&0xff) << 16) | ((0&0xff) << 8) | 1)

static struct in_addr to_node;

struct ukvm_ps {
	pid_t pid;
	char socket[30];
	char binary[30];
	char net[30];
	char mig_file[30];
};

/*
 * Write exactly <size> bytes to the file pointed  by <file>
 */
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

/*
 * Read exactly size bytes from a file descriptor
 */
static uint8_t *read_file_n(int soc, off_t size)
{
	int n;
	uint8_t *buf = NULL;
	off_t offset = 0;

	buf = malloc(size);
	if (!buf) {
		err_print("Out of memory");
		return NULL;
	}

	while (offset < size) {
		n = read(soc, buf + offset, size - offset);
		if (n < 0) {
			perror("reading file");
			goto err_read_f;
		} else if (n == 0) {
			err_print("Unexpected EOF\n");
			goto err_read_f;
		}
		offset += n;
	}

	return buf;
err_read_f:
	free(buf);
	return NULL;
}

static ssize_t receive_mig_files(int socket, char *file)
{
	ssize_t rc;
	uint8_t *buf;
	struct com_nod node_com;

	rc = read(socket, &node_com, sizeof(struct com_nod));
	if (rc < sizeof(struct com_nod)) {
		if (rc < 0)
			perror("Read file from socket\n");
		else if (rc == 0)
			err_print("Lost connection with sender\n");
		else
			err_print("Short read on file size\n");
		return -1;
	}
	if (node_com.type != migrate) {
		err_print("Invalid message type from other node\n");
		return -1;
	}
	printf("size of binary is %ld\n", node_com.size);

	buf = read_file_n(socket, node_com.size);
	if (!buf)
		return -1;

	rc = write_file_n(buf, node_com.size, file);
	free(buf);
	if (rc < 0)
		return -1;

	return 0;
}

/*
 * Send migration file to an another node
 */
static void transmit_mig_file()
{
	int sockfd, rc;
	struct sockaddr_in addr = {0};

	memcpy(&addr.sin_addr, &to_node, sizeof(struct in_addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(PORT_NODES);
	sockfd = setup_socket(0, (struct sockaddr *) &addr, 0);
	if (sockfd == -1) {
		err_print("socket error %d\n", errno);
		return;
	}

	rc = send_file(sockfd, "/tmp/binary_0.ukvm", migrate);
	if (rc < 0)
		return;

	rc = send_file(sockfd, "/tmp/file.mig", migrate);
	if (rc < 0)
		return;
}

/*
 * Start a new guest using solo5. The flag mig is used to start
 * a migrated guest.
 */
static int start_guest(const char *mig_arg, const char *guest_bin,
			const char *net_arg, const char *mon_arg,
			const char *args)
{
	int pid;

	pid  = fork();
	if (pid == 0) {
		/*
		 * child
		 */
		int fd;
		char *ukvm_bin = NULL;
		char out_file[24];
		char disk_arg[32] = "--disk=";
		char mem_arg[32] = "--mem=1024";

		strcat(disk_arg, guest_bin);
		ukvm_bin = getenv("UKVM_BIN");
		if (!ukvm_bin) {
			err_print("UKVM_BIN environment variable has not been set\n");
			return -1;;
		}
		sprintf(out_file, "/tmp/guest_%d.out", getpid());
		fd = open(out_file, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
		if (fd < 0) {
			perror("Opening redirected file\n");
			exit(EXIT_FAILURE);
		}
		/*
		 * Redirect both stderr and stdout to the output file
		 * of the guest.
		 */
		if (dup2(fd, 1) < 0) {
			perror("Redirecting stdout\n");
			close(fd);
			exit(EXIT_FAILURE);
		}
		if (dup2(fd, 2) < 0) {
			perror("Redirecting stderr\n");
			close(fd);
			exit(EXIT_FAILURE);
		}
		if (mig_arg)
			execl(ukvm_bin, ukvm_bin, mem_arg, net_arg, disk_arg,
				mig_arg, mon_arg, guest_bin, (char *)NULL);
		else
			execl(ukvm_bin, ukvm_bin, mem_arg, net_arg, disk_arg,
				mon_arg, guest_bin, args, (char *)NULL);
	} else if (pid == -1) {
		perror("fork");
		return -1;
	}

	return pid;
}

static int send_ukvm_cmd(const char *cmd, const char *soc_pth)
{
	struct sockaddr_un addr = {0};
	int sockfd;
	int rc;

	/*
	 * Send migration command
	 */
	addr.sun_family = AF_UNIX;
	strcpy(addr.sun_path, soc_pth);
	sockfd = setup_socket(0, (struct sockaddr *) &addr, 0);
	if (sockfd < 0) {
		perror("Create socket to send commands to ukvm");
		return -1;
	}

	rc = write(sockfd, cmd, strlen(cmd));
	if (rc < strlen(cmd)) {
		if (rc < 0)
			perror("write to ukvm socket");
		else
			err_print("Short write to ukvm socket\n");
		close(sockfd);
		return -1;
	}
	close(sockfd);
	return 0;
}

static char *rcv_args(int socket, int *rc)
{
	struct com_nod nargs_com;
	char *args = NULL;

	*rc = 0;
	*rc = read(socket, &nargs_com, sizeof(struct com_nod));
	if (*rc < sizeof(struct com_nod)) {
		err_print("Lost connection with primary scheduler\n");
		if (*rc < 0)
			perror("Read message from primary\n");
		*rc = -1;
		return NULL;
	}
	if (nargs_com.type != arguments) {
		err_print("Received invalid message\n");
		*rc = -1;
		return NULL;
	}
	if (nargs_com.size == 0) {
		*rc = 0;
		return NULL;
	}
	args = malloc(nargs_com.size);
	if (args == NULL) {
		err_print("Out of memory\n");
		return NULL;
	}
	*rc = read(socket, args, nargs_com.size);
	if (*rc < nargs_com.size) {
		err_print("Lost connection with primary scheduler\n");
		if (*rc < 0)
			perror("Read message from primary\n");
		free(args);
		*rc = -1;
		return NULL;
	}

	return args;
}

/*
 * Handle new message from primary scheduler
 */
static struct ukvm_ps *msg_from_primary(int socket, int *ret)
{
	int rc;
	uint8_t *buf;
	struct com_nod node_com;
	struct ukvm_ps *ps_ukvm = NULL;

	*ret = 3;

	ps_ukvm = malloc(sizeof(struct ukvm_ps));
	if (ps_ukvm == NULL) {
		err_print("Out of memory\n");
		goto ret_1;
	}

	rc = read(socket, &node_com, sizeof(struct com_nod));
	if (rc < sizeof(struct com_nod)) {
		err_print("Lost connection with primary scheduler\n");
		if (rc < 0)
			perror("Read message from primary\n");
		goto ret_1;
	}
	if (node_com.type == deploy) {
		char *args = NULL;

		memset(ps_ukvm->socket, 0, 30*sizeof(char));
		memset(ps_ukvm->binary, 0, 30*sizeof(char));
		/*
		 * Receive and store the binary to deploy
		 */
		buf = read_file_n(socket, node_com.size);
		if (!buf)
			goto ret_1;

		sprintf(ps_ukvm->binary, "/tmp/binary_0.ukvm");
		sprintf(ps_ukvm->socket, "--mon=/tmp/ukvm0.sock");
		rc = write_file_n(buf, node_com.size, ps_ukvm->binary);
		free(buf);
		if (rc < 0)
			goto ret_1;
		args = rcv_args(socket, &rc);
		if (rc < 0)
			goto ret_1;
		// start new task
		ps_ukvm->pid = start_guest(NULL, ps_ukvm->binary, "--net=tap0",
					   ps_ukvm->socket, args);
		if (ps_ukvm->pid < 0)
			goto ret_1;
		printf("Started ukvm guest with pid %d\n", ps_ukvm->pid);
		*ret = 0;
	} else if (node_com.type == evict) {
		char *args = NULL;

		memset(ps_ukvm->socket, 0, 30*sizeof(char));
		memset(ps_ukvm->binary, 0, 30*sizeof(char));
		/*
		 * Receive and store the binary to deploy
		 */
		buf = read_file_n(socket, node_com.size);
		if (!buf)
			goto ret_1;

		sprintf(ps_ukvm->binary, "/tmp/binary_1.ukvm");
		sprintf(ps_ukvm->socket, "--mon=/tmp/ukvm1.sock");
		rc = write_file_n(buf, node_com.size, ps_ukvm->binary);
		free(buf);
		if (rc < 0)
			goto ret_1;
		args = rcv_args(socket, &rc);
		if (rc < 0)
			goto ret_1;
		// Stop current task
		if (send_ukvm_cmd("stop", "/tmp/ukvm0.sock") < 0)
			goto ret_1;
		// start new task
		ps_ukvm->pid = start_guest(NULL, ps_ukvm->binary, "--net=tap1",
					   ps_ukvm->socket, args);
		if (ps_ukvm->pid < 0)
			goto ret_1;
		printf("Started ukvm guest with pid %d\n", ps_ukvm->pid);
		*ret = 1;
	} else if (node_com.type == resume) {
		if (send_ukvm_cmd("resume", "/tmp/ukvm0.sock") < 0)
			goto ret_1;
		*ret = 3;
	} else if (node_com.type == mig_cmd) {
		if (send_ukvm_cmd("savevm /tmp/file.mig", "/tmp/ukvm0.sock") < 0)
			goto ret_1;
		to_node = node_com.rcv_ip;
		if (ntohl(to_node.s_addr) == LOCALHOST) {
			struct sockaddr_in saddr;
			socklen_t slen;

			slen = sizeof(struct sockaddr_in);
			memset(&saddr, 0, slen);
			if (getpeername(socket, (struct sockaddr *) &saddr, &slen) < 0) {
				perror("Get ip from socket\n");
				goto ret_1;
			}
			to_node = saddr.sin_addr;
		}
		*ret = 3;
	}
	return ps_ukvm;
ret_1:
	*ret = -1;
	return NULL;
}

/*
 * Handle a change in child's process state
 */
static int handle_sigchld(int sigfd)
{
	int rc;
	struct signalfd_siginfo sinfo;

	rc = read(sigfd, &sinfo, sizeof(struct signalfd_siginfo));
	if (rc < 0) {
		perror("Read signalfd\n");
		return -1;
	} else if (rc < sizeof(struct signalfd_siginfo)) {
		err_print("Short read on signalfd\n");
		return -1;
	}
	if (sinfo.ssi_signo == SIGCHLD) {
		/*
		 * Do not let our child turn to zombie
		 */
		if (wait(NULL) < 0) {
			perror("wait child");
			return -1;
		}

		printf("My child died with code %d\n", sinfo.ssi_status);
		if (sinfo.ssi_status == 0) {
			/*
			 * Successful execution
			 */
			return 4;
		} else if (sinfo.ssi_status == 7) {
			/*
			 * Migration file is ready
			 */
			return 7;
		} else {
			/*
			 * Task failed
			 */
			return 5;
		}
	}
	return 0;
}

/*
 * Handle a connection with another node in order to
 * receive the migration files
 */
static struct ukvm_ps *rcv_start_migrated_guest(int server_soc)
{
	int rc;
	int mig_soc;
	struct ukvm_ps *ps_ukvm = NULL;

	mig_soc = accept(server_soc, NULL, NULL);
	if (mig_soc == -1) {
		perror("frontend socket accept");
		goto err_out;
	}

	ps_ukvm = malloc(sizeof(struct ukvm_ps));
	if (ps_ukvm == NULL) {
		err_print("Out of memory\n");
		goto err_out;
	}
	sprintf(ps_ukvm->binary, "/tmp/rcvd_file.ukvm");
	sprintf(ps_ukvm->mig_file, "--load=/tmp/file.mig");
	sprintf(ps_ukvm->socket, "--mon=/tmp/ukvm1.sock");
	sprintf(ps_ukvm->net, "--net=tap0");
	/*
	 * The first file is the binary
	 * The second file is the migration file
	 */
	rc = receive_mig_files(mig_soc, ps_ukvm->binary);
	if (rc < 0)
		goto err_out;

	rc = receive_mig_files(mig_soc, ps_ukvm->mig_file + 7);
	if (rc < 0)
		goto err_out;

	/*
	 * Start the migrated guest.
	 */
	ps_ukvm->pid = start_guest(ps_ukvm->mig_file, ps_ukvm->binary, ps_ukvm->net,
				ps_ukvm->socket, NULL);
	if (ps_ukvm->pid < 0)
		goto err_out;
	printf("Started ukvm guest with pid %d\n", ps_ukvm->pid);

	close(mig_soc);
	return ps_ukvm;
err_out:
	free(ps_ukvm);
	ps_ukvm = NULL;
	close(mig_soc);
	return ps_ukvm;
}

/*
 * Send the execution result to primary scheduler.
 * The result is the exit status of the ukvm process
 */
static int send_deploy_res(int socket, int res)
{
	ssize_t rc;

	rc = write(socket, &res, sizeof(int));
	if (rc < 0) {
		perror("Send result to primary\n");
		return -1;
	} else if (rc < sizeof(int)) {
		err_print("Short write on execution result\n");
		return -1;
	}
	return 0;
}

int main(int argc, char *argv[])
{
	int rc, sched_sock, port = 0, epollfd, sfd, server_soc;
	struct sockaddr_in sockaddr = {0};
	char *ip_addr = NULL;
	struct epoll_event ev;
	sigset_t schld_set;
	struct ukvm_ps *instance = NULL;

	/*
	 * Get ip address and port of the primary scheduler
	 */
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

	rc = inet_pton(AF_INET, ip_addr, &sockaddr.sin_addr);
	if (rc <=0) {
		err_print("Wrong IPv4 address format\n");
		exit(EXIT_FAILURE);
	}

	epollfd = epoll_create1(0);
	if (epollfd == -1) {
		perror("epoll_create1");
		exit(EXIT_FAILURE);
	}

	/*
	 * Setup socket for communication with the primary scheduler
	 */
	sockaddr.sin_family = AF_INET;
	sockaddr.sin_port = htons(port);
	sched_sock = setup_socket(epollfd, (struct sockaddr *) &sockaddr, 0);
	if (sched_sock == -1) {
		err_print("socket error %d\n", errno);
		goto out_pol;
	}

	/*
	 * Setup socket for communication between daemons (for migration)
	 */
	sockaddr.sin_addr.s_addr = htonl(INADDR_ANY);
	sockaddr.sin_port = htons(PORT_NODES);
	server_soc = setup_socket(epollfd, (struct sockaddr *) &sockaddr, 1);
	if (server_soc < 0) {
		err_print("Could not setup_socket for inter node communication\n");
		goto out_soc;
	}

	/*
	 * Setup signalfd for SIGCHLD in order to poll for changes in
	 * execution ukvm children who got spawned.
	 */
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
		goto out_sfd;
	}

	/*
	 * Main loop for daemon scheduler
	 */
	while(1)
	{
		int epoll_ret, i;
		struct epoll_event events[3];
		/*
		 * Handle a poll event. There are 3 possible events.
		 * - Message from primary scheduler asking to deploy a task
		 * - Message from primary scheduler asking for migration
		 * - Resume migrated task
		 * - Send the result of task's execution back to primary
		 */

		epoll_ret = epoll_wait(epollfd, events, 3, -1);
		if (epoll_ret == -1) {
			perror("epoll wait");
			goto out_sfd;
		}

		for (i = 0; i < epoll_ret; i++) {

			if (events[i].data.fd == sched_sock) {
				instance = msg_from_primary(sched_sock, &rc);
				if (rc < 0)
					goto out_sfd;
			} else if (events[i].data.fd == sfd) {
				rc = handle_sigchld(sfd);
				if (rc < 0)
					goto out_sfd;
				if (rc == 0)
					continue;
				if (rc == 7)
					transmit_mig_file();
				// report execution result to primary
				free(instance);
				instance = NULL;
				if (send_deploy_res(sched_sock, rc) < 0)
					goto out_socs;
			} else if (events[i].data.fd == server_soc) {
				instance = rcv_start_migrated_guest(server_soc);
				if (instance == NULL)
					goto out_sfd;
			}
		}

	}

	return 0;

out_sfd:
	close(sfd);
out_socs:
	close(server_soc);
out_soc:
	close(sched_sock);
out_pol:
	close(epollfd);
	exit(EXIT_FAILURE);
}
