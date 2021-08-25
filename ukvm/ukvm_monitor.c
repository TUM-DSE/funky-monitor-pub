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
#include "ukvm_cpu_x86_64.h"

#include "funky_backend_core.h"

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
static char *save_file;

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

        if (strlen(com_mon) <= 7)
            return; /* Do nothing */
        if (atomic_read(&vm_state) == 3)
            return; /* Do nothing */

        /* 
         * Before pausing VM, check if FPGA is ready to be migrated. 
         * If not, wait for the next sync point.  
         */
        if(is_fpga_worker_alive())
        {
            printf("MON-THR: start save_fpga() ...\n");

            // 0. receive an initial msg from Worker
            struct thr_msg* init_msg = recv_msg_from_worker();
            if(init_msg->msg_type != MSG_INIT)
                printf("Warning: not MSG_INIT \n");
              

            // 1. Send "save_fpga" req to Worker
            struct thr_msg msg = {MSG_SAVEFPGA, NULL, 0};
            send_msg_to_worker(&msg);

            // 2. Wait for a response from Worker (sync point e.g., clFinish(), clWaitForEvents())
            struct thr_msg* sync_msg = recv_msg_from_worker();
            if(sync_msg->msg_type != MSG_SYNCED)
                printf("Warning: not MSG_SYNCED \n");

            /* FPGA is synced now */
        }

        save_file = strtok(com_mon, " ");
        save_file = strtok(NULL, " ");
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

char *handle_load(char *cmdarg)
{
    size_t path_len = strlen(cmdarg);
    char *file;
    int rc;

    file = malloc(path_len);
    if (!file)
        errx(1, "Out of memory");

    rc = sscanf(cmdarg, "--load=%s", file);
    if (rc != 1) {
        errx(1, "Malformed argument to --load");
    }
    warn("I will load state from file %s", file);

    return file;
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

#define MSR_IA32_TSC                    0x10

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
    struct {
        struct kvm_msrs info;
        struct kvm_msr_entry entries[1];
    } msr_data = {};
    uint64_t tsc;

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

    memset(&msr_data, 0, sizeof(msr_data));
    msr_data.info.nmsrs = 1;
    msr_data.entries[0].index = MSR_IA32_TSC;
    if (ioctl(hv->b->vcpufd, KVM_GET_MSRS, &msr_data) == -1 ) {
        warn("savevm: KVM: ioctl(KVM_GET_REGS) failed");
        return;
    }
    tsc = msr_data.entries[0].data;
    assert(tsc != 0);

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

    nbytes = write(fd, &msr_data, sizeof(msr_data));
    if (nbytes < 0) {
        warn("savevm: Error writing kvm_regs");
        return;
    }
    else if (nbytes != sizeof(msr_data)) {
        warnx("savevm: Short write() writing msr_data: %zd", nbytes);
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

long loadvm(char *load_file, struct ukvm_hv *hv)
{
    int fd, ret;
    struct ukvm_hvb *hvb = hv->b;
    struct kvm_sregs sregs;
    struct kvm_regs kregs;
    long page_size;
    size_t total_pgs = 0;
    struct {
        struct kvm_msrs info;
        struct kvm_msr_entry entries[1];
    } msr_data = {};

    fd = open(load_file, O_RDONLY);
    if (fd < 0) {
        warn("loadvm: open(%s)", load_file);
        return -1;
    }

    ukvm_x86_setup_gdt(hv->mem);
    ukvm_x86_setup_pagetables(hv->mem, hv->mem_size);

    setup_cpuid(hvb);

    ret = read(fd, &kregs, sizeof(struct kvm_regs));
    if (ret < sizeof(struct kvm_regs)) {
        if (ret < 0)
            warnx("Could not read kregs");
        warnx("Incomplete read of kregs\n");
    }

    ret = read(fd, &sregs, sizeof(struct kvm_sregs));
    if (ret < sizeof(struct kvm_sregs)) {
        if (ret < 0)
            warnx("Could not read sregs");
        warnx("Incomplete read of sregs\n");
    }

    ret = read(fd, &msr_data, sizeof(msr_data));
    if (ret < sizeof(msr_data)) {
        if (ret < 0)
            warnx("Could not read msr_data");
        warnx("Incomplete read of msr_data\n");
    }

    ret = ioctl(hvb->vcpufd, KVM_SET_SREGS, &sregs);
    if (ret == -1)
        err(1, "loadvm: KVM ioctl (SET_SREGS) failed");

    ret = ioctl(hvb->vcpufd, KVM_SET_REGS, &kregs);
    if (ret == -1)
        err(1, "loadvm: KVM ioctl (SET_REGS) failed");

    ret = ioctl(hvb->vcpufd, KVM_SET_MSRS, &msr_data);
    if (ret == -1)
        err(1, "loadvm: KVM ioctl (SET_MSRS) failed");

    ret = read(fd, &page_size, sizeof(long));
    if (ret < sizeof(long)) {
        if (ret < 0)
            warnx("Could not read pge_size");
        warnx("Incomplete read of page_size\n");
    }
    ret = read(fd, &total_pgs, sizeof(size_t));
    if (ret < sizeof(size_t)) {
        if (ret < 0)
            warnx("Could not read total pages number");
        warnx("Incomplete read of page_size\n");
    }
    warnx("loadvm: I need to read %ld pages", total_pgs);
    for(int i = 0; i < total_pgs; i++) {
        off_t pgoff;
        size_t pg;
        ssize_t nbytes = read(fd, &pg, sizeof(size_t));
        if (nbytes == -1) {
            warn("loadvm: Error reading offset of guest memory page %zd", pg);
            return -1;
        } else if (nbytes != sizeof(size_t)) {
            warnx("loadvm: Short read on guest memory page "
                    "%zd: %zd bytes", pg, nbytes);
        return -1;
        }
        pgoff = (pg * page_size);
        nbytes = read(fd, hv->mem + pgoff, page_size);
        if (nbytes == -1) {
            warn("loadvm: Error reading guest memory page %zd", pg);
            return -1;
        } else if (nbytes != page_size) {
            warnx("loadvm: Short read on guest memory page "
                    "%zd: %zd bytes", pg, nbytes);
            break;
        }
    }
    warnx("loadvm: loaded %ld pages with page size %ld", total_pgs, page_size);

    off_t offset = lseek(fd, 0, SEEK_CUR);

    close(fd);
    return offset;
}


/* 
 * void savefpga(struct ukvm_hv *hv)
 *
 * This function receives FPGA state from Worker thread and writes the data to save_file. 
 * Worker thread is destroyed at the end.
 */
void savefpga(struct ukvm_hv *hv)
{
    /* Receive messages from Worker */
    struct thr_msg* state_msg=NULL;
    struct thr_msg* data_msg=NULL;
    if(is_fpga_worker_alive()) {
        state_msg = recv_msg_from_worker();

        if(state_msg->msg_type == MSG_UPDATED)
            data_msg = recv_msg_from_worker();
    }

    /* update data header */
    struct fpga_data_header header = {0};
    if(state_msg) {
        // warn("savefpga(): Worker is alive. \n");
        header.sb_fpgainit = 1;
    }

    if(data_msg) {
        // warnx("savefpga(): got FPGA data, %lu Bytes at 0x%08lx\n", data_msg->size, (uint64_t) data_msg->data);
        header.data_size = data_msg->size;
    }

    /* open file */
    int fd = open(save_file, O_WRONLY, S_IRUSR | S_IWUSR);
    if (fd < 0) {
        warn("savefpga(): open(%s)", save_file);
        return;
    }
    off_t offset = lseek(fd, 0, SEEK_END);
    warnx("savefpga(): file is seeked to %lu\n", offset);

    /* write FPGA data header */
    size_t nbytes = write(fd, &header, sizeof(struct fpga_data_header));
    if (nbytes < 0) {
        warn("savefpga(): Error writing fpga_data_header");
        return;
    }

    /* write FPGA thread info */
    if(state_msg) {
        nbytes = write(fd, state_msg->data, sizeof(struct fpga_thr_info));
        if (nbytes < 0) {
            warn("savefpga(): Error writing fpga_thr_info");
            return;
        }
    }

    /* write FPGA data */
    if(data_msg) {
        nbytes = write(fd, data_msg->data, data_msg->size);
        if (nbytes < 0) {
            warn("savefpga(): Error writing fpga_data");
            return;
        }
    }

    if(state_msg)
        destroy_fpga_worker();
    else
        warn("savefpga(): Worker doesn't exist. \n");

    close(fd);
    return;
}

/* 
 * void loadfpga(char *load_file, long offset, struct ukvm_hv *hv)
 *
 * This function reads the previous FPGA state from save_file and recreates FPGA backend contexts. 
 * Worker thread is also created if necessary.
 */
void loadfpga(char *load_file, long offset, struct ukvm_hv *hv)
{
    int fd = open(load_file, O_RDONLY);
    if (fd < 0) {
        warn("loadfpga(): open(%s)", load_file);
        return;
    }

    lseek(fd, offset, SEEK_SET);

    /* read FPGA data header */
    struct fpga_data_header header;
    int ret = read(fd, &header, sizeof(struct fpga_data_header));
    if (ret < sizeof(struct fpga_data_header)) {
        if (ret < 0)
            warnx("Could not read fpga data header");
        warnx("Incomplete read of fpga data header\n");
    }

    /* read FPGA thread info */
    struct fpga_thr_info thr_info;
    if(header.sb_fpgainit) {
        ret = read(fd, &thr_info, sizeof(struct fpga_thr_info));
        if (ret < sizeof(struct fpga_thr_info)) {
            if (ret < 0)
                warnx("Could not read fpga thr info");
            warnx("Incomplete read of fpga thr info\n");
        }

        thr_info.hv = hv;
    }

    /* read FPGA data */
    if(header.data_size > 0) {
        size_t data_size = header.data_size;
        void* fpga_data = malloc(data_size);

        ret = read(fd, fpga_data, data_size);
        if (ret < data_size) {
            if (ret < 0)
                warnx("Could not read fpga thr info");
            warnx("Incomplete read of fpga thr info\n");
        }

        thr_info.mig_data = fpga_data;
        thr_info.mig_size = data_size;
    }

    /* create Worker thread */
    if(header.sb_fpgainit)
        create_fpga_worker(thr_info);
    else
        warnx("loadfpga(): Worker is not created. \n");

    close(fd);
    return;
}
