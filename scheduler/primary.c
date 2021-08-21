#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <stdint.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>

#define FRONT_SOCK "/tmp/front.sock"
#define BIN_PATH_LEN 24

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

uint32_t nr_tsks;
pthread_mutex_t tasks_lock;
struct task *tsk_to_add;

static struct task *create_new_task(char *path)
{
	struct task *new_task;

	new_task = malloc(sizeof(struct task));
	if (!new_task) {
		fprintf(stderr, "Out of memory while creating new struct task\n");
		return NULL;
	}

	new_task->id = nr_tsks++;
	new_task->bin_path = strdup(path);
	new_task->node_id = 0;
	new_task->state = ready;
	new_task->next = NULL;

	return new_task;
}

static void *frontend_link(void *arg)
{
	struct sockaddr_un front_sockaddr;
	int rc, front_sock;
	pthread_t sched_thr = *(pthread_t *)arg;

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
		rc = errno;
		close(front_sock);
		fprintf(stderr, "Socket bind error: %d\n", rc);
	}

	rc = listen(front_sock, 5);
	if (rc == -1) {
		rc = errno;
		close(front_sock);
		fprintf(stderr, "Socket listen error: %d\n", rc);
	}

	printf("listesning commands at %s\n", FRONT_SOCK);
	for (;;) {
		int con;
		char bin_path[BIN_PATH_LEN];

		con = accept(front_sock, NULL, NULL);
		if (con == -1) {
			rc = errno;
			close(front_sock);
			fprintf(stderr, "Socket accept error: %d\n", rc);
		}

		rc = read(con, bin_path, BIN_PATH_LEN);
		if (rc <= 0) {
			close(con);
			fprintf(stderr, "Read from peer failed");
		}
		/* Make sure we do not read rubbish, old commands etc */
		/*
		 * Edw bazei ena new line...
		 */
		printf("bytes = %d\n", rc);
		bin_path[(rc > BIN_PATH_LEN) ? BIN_PATH_LEN - 1: rc] = '\0';

		printf("New binary at %s\n", bin_path);
		tsk_to_add = create_new_task(bin_path);
		if (!tsk_to_add)
			fprintf(stderr, "Could not create new task\n");

		rc = pthread_kill(sched_thr, SIGUSR1);
		if (rc < 0)
			perror("signal to scheduler thread\n");
		close(con);
	}
}

int main()
{
	pthread_t frontend_thr, sched_thr = pthread_self();
	sigset_t waitset;
	struct sigaction siga;
	struct task *tsk_head = NULL, *tsk_last = NULL;
	int rc, sig;

	struct task *tmp;

	rc  = pthread_mutex_init(&tasks_lock, NULL);
	if (rc !=0 ) {
		fprintf(stderr, "Could not init task mutex: %d\n", rc);
		exit(1);
	}

	nr_tsks = 0;

	siga.sa_handler = SIG_DFL;
	if(sigaction(SIGUSR1, &siga, NULL) == -1) {
		perror("Deleting SIGUSR1 handler");
		exit(1);
	}

	sigemptyset(&waitset);
	if(sigaddset(&waitset, SIGUSR1) == -1) {
		perror("Sigaddset scheduler thread");
		exit(1);
	}
	rc = pthread_sigmask(SIG_BLOCK, &waitset, NULL);
	if (rc) {
		fprintf(stderr, "Could not block SIG_DFL %d\n", rc);
		exit(1);
	}

	rc = pthread_create(&frontend_thr, NULL, frontend_link, (void *) &sched_thr);
	if (rc) {
		fprintf(stderr, "Could not create fontend thread %d\n", rc);
		exit(1);
	}

	while(1)
	{
		printf("helo\n");
		rc = sigwait(&waitset, &sig);
		if (rc == -1 && !(errno == EAGAIN || errno == EINTR)) {
			perror("Sigaddset scheduler thread");
			exit(1);
		}
		printf("helo 1\n");

		/*
		 * Got new binary?
		 */
		if (!tsk_to_add)
			continue;

		if (tsk_head == NULL) {
			tsk_head = tsk_last = tsk_to_add;
		} else {
			tsk_last->next = tsk_to_add;
			tsk_last = tsk_to_add;
		}

		tmp = tsk_head;
		while(tmp) {
			printf("Task with id %d and bin at %s\n", tmp->id, tmp->bin_path);
			printf("%s -- %s\n", tmp->bin_path, tmp->bin_path);
			tmp = tmp->next;
		}
	}
}
