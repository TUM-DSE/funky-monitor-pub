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
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/sendfile.h>
#include <fcntl.h>

#define FRONT_SOCK	"/tmp/front.sock"
#define BIN_PATH_LEN	254
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
	struct node *node;
	enum task_state state;
	struct task *next;
};

struct node {
	uint32_t id;
	pthread_t thr;
	int ev_fd;
	uint32_t task_id;
	struct task *task;
	enum node_state state;
	struct node *next;
};

struct node_result {
	pthread_t thr;
	int res;
};

enum msg_type {
	task_new = 0,
	node_ret,
	node_down
};

struct notify_msg {
	enum msg_type type;
	union {
		struct task *tsk;
		struct node_result nres;
	};
};

struct thr_data {
	int socket;
	int rcv_efd;
	int snd_efd;
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
	if (!new_task->bin_path) {
		free(new_task);
		return NULL;
	}
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

	rc = write(td->snd_efd, &nmsg, sizeof(uint64_t));
	if (rc < 0)
		perror("write new task in eventfd");

exit_front:
	close(con);
	free(td);
	return NULL;
}

static int send_binary(struct task *tsk, int socket)
{
	int rc = 0, fd;
	struct stat st;
	ssize_t n, count = 0;

	fd = open(tsk->bin_path, O_RDONLY);
	if (fd < 0) {
		perror("Opening binry to send it");
		return -1;
	}

	rc = stat(tsk->bin_path, &st);
	if (rc < 0) {
		perror("binary size");
		goto err_send;
	}

	rc = write(socket, &st.st_size, sizeof(off_t));
	if (rc < sizeof(off_t)) {
		fprintf(stderr, "Short send of binary size\n");
		if (rc < 0)
			perror("Send file siee");
		goto err_send;
	}

	printf("I will deploy binary with id %d. It has size %ld\n", tsk->id, st.st_size);

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

static int handle_node_comm(int epollfd, int con, int sched_efd, int snd_efd)
{
	int rc;
	int epoll_ret, i;
	struct epoll_event events[2];
	struct task *tsk;

	epoll_ret = epoll_wait(epollfd, events, 2, -1);
	if (epoll_ret == -1) {
		perror("node epoll wait");
		return -1;;
	}

	for (i = 0; i < epoll_ret; i++) {
		int node_msg = 0;

		if (events[i].data.fd == con) {
			struct notify_msg *nmsg = NULL;

			rc = read(con, &node_msg, sizeof(int));
			if (rc == 0) {
				printf("Connection closed\n");
				return -1;;
			} else if (rc < sizeof(int)) {
				fprintf(stderr, "Short read from node's message\n");
			}

			nmsg = malloc(sizeof(struct notify_msg));
			if (!nmsg) {
				fprintf(stderr, "Out of memory for notify msg\n");
				return -1;;
			}
			nmsg->type = node_ret;
			nmsg->nres.thr = pthread_self();
			nmsg->nres.res = node_msg;

			rc = write(snd_efd, &nmsg, sizeof(uint64_t));
			if (rc < 0)
				perror("write new task in eventfd");
		} else if (events[i].data.fd == sched_efd) {
			uint64_t efval;

			rc = read(sched_efd, &efval, sizeof(uint64_t));
			if (rc < sizeof(uint64_t)) {
				fprintf(stderr, "Read from eventfd failed");
				return -1;;
			}
			tsk = (struct task *) efval;

			rc = send_binary(tsk, con);
			if (rc < 0)
				return -1;

		}
	}
	return 0;
}

static void *node_communication(void *arg)
{
	int con, rc, epollfd = 0;
	struct thr_data *td = (struct thr_data *) arg;
	struct epoll_event ev;
	struct notify_msg *nmsg = NULL;

	con = accept(td->socket, NULL, NULL);
	if (con == -1) {
		perror("node socket accept");
		free(td);
		return NULL;
	}

	epollfd = epoll_create1(0);
	if (epollfd == -1) {
		perror("node_communication: epoll_create1");
		goto exit_node;
	}

	ev.events = EPOLLIN;
	ev.data.fd = con;
	if (epoll_ctl(epollfd, EPOLL_CTL_ADD, con, &ev) == -1) {
		perror("epoll_ctl: node con socket");
		goto exit_node;
	}

	ev.events = EPOLLIN;
	ev.data.fd = td->rcv_efd;
	if (epoll_ctl(epollfd, EPOLL_CTL_ADD, td->rcv_efd, &ev) == -1) {
		perror("epoll_ctl: node eventfd");
		goto exit_node;
	}

	while(1)
	{
		rc = handle_node_comm(epollfd, con, td->rcv_efd, td->snd_efd);
		if (rc < 0)
			goto exit_node;
	}

exit_node:
	close(con);
	nmsg = malloc(sizeof(struct notify_msg));
	if (!nmsg) {
		fprintf(stderr, "Out of memory for notify msg\n");
		goto out_node;
	}
	nmsg->type = node_down;
	nmsg->nres.thr = pthread_self();

	rc = write(td->snd_efd, &nmsg, sizeof(uint64_t));
	if (rc < 0)
		perror("write new task in eventfd");
out_node:
	free(td);
	if (epollfd)
		close(epollfd);
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
			struct task **tsk, struct node **nd, pthread_t *node_ref)
{
	int rc;
	pthread_t worker;
	struct thr_data *worker_data;

	if (fd == fsock) {
		struct epoll_event ev;

		worker_data = malloc(sizeof(struct thr_data));
		if (!worker_data) {
			fprintf(stderr, "Out of memory for worker data\n");
			return -1;
		}
		rc = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
		if (rc < 0) {
			perror("Eventfd for frontend socket");
			goto err_handle;
		}

		worker_data->socket = fsock;
		worker_data->rcv_efd = 0;
		worker_data->snd_efd = rc;
		ev.events = EPOLLIN | EPOLLET;
		ev.data.fd = rc;
		if (epoll_ctl(epollfd, EPOLL_CTL_ADD, rc, &ev) == -1) {
			perror("epoll_ctl: eventfd");
			goto err_handle_ev;
		}

		rc = pthread_create(&worker, NULL, get_cmd_front,
				   (void *) worker_data);
		if (rc) {
			perror("Frontend worker create");
			goto err_handle_ev;
		}

	} else if (fd == nsock) {
		struct epoll_event ev;
		struct node *new_node;

		worker_data = malloc(sizeof(struct thr_data));
		if (!worker_data) {
			fprintf(stderr, "Out of memory for worker data\n");
			return -1;
		}
		worker_data->socket = nsock;
		worker_data->rcv_efd = 0;
		worker_data->snd_efd = 0;
		rc = eventfd(0, EFD_CLOEXEC);
		if (rc < 0) {
			perror("Eventfd for frontend socket");
			goto err_handle;
		}
		worker_data->rcv_efd = rc;

		rc = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
		if (rc < 0) {
			perror("Eventfd for frontend socket");
			free(worker_data);
			goto err_handle_ev;
		}
		worker_data->snd_efd = rc;

		ev.events = EPOLLIN | EPOLLET;
		ev.data.fd = rc;
		if (epoll_ctl(epollfd, EPOLL_CTL_ADD, rc, &ev) == -1) {
			perror("epoll_ctl: eventfd");
			goto err_handle_ev;
		}

		rc = pthread_create(&worker, NULL, node_communication,
				   (void *) worker_data);
		if (rc) {
			perror("Frontend worker create");
			goto err_handle_ev;
		}

		new_node = malloc(sizeof(struct node));
		if (!new_node) {
			fprintf(stderr, "Out of memory for new node\n");
			/* TODO: close connection and destroy thread */
			goto err_handle_ev;
		}
		new_node->thr = worker;
		new_node->task_id = 0;
		new_node->task = NULL;
		new_node->ev_fd = worker_data->rcv_efd;
		new_node->state = available;
		*nd = new_node;
		return 1;

	} else {
		uint64_t eeval;
		struct notify_msg *new_msg;
		int ret = 2;

		rc = read(fd, &eeval, sizeof(uint64_t));
		if (rc < 0) {
			perror("Read eventfd");
			return -1;;
		}
		new_msg = (struct notify_msg *) eeval;
		switch(new_msg->type) {
		case task_new:
			*tsk = new_msg->tsk;
			break;
		case node_down:
			*node_ref = new_msg->nres.thr;
			ret = 3;
			break;
		case node_ret:
			*node_ref = new_msg->nres.thr;
			ret = new_msg->nres.res;
			break;
		default:
			fprintf(stderr, "I should not be here %s - %d\n", __func__, __LINE__);
			exit(1);
		}
		free(new_msg);

		return ret;
	}

	return 0;
err_handle_ev:
	if (worker_data->rcv_efd)
		close(worker_data->rcv_efd);
	if (worker_data->snd_efd)
		close(worker_data->snd_efd);
err_handle:
	free(worker_data);
	return -1;
}

static void remove_task(struct task **hd, struct task **lst, uint32_t id)
{
	struct task *task_tmp = *hd, *task_prev = NULL;

	while(task_tmp) {
		if (task_tmp->id == id) {
			if (task_tmp == *lst)
				*lst = task_prev;
			if (task_prev == NULL)
				*hd = task_tmp->next;
			else
				task_prev->next = task_tmp->next;
			if (task_tmp->node)
				task_tmp->node->state = available;
			printf("%d ", task_tmp->id);
			free(task_tmp->bin_path);
			free(task_tmp);
			return;
		}
		task_prev = task_tmp;
		task_tmp = task_tmp->next;
	}

	return;
}

static void remove_node(struct node **hd, struct node **lst, pthread_t ref)
{

	struct node *node_tmp = *hd, *node_prev = NULL;

	while(node_tmp) {
		if (node_tmp->thr == ref) {
			if (node_tmp == *lst)
				*lst = node_prev;
			if (node_prev == NULL)
				*hd = node_tmp->next;
			else
				node_prev->next = node_tmp->next;
			if (node_tmp->task)
				node_tmp->task->state = ready;
			close(node_tmp->ev_fd);
			free(node_tmp);
			return;
		}
		node_prev = node_tmp;
		node_tmp = node_tmp->next;
	}

	return;
}

#if 0
		} else if (rc == 4 || rc == 5) {
			if (node_tmp->thr == node_ref) {
				node_tmp->state = available;
				break;
			}
		}
#endif
int main()
{
	uint32_t nr_tsks = 1;
	struct task *tsk_to_add = NULL;
	struct node *node_to_add = NULL;
	struct node *node_head = NULL, *node_last = NULL;
	struct task *tsk_head = NULL, *tsk_last = NULL;
	struct epoll_event events[MAX_EVENTS];
	int rc = 0, epoll_ret, epollfd, front_sock, nodes_sock;
	struct task *tsk_tmp;
	struct node *node_tmp;

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
		pthread_t node_ref = 0;
		struct task *tsk_avail = NULL;
		struct node *node_avail = NULL;

		epoll_ret = epoll_wait(epollfd, events, MAX_EVENTS, -1);
		if (epoll_ret == -1) {
			perror("epoll_wait");
			goto fail_socs;
		}

		for (i = 0; i < epoll_ret; i++) {
			printf("ksupnhsa gia %d\n", events[i].data.fd);
			if (events[i].events == 0)
				continue;
			rc = handle_event(events[i].data.fd, front_sock,
					  nodes_sock, epollfd,
					  &tsk_to_add, &node_to_add, &node_ref);
			switch(rc) {
			case 0:
				continue;
			case 1:
				if (node_head == NULL) {
					node_to_add->id = 1;
					node_head = node_last = node_to_add;
				} else {
					node_to_add->id = node_last->id + 1;
					node_last->next = node_to_add;
					node_last = node_to_add;
				}
				node_to_add = NULL;
				continue;
			case 2:
				tsk_to_add->id = nr_tsks++;
				if (tsk_head == NULL) {
					tsk_head = tsk_last = tsk_to_add;
				} else {
					tsk_last->next = tsk_to_add;
					tsk_last = tsk_to_add;
				}
				tsk_to_add = NULL;
				break;
			case 3:
				printf("Node down\n");
				remove_node(&node_head, &node_last, node_ref);
				break;
			case 4:
				printf("Task with id ");
				node_tmp = node_head;
				while (node_tmp) {
					if (node_tmp->thr == node_ref)
						break;
					node_tmp = node_tmp->next;

				}
				if (!node_tmp) {
					fprintf(stderr, "I could not find node..\n");
					goto fail_socs;
				}

				remove_task(&tsk_head, &tsk_last, node_tmp->task_id);
				printf("terminated successfully\n");
				continue;
			case 5:
				printf("Task with id ");
				node_tmp = node_head;
				while (node_tmp) {
					if (node_tmp->thr == node_ref)
						break;
					node_tmp = node_tmp->next;

				}
				if (!node_tmp) {
					fprintf(stderr, "I could not find node..\n");
					goto fail_socs;
				}

				remove_task(&tsk_head, &tsk_last, node_tmp->task_id);
				printf("failed\n");
				continue;
			default:
				fprintf(stderr, "Error while handling event\n");
				close(events[i].data.fd);
				goto fail_socs;
			}
			close(events[i].data.fd);
		}

		printf("----------------- Nodes ----------------\n");
		node_tmp = node_head;
		while(node_tmp) {
			printf("Node with id %d state %d\n", node_tmp->id, node_tmp->state);
			if (node_tmp->state == available && !node_avail)
				node_avail = node_tmp;
			node_tmp = node_tmp->next;
		}
		printf("----------------------------------------\n");
		printf("----------------- Tasks ----------------\n");
		tsk_tmp = tsk_head;
		while(tsk_tmp) {
			printf("Task with id %d with state %d and bin at %s\n", tsk_tmp->id,tsk_tmp->state , tsk_tmp->bin_path);
			if (tsk_tmp->state == ready && !tsk_avail)
				tsk_avail = tsk_tmp;;
			tsk_tmp = tsk_tmp->next;
		}
		printf("----------------------------------------\n");
		if (!tsk_avail || !node_avail)
			continue;
		/*
		 * Deploy
		 */
		tsk_avail->state = running;
		tsk_avail->node = node_avail;
		tsk_avail->node_id = node_avail->id;
		node_avail->task_id = tsk_avail->id;
		node_avail->task = tsk_avail;
		node_avail->state = busy;
		rc = write(node_avail->ev_fd, &tsk_avail, sizeof(uint64_t));
		if (rc < sizeof(uint64_t))
			fprintf(stderr, "Could not write to eventfd\n");


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
