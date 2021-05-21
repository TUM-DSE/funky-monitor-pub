#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <unistd.h>

#include "ukvm.h"

#define COMMAND_LEN 24

/*
 * Data for the monitor thread
 */
struct mon_thr_data
{
    char *socket_path;  /* The socket for the monitor */
    pthread_t vm_thr;   /* The thread which runs vcpu */
};

/*
 * A global variable to keep track of the current state of the
 * vm. We need to access this variable from both threads.
 * As a result we need to be carefull when we access it.
 */
int vm_state;

/*
 * Handle the incomming commands
 */
static void handle_mon_com(char *com_mon, pthread_t thr)
{
    size_t len;

    len = strlen(com_mon);
    /* Discard the new line character from the command */
    if (com_mon[len - 1] == '\n')
        com_mon[len - 1] = '\0';

    /*
     * Stop command will result to a pause in the execution of vm.
     * We want to immediately stop the vcpu. KVM does not provide any
     * function to do that but we can force an exit by sending a signal
     * to the thread that executes the vm. KVM will see the pending signal
     * and return from KVM_RUN ioctl with -EINTR.
     */
    if (strcmp(com_mon, "stop") == 0) {
        int r;

        atomic_set(&vm_state, 1);
        r = pthread_kill(thr, SIGUSR1);
        if (r < 0)
            perror("pthread_kill stop");
        return;
    }

    /*
     * Resume command will resume a previous stopped vm.
     * When a vm is stopped its thread waits till it gets a SIGUSR1
     * signal and a change in the vm_state variable.
     */
    if (strcmp(com_mon, "resume") == 0) {
        int r;

        if (atomic_read(&vm_state) != 1)
            return; /* Do nothing */
        atomic_set(&vm_state, 2);
        r = pthread_kill(thr, SIGUSR1);
        if (r < 0)
            perror("pthread_kill stop");
        return;
    }

    if (strcmp(com_mon, "quit") == 0) {
        errx(1, "I got the quit command. I will quit\n");
    }

    printf("Unknown command %s %ld\n", com_mon, strlen(com_mon));
}

/*
 * Solo5 monitor thread. It will create a new socket and wait for
 * commands. One command per connection
 */
static void *solo5_monitor(void *arg)
{
    struct mon_thr_data *thr_data = (struct mon_thr_data *) arg;
    struct sockaddr_un mon_sockaddr;
    int rc, mon_sock;

    mon_sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (mon_sock == -1) {
        errx(1, "socket error %d\n", errno);
    }

    mon_sockaddr.sun_family = AF_UNIX;
    strcpy(mon_sockaddr.sun_path, thr_data->socket_path);
    unlink(thr_data->socket_path);
    rc = bind(mon_sock, (struct sockaddr *) &mon_sockaddr, sizeof(struct sockaddr_un));
    if (rc == -1) {
        rc = errno;
        close(mon_sock);
        errx(1, "Socket bind error: %d\n", rc);
    }

    rc = listen(mon_sock, 5);
    if (rc == -1) {
        rc = errno;
        close(mon_sock);
        errx(1, "Socket listen error: %d\n", rc);
    }

    printf("listesning commands at %s\n", thr_data->socket_path);
    for (;;) {
        int con;
        char mon_com[COMMAND_LEN];

        con = accept(mon_sock, NULL, NULL);
        if (con == -1) {
            rc = errno;
            close(mon_sock);
            errx(1, "Socket accept error: %d\n", rc);
        }

        rc = read(con, mon_com, COMMAND_LEN);
        if (rc <= 0) {
                close(con);
                errx(1, "Read from peer failed");
        }
        /* Make sure we do not read rubbish, old commands etc */
        mon_com[(rc > COMMAND_LEN) ? COMMAND_LEN - 1: rc] = '\0';

        handle_mon_com(mon_com, thr_data->vm_thr);
        close(con);
    }
    free(thr_data->socket_path);
    free(thr_data);

}

void handle_mon(char *cmdarg)
{
    struct mon_thr_data *thr_data;
    size_t path_len = strlen(cmdarg);
    int rc;
    pthread_t mon_thread;

    thr_data = malloc(sizeof(struct mon_thr_data));
    if (!thr_data)
        errx(1, "out of memory");
    thr_data->socket_path = malloc(path_len);
    if (!thr_data->socket_path)
        errx(1, "out of memory");

    rc = sscanf(cmdarg, "--mon=%s", thr_data->socket_path);
    if (rc != 1) {
        errx(1, "Malformed argument to --mon");
    }

    thr_data->vm_thr = pthread_self();
    rc = pthread_create(&mon_thread, NULL, solo5_monitor, (void *) thr_data);
    if (rc) {
        errx(1, "Errorc creating monitor thread\n");
    }
}

/*
 * Signal handler. For the time being it is just consumes the signal
 */
static void ipi_signal(int sig)
{
    printf("hello thereeeee\n");
}

void init_cpu_signals()
{
    int r;
    sigset_t set;
    struct sigaction sigact;

    memset(&sigact, 0, sizeof(struct sigaction));
    sigact.sa_handler = ipi_signal;
    sigaction(SIGUSR1, &sigact, NULL);

    pthread_sigmask(SIG_BLOCK, NULL, &set);
    sigdelset(&set, SIGUSR1);
    r = pthread_sigmask(SIG_SETMASK, &set, NULL);
    if (r) {
        errx(1, "set signal mask %d\n", r);
    }
}
