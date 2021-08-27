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

#define FRONT_SOCK "/tmp/front.sock"
#define BIN_PATH_LEN 24
#define MAX_EVENTS 10

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

struct thr_data {
	int socket;
	int efd;
};

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

	uint64_t mpla = 17;

	con = accept(td->socket, NULL, NULL);
	if (con == -1) {
		perror("frontend socket accept");
	}

	rc = read(con, bin_path, BIN_PATH_LEN);
	if (rc <= 0) {
		fprintf(stderr, "Read from peer failed");
		close(con);
	}
	/* Make sure we do not read rubbish, old commands etc */
	bin_path[(rc > BIN_PATH_LEN) ? BIN_PATH_LEN - 1: rc] = '\0';

	printf("New binary at %s\n", bin_path);
	tsk_to_add = create_new_task(bin_path);
	if (!tsk_to_add)
		fprintf(stderr, "Could not create new task\n");

	rc = write(td->efd, &tsk_to_add, sizeof(uint64_t));
	if (rc < 0)
		perror("write new task in eventfd");

	close(con);
}

int main()
{
	uint32_t nr_tsks = 0, nr_efds = 0;
	struct task *tsk_to_add = NULL;
	struct task *tsk_head = NULL, *tsk_last = NULL;
	struct epoll_event ev, events[MAX_EVENTS];
	int rc, epoll_ret, epollfd, front_sock, efds[MAX_EVENTS - 1];
	struct sockaddr_un front_sockaddr;
	struct task *tmp;
	uint64_t eeval;

	front_sock = socket(AF_UNIX, SOCK_STREAM, 0);
	if (front_sock == -1) {
		fprintf(stderr, "socket error %d\n", errno);
	}

	front_sockaddr.sun_family = AF_UNIX;
	strcpy(front_sockaddr.sun_path, FRONT_SOCK);
	unlink(FRONT_SOCK);
	rc = bind(front_sock, (struct sockaddr *) &front_sockaddr,
		  sizeof(struct sockaddr_un));
	if (rc == -1) {
		perror("socket bind");
		goto out_soc;
	}

	rc = listen(front_sock, MAX_EVENTS - 1);
	if (rc == -1) {
		perror("Frontend socket listen");
		goto out_soc;
	}

	epollfd = epoll_create1(0);
	if (epollfd == -1) {
		perror("epoll_create1");
		goto out_soc;
	}

	ev.events = EPOLLIN | EPOLLET;
	ev.data.fd = front_sock;
	if (epoll_ctl(epollfd, EPOLL_CTL_ADD, front_sock, &ev) == -1) {
		perror("epoll_ctl: frontend socket");
		goto out_pol;
	}

	printf("Listesning commands at %s\n", FRONT_SOCK);

	while(1) {
		int i;
		pthread_t worker;

		epoll_ret = epoll_wait(epollfd, events, MAX_EVENTS, -1);
		if (epoll_ret == -1) {
			perror("epoll_wait");
			goto out_pol;
		}

		for (i = 0; i < epoll_ret; i++) {
			if (events[i].events == 0)
				continue;
			if (events[i].data.fd == front_sock) {
				struct thr_data worker_data;

				rc = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
				if (rc < 0) {
					perror("Eventfd for frontend socket");
					goto out_pol;
				}

				worker_data.socket = front_sock;
				worker_data.efd = rc;
				ev.events = EPOLLIN | EPOLLET;
				ev.data.fd = rc;
				if (epoll_ctl(epollfd, EPOLL_CTL_ADD, rc, &ev) == -1) {
					perror("epoll_ctl: eventfd");
					goto out_pol;
				}

				rc = pthread_create(&worker, NULL, get_cmd_front,
						   (void *) &worker_data);
				if (rc) {
					perror("Frontend worker create");
					goto out_pol;
				}
			} else {
				rc = read(events[i].data.fd, &eeval, sizeof(uint64_t));
				if (rc < 0) {
					perror("Rread eventfd");
					goto out_pol;
				}
				tsk_to_add = (struct task *) eeval;
				tsk_to_add->id = nr_tsks++;
				if (close(events[i].data.fd) == -1)
					perror("Close eventfd");
			}

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

out_pol:
	close(epollfd);
out_soc:
	close(front_sock);
	exit(EXIT_FAILURE);
}
