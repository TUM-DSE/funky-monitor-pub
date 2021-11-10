#ifndef SCHED_COMMON_H
#define SCHED_COMMON_H

#define FRONT_SOCK	"/tmp/front.sock"
#define MAX_EVENTS	10
#define NODES_PORT	4217

#define err_print(fmt, ...)	fprintf(stderr, "%s - %d: " fmt, __func__, \
					__LINE__, ##__VA_ARGS__)

/*
 * Type of message between primary scheduler and daemons
 * It is mainly used to distinguish commands from files.
 */
enum mnode_type {
	deploy = 0,
	mig_cmd,
	evict,
	resume,
	migrate,
	arguments
};

/*
 * A struct which contains the result of a task execution
 * In case the task which completed was the evicted one then the
 * is_evicted value wil lbe set.
 */
struct tsk_res {
	uint8_t exit_code;
	uint8_t is_evicted;
};

/*
 * A struct which contains a message to a node.
 * This message precedes file transmission and in case of migration
 * notifies the daemon about the new node where task will migrate.
 */
struct com_nod {
	enum mnode_type type;
	union {
		off_t size;
		struct in_addr rcv_ip;
	};
};

int setup_socket(int epollfd, struct sockaddr *saddr, uint8_t tobind);
ssize_t send_file(int socket, const char *filename, enum mnode_type msg_type);

#endif /* SCHED_COMMON_H */
