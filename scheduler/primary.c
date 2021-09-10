#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

#include "common.h"

#define BIN_PATH_LEN	254

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
	struct in_addr ipv4;;
	uint32_t task_id;
	struct task *task;
	enum node_state state;
	struct node *next;
};

struct node_result {
	pthread_t thr;
	int res;
};

struct mig_info {
	uint32_t tsk_id;
	uint32_t node_id;
};

enum msg_type {
	task_new = 0,
	node_new,
	node_ret,
	node_down,
	migration
};

struct notify_msg {
	enum msg_type type;
	union {
		struct node *node;
		struct task *tsk;
		struct node_result nres;
		struct mig_info migr_info;
	};
};

enum mnode_type {
	deploy = 0,
	migrate
};

struct msg_to_node {
	enum mnode_type type;
	union {
		struct task *tsk;
		struct node *node;
	};
};

struct thr_data {
	int socket;
	int rcv_efd;
	int snd_efd;
	struct node *node;
};

struct com_nod {
	uint8_t type;
	union {
		off_t size;
		struct in_addr rcv_ip;
	};
};

static char *strdup(const char *s)
{
	size_t len = strlen(s) + 1;
	void *new = malloc(len);

	if (new == NULL)
		return NULL;

	return (char *) memcpy(new, s, len);
}

/*
 * Create and initialize an entry for a new task
 */
static struct task *create_new_task(char *path)
{
	struct task *new_task;

	new_task = malloc(sizeof(struct task));
	if (!new_task) {
		err_print("Out of memory while creating new struct task\n");
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

/*
 * Handle commands from the frontend socket.
 * The commands can be:
 * - New task: <path_to_binary>
 * - Migrate: Task <id> Node <node_id_to_migrate>
 */
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
		err_print("Read from peer failed\n");
		goto exit_front;
	}

	/*
	 * Determine if it is a command or a pth to a new binary
	 * New task should start with '/' since full path is required.
	 */
	if (bin_path[0] == '/') {
		/* Make sure we do not read rubbish, old commands etc */
		bin_path[(rc > BIN_PATH_LEN) ? BIN_PATH_LEN - 1: rc] = '\0';

		tsk_to_add = create_new_task(bin_path);
		if (!tsk_to_add) {
			err_print("Could not create new task\n");
			goto exit_front;
		}

		/*
		 * Send the new task to main thread
		 */
		nmsg = malloc(sizeof(struct notify_msg));
		if (!nmsg) {
			err_print("Out of memory for notify msg\n");
			free(tsk_to_add);
			goto exit_front;
		}
		nmsg->type = task_new;
		nmsg->tsk = tsk_to_add;

		rc = write(td->snd_efd, &nmsg, sizeof(uint64_t));
		if (rc < 0)
			perror("write new task in eventfd");
	} else {
		uint32_t tsk_ref, node_ref;

		/*
		 * Read the id of the task which will get migrated and
		 * the new node ehre the migrated task will resume execution
		 */
		rc = sscanf(bin_path, "Task: %u Node: %u", &tsk_ref, &node_ref);
		if (rc < 2) {
			err_print("Invalid command\n");
			goto exit_front;
		}

		/*
		 * Send migration info to main thread
		 */
		nmsg = malloc(sizeof(struct notify_msg));
		if (!nmsg) {
			err_print("Out of memory for notify msg\n");
			goto exit_front;
		}
		nmsg->type = migration;
		nmsg->migr_info.tsk_id = tsk_ref;
		nmsg->migr_info.node_id = node_ref;

		rc = write(td->snd_efd, &nmsg, sizeof(uint64_t));
		if (rc < 0)
			perror("write new task in eventfd");
	}

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
	struct com_nod node_com;

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

	node_com.type = 0;
	node_com.size = st.st_size;
	rc = write(socket, &node_com, sizeof(struct com_nod));
	if (rc < sizeof(struct com_nod)) {
		if (rc < 0)
			perror("Send file size");
		else
			err_print("Short send of binary size\n");
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

static ssize_t send_migration_command(struct node *nd, int socket)
{
	ssize_t rc;
	char myIP[16];
	struct com_nod nod_com;

	inet_ntop(AF_INET, &nd->ipv4, myIP, sizeof(myIP));

	printf("Ip is %s\n", myIP);

	nod_com.type = 1;
	nod_com.rcv_ip = nd->ipv4;

	rc = write(socket, &nod_com, sizeof(struct com_nod));
	if (rc < sizeof(struct com_nod)) {
		if (rc < 0)
			perror("Send migration command");
		else
			err_print("Short send of migration command\n");
		return -1;
	}
	return 0;
}

static int handle_node_comm(int epollfd, int con, int sched_efd, int snd_efd)
{
	int rc;
	int epoll_ret, i;
	struct epoll_event events[2];

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
				err_print("Short read from node's message\n");
				return -1;
			}
			if (node_msg == 7)
				continue;

			nmsg = malloc(sizeof(struct notify_msg));
			if (!nmsg) {
				err_print("Out of memory for notify msg\n");
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
			struct msg_to_node *msg_node;

			rc = read(sched_efd, &efval, sizeof(uint64_t));
			if (rc < sizeof(uint64_t)) {
				err_print("Read from eventfd failed");
				return -1;;
			}

			msg_node = (struct msg_to_node *) efval;
			if (msg_node->type == deploy) {
				rc = send_binary(msg_node->tsk, con);
				free(msg_node);
				if (rc < 0)
					return -1;
			} else if (msg_node->type == migrate) {
				rc = send_migration_command(msg_node->node, con);
				free(msg_node);
				if (rc < 0)
					return -1;
			}

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
	struct node *nd = td->node;
	struct sockaddr_in saddr_node;
	socklen_t slen;

	con = accept(td->socket, NULL, NULL);
	if (con == -1) {
		perror("node socket accept");
		free(td);
		return NULL;
	}

	slen = sizeof(struct sockaddr_in);
	memset(&saddr_node, 0, slen);
	if (getsockname(con, (struct sockaddr *) &saddr_node, &slen) < 0) {
		perror("Get ip from socket\n");
		goto exit_node;
	}
	nd->ipv4 = saddr_node.sin_addr;

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
		err_print("Out of memory for notify msg\n");
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

static void migration_cmd(struct mig_info minfo,struct node *nhead, struct task *thead)
{
	struct node *node_tmp, *node_in;
	struct task *tsk_tmp;
	ssize_t rc;
	struct msg_to_node *msg_nd;

	tsk_tmp = thead;
	while (tsk_tmp) {
		if (tsk_tmp->id == minfo.tsk_id)
			break;
		tsk_tmp = tsk_tmp->next;
	}

	node_tmp = nhead;
	while (node_tmp) {
		if (node_tmp->id == minfo.node_id)
			break;
		node_tmp = node_tmp->next;
	}
	if (!node_tmp || !tsk_tmp) {
		err_print("Could not find task or node\n");
		return;
	}
	node_in = tsk_tmp->node;
	if (node_tmp->state == busy) {
		err_print("Destination node is busy\n");
		return;
	}

	msg_nd = malloc(sizeof(struct msg_to_node));
	if (!msg_nd) {
		err_print("Out of memory for message to node\n");
		return;
	}
	msg_nd->type = migrate;
	msg_nd->node = node_tmp;

	rc = write(node_in->ev_fd, &msg_nd, sizeof(uint64_t));
	if (rc < sizeof(uint64_t)) {
		err_print("Could not write to eventfd\n");
		free(msg_nd);
		return;
	}
	node_tmp->state = busy;
	node_in->state = available;
}

static int handle_event(int fd, int fsock, int nsock, int epollfd,
			struct notify_msg **new_msg)
{
	int rc;
	pthread_t worker;
	struct thr_data *worker_data;

	if (fd == fsock) {
		/*
		 * Someone connected to Unix domain socket.
		 * Either a new command or a new task.
		 */
		struct epoll_event ev;

		worker_data = malloc(sizeof(struct thr_data));
		if (!worker_data) {
			err_print("Out of memory\n");
			return -1;
		}

		/*
		 * Create eventfd for communication between main thread and
		 * the worker thread which will handle the incoming request.
		 */
		rc = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
		if (rc < 0) {
			perror("Eventfd for frontend worker");
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

		/*
		 * Spawn worker thread to handle the request from the frontend.
		 */
		rc = pthread_create(&worker, NULL, get_cmd_front,
				   (void *) worker_data);
		if (rc) {
			perror("Frontend worker create");
			goto err_handle_ev;
		}

	} else if (fd == nsock) {
		/*
		 * Someone connected to the TCP/IP socket for nodes.
		 */
		struct epoll_event ev;
		struct node *new_node;
		struct notify_msg *tmp_msg = NULL;

		worker_data = malloc(sizeof(struct thr_data));
		if (!worker_data) {
			err_print("Out of memory\n");
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

		/*
		 * Create new node
		 */
		new_node = malloc(sizeof(struct node));
		if (!new_node) {
			err_print("Out of memory\n");
			goto err_handle_ev;
		}
		worker_data->node = new_node;
		rc = pthread_create(&worker, NULL, node_communication,
				   (void *) worker_data);
		if (rc) {
			perror("Create node worker");
			goto err_handle_ev;
		}

		new_node->thr = worker;
		new_node->task_id = 0;
		new_node->task = NULL;
		new_node->ev_fd = worker_data->rcv_efd;
		new_node->state = available;

		/*
		 * Return the new node
		 */
		tmp_msg = malloc(sizeof(struct notify_msg));
		if (!tmp_msg) {
			err_print("Out of memory\n");
			free(new_node);
			goto err_handle_ev;
		}
		tmp_msg->type = node_new;
		tmp_msg->node = new_node;
		*new_msg = tmp_msg ;

		return 1;

	} else {
		uint64_t eeval;

		rc = read(fd, &eeval, sizeof(uint64_t));
		if (rc < 0) {
			perror("Read eventfd");
			return -1;;
		}
		*new_msg = (struct notify_msg *) eeval;
		return 1;
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

/*
 * Apply scheduling algorithm and pick task and the node where it will
 * be deployed
 */
static void scheduler_algorithm(struct node *nhead, struct task *thead,
				struct node **pick_n, struct task **pick_t)
{
	struct node *node_tmp = NULL, *node_avail = NULL;
	struct task *tsk_tmp = NULL, *tsk_avail = NULL;

	printf("----------------- Nodes ----------------\n");
	node_tmp = nhead;
	while(node_tmp) {
		printf("Node with id %d state %d\n", node_tmp->id, node_tmp->state);
		if (node_tmp->state == available && !node_avail)
			node_avail = node_tmp;
		node_tmp = node_tmp->next;
	}
	printf("----------------------------------------\n");
	printf("----------------- Tasks ----------------\n");
	tsk_tmp = thead;
	while(tsk_tmp) {
		printf("Task id %d with state %d and bin at %s\n", tsk_tmp->id,tsk_tmp->state , tsk_tmp->bin_path);
		if (tsk_tmp->state == ready && !tsk_avail)
			tsk_avail = tsk_tmp;;
		tsk_tmp = tsk_tmp->next;
	}
	printf("----------------------------------------\n");
	*pick_n = node_avail;
	*pick_t = tsk_avail;
}

int main()
{
	uint32_t nr_tsks = 1;
	struct node *node_head = NULL, *node_last = NULL;
	struct task *tsk_head = NULL, *tsk_last = NULL;
	struct epoll_event events[MAX_EVENTS];
	int rc = 0, epoll_ret, epollfd, front_sock, nodes_sock;
	struct node *node_tmp;

	epollfd = epoll_create1(0);
	if (epollfd == -1) {
		perror("epoll_create1");
		exit(EXIT_FAILURE);
	}

	/*
	 * Create socket for the frontend part, where new tasks are reported.
	 */
	front_sock = setup_socket(AF_UNIX, epollfd);
	if (front_sock < 0) {
		err_print("Could not setup_socketup frontend socket\n");
		goto fail_pol;
	}

	/*
	 * Open a socket to work as a server where nodes will connect to
	 */
	nodes_sock = setup_socket(AF_INET, epollfd);
	if (front_sock < 0) {
		err_print("Could not setup_socketup frontend socket\n");
		goto fail_soc;
	}

	printf("Listesning commands at %s\n", FRONT_SOCK);
	printf("Waiting for nodes to connect in port %d\n", NODES_PORT);

	while(1) {
		int i;
		struct task *tsk_avail = NULL;
		struct node *node_avail = NULL;
		struct msg_to_node *msg_nd;

		/*
		 * Main thread waits for events from:
		 * front end socket
		 * server socket for new new nodes
		 * shared eventfds with the node worker threads
		 */
		epoll_ret = epoll_wait(epollfd, events, MAX_EVENTS, -1);
		if (epoll_ret == -1) {
			perror("epoll_wait");
			goto fail_socs;
		}

		for (i = 0; i < epoll_ret; i++) {
			struct notify_msg *new_msg = NULL;;

			if (events[i].events == 0)
				continue;

			rc = handle_event(events[i].data.fd, front_sock,
					  nodes_sock, epollfd, &new_msg);
			if (rc < 0) {
				err_print("Error while handling event\n");
				close(events[i].data.fd);
				goto fail_socs;
			} else if (rc == 0) {
				/*
				 * Nothing new happened yet.
				 * Just a new connection
				 * WOrker thread will handle and report it.
				 */
				continue;
			}

			switch(new_msg->type) {
			case node_new:
				if (node_head == NULL) {
					new_msg->node->id = 1;
					node_head = node_last = new_msg->node;
				} else {
					new_msg->node->id = node_last->id + 1;
					node_last->next = new_msg->node;
					node_last = new_msg->node;
				}
				break;
			case task_new:
				new_msg->tsk->id = nr_tsks++;
				if (tsk_head == NULL) {
					tsk_head = tsk_last = new_msg->tsk;
				} else {
					tsk_last->next = new_msg->tsk;
					tsk_last = new_msg->tsk;
				}
				close(events[i].data.fd);
				break;
			case node_down:
				printf("Node with id ");
				remove_node(&node_head, &node_last, new_msg->nres.thr);
				printf("went down\n");
				break;
			case node_ret:
				node_tmp = node_head;
				while (node_tmp) {
					if (node_tmp->thr == new_msg->nres.thr)
						break;
					node_tmp = node_tmp->next;

				}
				if (!node_tmp) {
					err_print("I could not find node..\n");
					goto fail_socs;
				}
				printf("Task with id ");
				remove_task(&tsk_head, &tsk_last, node_tmp->task_id);
				if (new_msg->nres.res == 4)
					printf("terminated successfully\n");
				else if (new_msg->nres.res == 5)
					printf("failed\n");
				break;
			case migration:
				migration_cmd(new_msg->migr_info, node_head, tsk_head);
				close(events[i].data.fd);
				break;
			default:
				err_print("Error while handling event\n");
				close(events[i].data.fd);
				goto fail_socs;
			}
			free(new_msg);
		}

		scheduler_algorithm(node_head, tsk_head, &node_avail, &tsk_avail);
		if (!tsk_avail || !node_avail)
			continue;
		/*
		 * Deploy
		 */
		msg_nd = malloc(sizeof(struct msg_to_node));
		if (!msg_nd) {
			err_print("Out of memory for message to node\n");
			goto fail_socs;
		}
		msg_nd->type = deploy;
		msg_nd->tsk = tsk_avail;

		rc = write(node_avail->ev_fd, &msg_nd, sizeof(uint64_t));
		if (rc < sizeof(uint64_t)) {
			err_print("Could not write to eventfd\n");
			free(msg_nd);
			continue;
		}

		tsk_avail->state = running;
		tsk_avail->node = node_avail;
		tsk_avail->node_id = node_avail->id;
		node_avail->task_id = tsk_avail->id;
		node_avail->task = tsk_avail;
		node_avail->state = busy;

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
