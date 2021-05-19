#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <unistd.h>

#include "ukvm.h"

#define COMMAND_LEN 24

static void handle_mon_com(char *com_mon)
{
    size_t len;

    len = strlen(com_mon);
    if (com_mon[len - 1] == '\n')
            com_mon[len - 1] = '\0';

    if (strcmp(com_mon, "quit") == 0) {
            errx(1, "I got the quit command i will quit\n");
    }
    printf("Unknown command %s %ld\n", com_mon, strlen(com_mon));
}

static void *solo5_monitor(void *arg)
{
    char *sock_path = (char *) arg;
    struct sockaddr_un mon_sockaddr;
    int rc, mon_sock;

    mon_sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (mon_sock == -1) {
        errx(1, "socket error %d\n", errno);
    }

    mon_sockaddr.sun_family = AF_UNIX;
    strcpy(mon_sockaddr.sun_path, sock_path);
    unlink(sock_path);
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

    printf("listesning commands at %s\n", sock_path);
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

        handle_mon_com(mon_com);
        close(con);
    }

}

void handle_mon(char *cmdarg)
{
    char *sock_path;
    size_t path_len = strlen(cmdarg);
    int rc;
    pthread_t mon_thread;

    sock_path = malloc(path_len);
    if (!sock_path)
        errx(1, "out of memory");

    rc = sscanf(cmdarg, "--mon=%s", sock_path);
    if (rc != 1) {
        errx(1, "Malformed argument to --mon");
    }

    rc = pthread_create(&mon_thread, NULL, solo5_monitor, (void *) sock_path);
    if (rc) {
        errx(1, "Errorc creating monitor thread\n");
    }
}
