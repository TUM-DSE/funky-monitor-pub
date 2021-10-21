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
#include <sys/types.h>
#include <sys/un.h>
#include <arpa/inet.h>

#include "common.h"

#define BIN_PATH_LEN	254
#define FRONT_CMD_LEN	300

/*
 * The state of a task. For the time being
 * only the first 2 states are used.
 */
enum task_state {
	ready = 0,
	running,
	stopped,
	done
};

/*
 * The state of a node. If a node goes down then it gets removed
 * from node list.
 */
enum node_state {
	available = 0,
	busy
};

/*
 * Type of message between worker threads and main thread
 */
enum msg_type {
	task_new = 0,
	node_new,
	node_ret,	// A node reported back the sult of a task execution
	node_down,
	migration
};

struct task {
	uint32_t id;
	char *bin_path;
	uint8_t priority;
	enum task_state state;
	struct node *node;	// the node where the task has been deployed
	struct task *next;
	struct task *prev;
};

struct node {
	uint32_t id;
	enum node_state state;
	// the thread which handles the communication with a node
	pthread_t thr;
	// eventfd used to send commands to the worker thead
	int ev_fd;
	int snd_efd;		// eventfd used to send commands to main thread
	struct in_addr ipv4;;	// ip address of connected node
	struct task *task;	// the task which has been deployed in this node
	struct node *next;
};

/*
 * A struct which contains info regarding the result of execution in a node
 * The thr field is used to identify the node where the task was running
 */
struct node_result {
	pthread_t thr;
	int res;
};

/*
 * A struct containing the id of the task which will be migrated and the id
 * of new node.
 */
struct mig_info {
	uint32_t tsk_id;
	uint32_t node_id;
};

/*
 * Struct which contains a message from worker threads to main thread
 * The message can be:
 * - A new task
 * - An event regarding a node
 * - Result of task execution
 * - Info abour migration request
 */
struct notify_msg {
	enum msg_type type;
	union {
		struct node *node;
		struct task *tsk;
		struct node_result nres;
		struct mig_info migr_info;
	};
};

/*
 * Struct which contains a message from main thread to worker threads
 * There are 2 types of messages. A new task to deploy or
 * a command to migrate current task to node pointed in this struct.
 */
struct msg_to_worker {
	enum mnode_type type;
	union {
		struct task *tsk;
		struct node *node;
	};
};

/*
 * Struct which is used to pass info in the worker threads.
 */
struct thr_data {
	int socket;
	int rcv_efd; // eventfd where worker thread will receive new messages
	int snd_efd; // eventfd where worker thread will send new messages
	struct node *node; // the node associated with the current thread
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
static struct task *create_new_task(char *path, uint8_t priority)
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
	new_task->priority = priority;
	new_task->node = NULL;;
	new_task->state = ready;
	new_task->next = NULL;
	new_task->prev = NULL;

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
	int con, rc = 0;
	char front_cmd[FRONT_CMD_LEN] = {0};
	struct thr_data *td = (struct thr_data *) arg;
	struct task *tsk_to_add = NULL;
	struct notify_msg *nmsg = NULL;

	rc = pthread_detach(pthread_self());
	if (rc < 0) {
		perror("detach thread");
		return NULL;
	}

	con = accept(td->socket, NULL, NULL);
	if (con == -1) {
		perror("frontend socket accept");
		return NULL;
	}

	rc = read(con, front_cmd, FRONT_CMD_LEN);
	if (rc <= 0) {
		err_print("Read from peer failed\n");
		goto exit_front;
	}

	/*
	 * Determine if it is a command or a path to a new binary
	 * New task should start with '/' since full path is required.
	 */
	if (front_cmd[0] == 'N') {
		uint8_t prior = 2;
		char path_bin[BIN_PATH_LEN] = {0};

		rc = sscanf(front_cmd, "New: %s priority: %hhu", path_bin, &prior);
		if (rc < 2) {
			err_print("Invalid command\n");
			goto exit_front;
		}

		tsk_to_add = create_new_task(path_bin, prior);
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
		if (rc < 0) {
			perror("write new task in eventfd");
			free(nmsg);
		}
	} else if (front_cmd[0] == 'T') {
		uint32_t tsk_ref, node_ref;

		/*
		 * Read the id of the task which will get migrated and
		 * the new node ehre the migrated task will resume execution
		 */
		rc = sscanf(front_cmd, "Task: %u Node: %u", &tsk_ref, &node_ref);
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
		if (rc < 0) {
			perror("write new task in eventfd");
			free(nmsg);
		}
	} else {
		err_print("Received unknown command: %s\n", front_cmd);
	}

exit_front:
	close(con);
	free(td);
	return NULL;
}

/*
 * Find the task which should get migrated and
 * notify current node regarding the migration request.
 * 
 * IMPORTANT:
 * For the time being the primary scheduler does not check if migration was
 * successful or not. As long as the command has been sent the task is
 * considered migrated to the new node. In case any error happens, both nodes
 * will go down and the task will become ready for execution. We might need
 * to revisit this in the future.
 */
static void handle_migration_cmd(struct mig_info minfo,struct node *nhead,
				 struct task *thead)
{
	struct node *node_tmp, *node_cur;
	struct task *tsk_tmp;
	ssize_t rc;
	struct msg_to_worker *msg_wrk;

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
	if (node_tmp->state == busy) {
		err_print("Destination node is busy\n");
		return;
	}

	node_cur = tsk_tmp->node;
	/*
	 * Notify the worker to send the migration command to the respective
	 * node.
	 */
	msg_wrk = malloc(sizeof(struct msg_to_worker));
	if (!msg_wrk) {
		err_print("Out of memory\n");
		return;
	}
	msg_wrk->type = migrate;
	msg_wrk->node = node_tmp;

	rc = write(node_cur->ev_fd, &msg_wrk, sizeof(uint64_t));
	if (rc < sizeof(uint64_t)) {
		err_print("Could not write to eventfd\n");
		free(msg_wrk);
		return;
	}
	/*
	 * Change status of the nodes
	 */
	node_tmp->state = busy;
	node_cur->state = available;
	node_cur->task = NULL;
	tsk_tmp->node = node_tmp;
	node_tmp->task = tsk_tmp;
}

/*
 * Send migration command to the node where task is currently tunning
 * Primary scheduler does not get involved in the migration process.
 * The current node will receive the migration and command, request ukvm
 * to ave the vm and transfer the needed files to the new node.
 */
static ssize_t send_migration_command(struct node *nd, int socket)
{
	ssize_t rc;
	struct com_nod nod_com;

	nod_com.type = mig_cmd;
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

/*
 * Handle events from worker's epoll.
 * There can be just 2 events:
 * - An event from the connected node
 * - A new command from main thread
 */
static int handle_node_comm(int epollfd, int con, int sched_efd, int snd_efd)
{
	int rc;
	int epoll_ret, i;
	struct epoll_event events[2];

	epoll_ret = epoll_wait(epollfd, events, 2, -1);
	if (epoll_ret == -1) {
		perror("worker epoll wait");
		return -1;
	}

	for (i = 0; i < epoll_ret; i++) {
		if (events[i].data.fd == con) {
			/*
			 * Something happened in the connected node.
			 * Read it and report it back to main thread.
			 */
			struct notify_msg *nmsg = NULL;
			int node_msg = 0;

			rc = read(con, &node_msg, sizeof(int));
			if (rc == 0) {
				printf("Connection closed\n");
				return -1;
			} else if (rc < sizeof(int)) {
				err_print("Short read in node's message\n");
				return -1;
			}
			/*
			 * The node notified that the task was stopped to
			 * generate a migration file successfully.
			 * For the time being we ignore that info, but
			 * we might want to revisit this in the future.
			 */
			if (node_msg == 7)
				continue;

			nmsg = malloc(sizeof(struct notify_msg));
			if (!nmsg) {
				err_print("Out of memory\n");
				return -1;
			}
			nmsg->type = node_ret;
			nmsg->nres.thr = pthread_self();
			nmsg->nres.res = node_msg;

			rc = write(snd_efd, &nmsg, sizeof(uint64_t));
			if (rc < 0) {
				perror("notify main thread for task result");
				return -1;
			}
		} else if (events[i].data.fd == sched_efd) {
			/*
			 * Handle a new command from main thread.
			 */
			uint64_t efval;
			struct msg_to_worker *msg_node;

			rc = read(sched_efd, &efval, sizeof(uint64_t));
			if (rc < sizeof(uint64_t)) {
				err_print("Read from eventfd failed");
				return -1;
			}

			msg_node = (struct msg_to_worker *) efval;
			if (msg_node->type == deploy)
				rc = send_file(con, msg_node->tsk->bin_path, deploy);
			else if (msg_node->type == migrate)
				rc = send_migration_command(msg_node->node, con);
			free(msg_node);
			if (rc < 0)
				return -1;

		}
	}
	return 0;
}

/*
 * Create and initialize an entry for a new node
 */
static struct node *create_new_node(struct thr_data *td, int sfd)
{
	struct node *nd = NULL;
	struct sockaddr_in saddr_node;
	socklen_t slen;

	/*
	 * Create new node
	 */
	nd = malloc(sizeof(struct node));
	if (!nd) {
		err_print("Out of memory\n");
		return NULL;;
	}
	memset(nd, 0, sizeof(struct node));
	/*
	 * Get ip of the new node
	 */
	slen = sizeof(struct sockaddr_in);
	memset(&saddr_node, 0, slen);
	if (getsockname(sfd, (struct sockaddr *) &saddr_node, &slen) < 0) {
		perror("Get ip from socket\n");
		free(nd);
		return NULL;
	}
	nd->ipv4 = saddr_node.sin_addr;
	nd->thr = pthread_self();
	nd->task = NULL;
	nd->ev_fd = td->rcv_efd;
	nd->snd_efd = td->snd_efd;
	nd->state = available;

	return nd;
}

/*
 * Handle the communication with a node.
 */
static void *node_communication(void *arg)
{
	int con, rc, epollfd = 0;
	struct thr_data *td = (struct thr_data *) arg;
	struct epoll_event ev;
	struct notify_msg *nmsg = NULL;
	struct node *nd = NULL;

	rc = pthread_detach(pthread_self());
	if (rc < 0) {
		perror("detach thread");
		return NULL;
	}

	con = accept(td->socket, NULL, NULL);
	if (con == -1) {
		perror("node socket accept");
		free(td);
		return NULL;
	}

	nd = create_new_node(td, con);
	if (!nd) {
		err_print("Could not create new node\n");
		goto err_node;
	}

	/*
	 * Notify main thread for the new node
	 */
	nmsg = malloc(sizeof(struct notify_msg));
	if (!nmsg) {
		err_print("Out of memory\n");
		goto err_node;
	}
	nmsg->type = node_new;
	nmsg->node = nd;
	rc = write(td->snd_efd, &nmsg, sizeof(uint64_t));
	if (rc < 0) {
		perror("write new node in eventfd");
		goto err_node;
	}

	/*
	 * Worker thread epoll. There can be 2 events.
	 * - A message from main thread (spawn/migrate task).
	 * - A message from the node (task terminated/node down)
	 */
	epollfd = epoll_create1(0);
	if (epollfd == -1) {
		perror("node_communication: epoll_create1");
		goto err_node;
	}

	ev.events = EPOLLIN;
	ev.data.fd = con;
	if (epoll_ctl(epollfd, EPOLL_CTL_ADD, con, &ev) == -1) {
		perror("epoll_ctl: node con socket");
		goto err_node;
	}

	ev.events = EPOLLIN;
	ev.data.fd = td->rcv_efd;
	if (epoll_ctl(epollfd, EPOLL_CTL_ADD, td->rcv_efd, &ev) == -1) {
		perror("epoll_ctl: node eventfd");
		goto err_node;
	}

	while(1)
	{
		rc = handle_node_comm(epollfd, con, td->rcv_efd, td->snd_efd);
		if (rc < 0)
			goto err_node;
	}

err_node:
	/*
	 * An error occurred and the thread will stop. As a result
	 * main thread should get notified that the node is down
	 * A new connection has to get established.
	 */
	close(con);
	nmsg = malloc(sizeof(struct notify_msg));
	if (!nmsg) {
		err_print("Out of memory for notify msg\n");
		goto err_node2;
	}
	nmsg->type = node_down;
	nmsg->nres.thr = pthread_self();

	rc = write(td->snd_efd, &nmsg, sizeof(uint64_t));
	if (rc < 0)
		perror("write new task in eventfd");
err_node2:
	if (epollfd)
		close(epollfd);
	free(td);
	return NULL;
}

/*
 * Handle events for main thead.
 * If the event is on a socket a new thread will get spawned,
 * otherwise the received message will be forwarded to main function.
 */
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

		worker_data = malloc(sizeof(struct thr_data));
		if (!worker_data) {
			err_print("Out of memory\n");
			return -1;
		}
		worker_data->socket = nsock;
		worker_data->rcv_efd = 0;
		worker_data->snd_efd = 0;
		/* 
		 * eventfd for messages to the worker
		 */
		rc = eventfd(0, EFD_CLOEXEC);
		if (rc < 0) {
			perror("Eventfd for frontend socket");
			goto err_handle;
		}
		worker_data->rcv_efd = rc;

		/* 
		 * eventfd for messages from the worker
		 */
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
			perror("Create node worker");
			goto err_handle_ev;
		}
	} else {
		/*
		 * Message from a worker thread
		 */
		uint64_t eeval;

		rc = read(fd, &eeval, sizeof(uint64_t));
		if (rc < 0) {
			perror("Read eventfd");
			return -1;
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

/*
 * Remove a task from the task list.
 */
static inline void remove_task(struct task **thd, struct task **tlst,
			struct task *task_tmp)
{
	struct task *tsk_head = *thd;
	struct task *tsk_last = *tlst;
	//struct task *task_tmp = *trmv;

	if (task_tmp->id == tsk_head->id)
		tsk_head = task_tmp->next;
	if (task_tmp->id == tsk_last->id)
		tsk_last = task_tmp->prev;
	*thd = tsk_head;
	*tlst = tsk_last;
	return;
}

/*
 * Insert a task in a task list.
 */
static inline void insert_task(struct task **thd, struct task **tlst,
				struct task *new)
{
	struct task *tsk_head = *thd;
	struct task *tsk_last = *tlst;

	if (tsk_head == NULL) {
		tsk_head = tsk_last = new;
	} else {
		tsk_last->next = new;
		new->prev = tsk_last;
		tsk_last = new;
	}
	*thd = tsk_head;
	*tlst = tsk_last;
	return;
}

/*
 * Remove a node from the node list.
 */
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
			printf("%d ", node_tmp->id);
			close(node_tmp->snd_efd);
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
		if ((node_tmp->state == available) && !node_avail)
			node_avail = node_tmp;
		node_tmp = node_tmp->next;
	}
	printf("----------------------------------------\n");
	printf("----------------- Tasks ----------------\n");
	tsk_tmp = thead;
	while(tsk_tmp) {
		printf("Task id %d with state %d, priority %hhu and bin at %s\n", tsk_tmp->id,tsk_tmp->state, tsk_tmp->priority, tsk_tmp->bin_path);
		if ((tsk_tmp->state == ready) && !tsk_avail)
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
	struct task *htsk_head = NULL, *htsk_last = NULL;
	struct task *ltsk_head = NULL, *ltsk_last = NULL;
	struct epoll_event events[MAX_EVENTS];
	int rc = 0, epoll_ret, epollfd, front_sock, nodes_sock;
	struct sockaddr_un saddr_un = {0};
	struct sockaddr_in saddr_in = {0};

	epollfd = epoll_create1(0);
	if (epollfd == -1) {
		perror("epoll_create1");
		exit(EXIT_FAILURE);
	}

	/*
	 * Create unix domain socket for frontend, where new tasks are reported.
	 */
	saddr_un.sun_family = AF_UNIX;
	strcpy(saddr_un.sun_path, FRONT_SOCK);
	unlink(FRONT_SOCK);
	front_sock = setup_socket(epollfd, (struct sockaddr *) &saddr_un, 1);
	if (front_sock < 0) {
		err_print("Could not setup_frontend socket\n");
		goto fail_pol;
	}

	/*
	 * Open a socket as a server for nodes
	 */
	saddr_in.sin_family = AF_INET;
	saddr_in.sin_addr.s_addr = htonl(INADDR_ANY);
	saddr_in.sin_port = htons(NODES_PORT);
	nodes_sock = setup_socket(epollfd, (struct sockaddr *) &saddr_in, 1);
	if (front_sock < 0) {
		err_print("Could not setup nodes socket\n");
		goto fail_soc;
	}

	printf("Listesning commands at %s\n", FRONT_SOCK);
	printf("Waiting for nodes to connect in port %d\n", NODES_PORT);

	while(1) {
		int i;
		struct task *tsk_avail = NULL;
		struct node *node_avail = NULL;
		struct msg_to_worker *msg_work;

		/*
		 * Main thread waits for events from:
		 * - front end socket
		 * - server socket for new new nodes
		 * - shared eventfds with the node worker threads
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
				 * Worker thread will handle and report it.
				 */
				continue;
			}

			switch(new_msg->type) {
			case node_new:
				if (node_head == NULL) {
					node_head = new_msg->node;
					node_last = new_msg->node;
					node_last->id = 1;
				} else {
					new_msg->node->id = node_last->id + 1;
					node_last->next = new_msg->node;
					node_last = new_msg->node;
				}
				break;
			case task_new:
				new_msg->tsk->id = nr_tsks++;

				if (new_msg->tsk->priority == 0)
					insert_task(&htsk_head, &htsk_last,
							new_msg->tsk);
				else if (new_msg->tsk->priority == 1)
					insert_task(&ltsk_head, &ltsk_last,
							new_msg->tsk);
				close(events[i].data.fd);
				break;
			case node_down:
				printf("Node with id ");
				remove_node(&node_head, &node_last, new_msg->nres.thr);
				printf("went down\n");
				break;
			case node_ret:
				struct node *node_tmp;
				struct task *task_tmp;

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
				task_tmp = node_tmp->task;
				node_tmp->state = available;
				node_tmp->task = NULL;

				if (task_tmp->next != NULL)
					task_tmp->next->prev = task_tmp->prev;
				if (task_tmp->prev != NULL) {
					task_tmp->prev->next = task_tmp->next;
				}
				printf("Task with id %d ", task_tmp->id);
				if (new_msg->nres.res == 4)
					printf("terminated successfully\n");
				else if (new_msg->nres.res == 5)
					printf("failed\n");
				if (task_tmp->priority == 0) {
					remove_task(&htsk_head, &htsk_last,
							task_tmp);
				}else if (task_tmp->priority == 1) {
					remove_task(&ltsk_head, &ltsk_last,
							task_tmp);
				}
				free(task_tmp->bin_path);
				free(task_tmp);
				break;
			case migration:
				handle_migration_cmd(new_msg->migr_info,
						     node_head, htsk_head);
				close(events[i].data.fd);
				break;
			default:
				err_print("Error while handling event\n");
				close(events[i].data.fd);
				goto fail_socs;
			}
			free(new_msg);
		}

		if (htsk_last != NULL) {
			htsk_last->next = ltsk_head;
			scheduler_algorithm(node_head, htsk_head, &node_avail,
					&tsk_avail);
			htsk_last->next = NULL;
		} else {
			scheduler_algorithm(node_head, ltsk_head, &node_avail,
					&tsk_avail);
		}
		if (!tsk_avail || !node_avail)
			continue;
		/*
		 * Deploy
		 */
		msg_work = malloc(sizeof(struct msg_to_worker));
		if (!msg_work) {
			err_print("Out of memory\n");
			goto fail_socs;
		}
		msg_work->type = deploy;
		msg_work->tsk = tsk_avail;

		rc = write(node_avail->ev_fd, &msg_work, sizeof(uint64_t));
		if (rc < sizeof(uint64_t)) {
			err_print("Could not write to eventfd\n");
			free(msg_work);
			continue;
		}

		tsk_avail->state = running;
		tsk_avail->node = node_avail;
		node_avail->task = tsk_avail;
		node_avail->state = busy;
		if (nr_tsks == 10)
			break;
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
