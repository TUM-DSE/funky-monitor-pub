#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <arpa/inet.h>

#define FRONT_SOCK	"/tmp/front.sock"
#define BIN_PATH_LEN	24
#define MAX_EVENTS	10
#define NODES_PORT	4217

enum task_state {
	ready = 0,
	running,
	stopped,
	done
};

enum node_state {
	available = 0,
	busy
};

struct task {
	uint32_t id;
	char *bin_path;
	uint32_t node_id;
	enum task_state state;
	struct task *next;
};

struct node {
	/*
	 * Communication data like ip, socket etc;
	 */
	enum node_state state;
};

enum msg_type {
	task_new = 0,
	node_new,
	node_ret
};

struct notify_msg {
	enum msg_type type;
	union {
		struct task *tsk;
		struct node *nod;
	};
};

struct thr_data {
	int socket;
	int efd;
};

static char *strdup(const char *s)
{
	size_t len = strlen(s) + 1;
	void *new = malloc(len);

	if (new == NULL)
		return NULL;

	return (char *) memcpy(new, s, len);
}

static struct task *create_new_task(char *path)
{
	struct task *new_task;

	new_task = malloc(sizeof(struct task));
	if (!new_task) {
		fprintf(stderr, "Out of memory while creating new struct task\n");
		return NULL;
	}

	new_task->bin_path = strdup(path);
	new_task->node_id = 0;
	new_task->state = ready;
	new_task->next = NULL;

	return new_task;
}

static void *get_cmd_front(void *arg)
{
	int con, rc;
	char bin_path[BIN_PATH_LEN];
	struct thr_data *td = (struct thr_data *) arg;
	struct task *tsk_to_add = NULL;
	struct notify_msg *nmsg = NULL;

	con = accept(td->socket, NULL, NULL);
	if (con == -1) {
		perror("frontend socket accept");
		return NULL;
	}

	rc = read(con, bin_path, BIN_PATH_LEN);
	if (rc <= 0) {
		fprintf(stderr, "Read from peer failed");
		goto exit_front;
	}
	/* Make sure we do not read rubbish, old commands etc */
	bin_path[(rc > BIN_PATH_LEN) ? BIN_PATH_LEN - 1: rc] = '\0';

	printf("New binary at %s\n", bin_path);
	tsk_to_add = create_new_task(bin_path);
	if (!tsk_to_add) {
		fprintf(stderr, "Could not create new task\n");
		goto exit_front;
	}

	nmsg = malloc(sizeof(struct notify_msg));
	if (!nmsg) {
		fprintf(stderr, "Out of memory for notify msg\n");
		free(tsk_to_add);
		goto exit_front;
	}
	nmsg->type = task_new;
	nmsg->tsk = tsk_to_add;

	rc = write(td->efd, &nmsg, sizeof(uint64_t));
	if (rc < 0)
		perror("write new task in eventfd");

exit_front:
	close(con);
	return NULL;
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
		strcpy(saddr_un.sun_path, FRONT_SOCK);
		unlink(FRONT_SOCK);
		rc = bind(sock, (struct sockaddr *) &saddr_un,
				sizeof(struct sockaddr_un));
	} else if (domain == AF_INET) {
		struct sockaddr_in saddr_in = {0};

		saddr_in.sin_family = AF_INET;
		saddr_in.sin_addr.s_addr = htonl(INADDR_ANY);;
		saddr_in.sin_port = htons(NODES_PORT);
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

	rc = listen(sock, MAX_EVENTS - 1);
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

static int handle_event(int fd, int fsock, int nsock, int epollfd,
			struct task **tsk, struct node *nd)
{
	int rc;
	pthread_t worker;

	if (fd == fsock) {
		struct thr_data worker_data;
		struct epoll_event ev;

		rc = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
		if (rc < 0) {
			perror("Eventfd for frontend socket");
			return -1;
		}

		worker_data.socket = fsock;
		worker_data.efd = rc;
		ev.events = EPOLLIN | EPOLLET;
		ev.data.fd = rc;
		if (epoll_ctl(epollfd, EPOLL_CTL_ADD, rc, &ev) == -1) {
			perror("epoll_ctl: eventfd");
			return -1;
		}

		rc = pthread_create(&worker, NULL, get_cmd_front,
				   (void *) &worker_data);
		if (rc) {
			perror("Frontend worker create");
			return -1;
		}

	} else if (fd == nsock) {
		printf("new connection in node socket\n");
	} else {
		uint64_t eeval;
		struct notify_msg *new_msg;

		rc = read(fd, &eeval, sizeof(uint64_t));
		if (rc < 0) {
			perror("Read eventfd");
			return -1;;
		}
		new_msg = (struct notify_msg *) eeval;
		if (new_msg->type == task_new) {
			*tsk = new_msg->tsk;
		}
		free(new_msg);
		return 1;
	}

	return 0;
}

int main()
{
	uint32_t nr_tsks = 0;
	struct task *tsk_to_add = NULL;
	struct task *tsk_head = NULL, *tsk_last = NULL;
	struct epoll_event events[MAX_EVENTS];
	int rc, epoll_ret, epollfd, front_sock, nodes_sock;
	struct task *tmp;

	epollfd = epoll_create1(0);
	if (epollfd == -1) {
		perror("epoll_create1");
		exit(EXIT_FAILURE);
	}

	front_sock = setup_socket(AF_UNIX, epollfd);
	if (front_sock < 0) {
		fprintf(stderr, "Could not setup_socketup frontend socket\n");
		goto fail_pol;
	}

	nodes_sock = setup_socket(AF_INET, epollfd);
	if (front_sock < 0) {
		fprintf(stderr, "Could not setup_socketup frontend socket\n");
		goto fail_soc;
	}

	printf("Listesning commands at %s\n", FRONT_SOCK);

	while(1) {
		int i;

		epoll_ret = epoll_wait(epollfd, events, MAX_EVENTS, -1);
		if (epoll_ret == -1) {
			perror("epoll_wait");
			goto fail_socs;
		}

		for (i = 0; i < epoll_ret; i++) {
			if (events[i].events == 0)
				continue;
			rc = handle_event(events[i].data.fd, front_sock,
					  nodes_sock, epollfd,
					  &tsk_to_add, NULL);
			switch(rc) {
			case 0:
				continue;
			case 1:
				tsk_to_add->id = nr_tsks++;
				break;
			case 2:
				printf("new node\n");
				break;
			default:
				fprintf(stderr, "Error while handling event\n");
				close(events[i].data.fd);
				goto fail_socs;
			}
			close(events[i].data.fd);
		}
		if (!tsk_to_add)
			continue;

		if (tsk_head == NULL) {
			tsk_head = tsk_last = tsk_to_add;
		} else {
			tsk_last->next = tsk_to_add;
			tsk_last = tsk_to_add;
		}
		tsk_to_add = NULL;

		tmp = tsk_head;
		while(tmp) {
			printf("Task with id %d and bin at %s\n", tmp->id, tmp->bin_path);
			tmp = tmp->next;
		}
	}

	return 0;

fail_socs:
	close(nodes_sock);
fail_soc:
	close(front_sock);
fail_pol:
	close(epollfd);
	exit(EXIT_FAILURE);
}
