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
#include <linux/kvm.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <assert.h>
#include <sys/mman.h>

#include "ukvm.h"
#include "ukvm_hv_kvm.h"

#define COMMAND_LEN 24

typedef unsigned char *host_mvec_t;

/*
 * Data for the monitor thread
 */
struct mon_thr_data {
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
 * A global variable for the file in which VM will be stored
 */
 static char save_file[20];

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

    /*
     * Savevm command will save vm state in the file specified after the
     * command.
     */
    if (strncmp(com_mon, "savevm", 6) == 0) {
        int r;
        size_t filename_len;

        if (strlen(com_mon) <= 7)
            return; /* Do nothing */
        if (atomic_read(&vm_state) == 3)
            return; /* Do nothing */

        com_mon += 7;
        filename_len = strlen(com_mon);
        strncpy(save_file, com_mon, filename_len);
        warnx("I will save VM in file %s", save_file);
        atomic_set(&vm_state, 3);
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

void savevm(struct ukvm_hv *hv)
{
    int fd;
    struct kvm_regs kregs;
    struct kvm_sregs sregs;
    size_t nbytes;
    long page_size;
    size_t npages;
    size_t ndumped = 0;
    host_mvec_t mvec;
    off_t num_pgs_off, file_off;

    fd = open(save_file, O_WRONLY | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);
    if (fd < 0) {
        warn("savevm: open(%s)", save_file);
        return;
    }
    warnx("savevm: save guest to: %s", save_file);

    if (ioctl(hv->b->vcpufd, KVM_GET_SREGS, &sregs) == -1) {
        warn("savevm: KVM: ioctl(KVM_GET_SREGS) failed");
        return;
    }

    if (ioctl(hv->b->vcpufd, KVM_GET_REGS, &kregs) == -1) {
        warn("savevm: KVM: ioctl(KVM_GET_REGS) failed");
        return;
    }

    nbytes = write(fd, &kregs, sizeof(struct kvm_regs));
    if (nbytes < 0) {
        warn("savevm: Error writing kvm_regs");
        return;
    }
    else if (nbytes != sizeof(struct kvm_regs)) {
        warnx("savevm: Short write() writing kvm_regs: %zd", nbytes);
        return;
    }

    nbytes = write(fd, &sregs, sizeof(struct kvm_sregs));
    if (nbytes < 0) {
        warn("savevm: Error writing kvm_sregs");
        return;
    }
    else if (nbytes != sizeof(struct kvm_sregs)) {
        warnx("savevm: Short write() writing kvm_sregs: %zd", nbytes);
        return;
    }

    page_size = sysconf(_SC_PAGESIZE);
    if (page_size == -1) {
        warn("savevm: Could not determine _SC_PAGESIZE");
        return;
    }
    assert (hv->mem_size % page_size == 0);
    npages = hv->mem_size / page_size;
    mvec = malloc(npages);
    assert (mvec);
    if (mincore(hv->mem, hv->mem_size, mvec) == -1) {
        warn("savevm: mincore() failed");
        return;
    }
    nbytes = write(fd, &page_size, sizeof(long));
    if (nbytes == -1) {
        warn("savevm: Error writing page size");
        free(mvec);
        return;
    } else if (nbytes != sizeof(long)) {
        warnx("savevm: Short write in page size");
        free(mvec);
        return;
    }
    num_pgs_off = lseek(fd, 0, SEEK_CUR);
    file_off = num_pgs_off + sizeof(size_t);
    if (lseek(fd, file_off, SEEK_SET) != file_off) {
        warnx("savevm: COuld not set file offset");
        free(mvec);
        return;
    }
    for (size_t pg = 0; pg < npages; pg++) {
        if (mvec[pg] & 1) {
            off_t pgoff = (pg * page_size);
            ssize_t nbytes = write(fd, &pg, sizeof(size_t));
            if (nbytes == -1) {
                warn("savevm: Error dumping guest memory page %zd", pg);
                free(mvec);
                return;
            } else if (nbytes != sizeof(size_t)) {
                warnx("savevm: Short write dumping guest memory page"
                        "%zd: %zd bytes", pg, nbytes);
                free(mvec);
                return;
            }
            nbytes = write(fd, hv->mem + pgoff, page_size);
            if (nbytes == -1) {
                warn("savevm: Error dumping guest memory page %zd", pg);
                free(mvec);
                return;
            } else if (nbytes != page_size) {
                warnx("savevm: Short write dumping guest memory page"
                        "%zd: %zd bytes", pg, nbytes);
                free(mvec);
                return;
            }
            ndumped++;
        }
    }
    free(mvec);
    warnx("savevm: dumped %zd pages of total %zd pages", ndumped, npages);
    nbytes = pwrite(fd, &ndumped, sizeof(size_t), num_pgs_off);
    if (nbytes == -1) {
        warn("savevm: Error writing total saved pages %zd", ndumped);
        return;
    } else if (nbytes != sizeof(size_t)) {
        warnx("savevm: Short write on total saved pages"
                " %zd: %zd bytes", ndumped, nbytes);
        return;
    }
    close(fd);
}
