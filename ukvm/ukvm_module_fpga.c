/* 
 * Copyright (c) 2015-2017 Contributors as noted in the AUTHORS file
 *
 * This file is part of ukvm, a unikernel monitor.
 *
 * Permission to use, copy, modify, and/or distribute this software
 * for any purpose with or without fee is hereby granted, provided
 * that the above copyright notice and this permission notice appear
 * in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 * WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
 * AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS
 * OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT,
 * NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * ukvm_module_fpga.c: Block device module.
 */

#define _GNU_SOURCE
#include <assert.h>
#include <err.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "ukvm.h"

#include "funky_backend_core.h"

// static struct ukvm_fpgainfo fpgainfo;
static char *fpganame;
// static int fpgafd;

static void hypercall_fpgainfo(struct ukvm_hv *hv, ukvm_gpa_t gpa)
{
    struct ukvm_fpgainfo *fpga =
        UKVM_CHECKED_GPA_P(hv, gpa, sizeof (struct ukvm_fpgainfo));

#ifndef EVAL_HYPERCALL_OH
    printf("\n*** entering monitor by hypercall...***\n\n");

    char* binfile = "vadd.xclbin";
    hello_fpga(binfile);
#endif

    fpga->ret = 0;

    // printf("\n***  exiting monitor... ***\n\n");
}

static void hypercall_fpgainit(struct ukvm_hv *hv, ukvm_gpa_t gpa)
{
    printf("UKVM: set up fpga...\n");

    struct ukvm_fpgainit *fpga =
        UKVM_CHECKED_GPA_P(hv, gpa, sizeof (struct ukvm_fpgainit));

    // TODO: reserves the pointer to bitstream (offset) for migration
    void* bitstream = UKVM_CHECKED_GPA_P(hv, fpga->bs, fpga->bs_len);
    void* wr_queue_addr = UKVM_CHECKED_GPA_P(hv, fpga->wr_queue, fpga->wr_queue_len);
    void* rd_queue_addr = UKVM_CHECKED_GPA_P(hv, fpga->rd_queue, fpga->rd_queue_len);

    // TODO: check here if any FPGA is available for the guest (unikernel)
    /**
     * If available, ukvm allocate FPGA to the guest and 
     * instanciates an FPGA backend context so that the guest can offload tasks to FPGA. 
     * If not, the guest has to wait until the FPGA is freed by another guest (or/and? transit to a migratable state)
     */

    // TODO: FPGA resource allocation depends on the scheduler.
    //       The request is always approved now because the scheduler doesn't exist.
    if(wr_queue_addr && rd_queue_addr) {
      allocate_fpga(wr_queue_addr, rd_queue_addr);
      // register_cmd_queues(wr_queue_addr, rd_queue_addr);
    }

    if(bitstream) {
      save_bitstream(fpga->bs, fpga->bs_len);
      reconfigure_fpga(bitstream, fpga->bs_len);
      // reconfigure_fpga(bitstream, fpga->bs_len);
    }
    
    /* following processes are just for a test: validating Funky backend functions 
     * TODO: remove here and develop new hypercalls to handle requests, to release FPGA
     */

    /* check the queue and invoke the request handler if any request is enqueued. */
    int num_of_reqs = handle_fpga_requests(hv);
    printf("UKVM: %d requests are processed.\n", num_of_reqs);

    // printf("\n***  exiting monitor... ***\n\n");
    fpga->ret = 0;
}

static void hypercall_fpgafree(struct ukvm_hv *hv, ukvm_gpa_t gpa)
{
    printf("UKVM: release FPGA.\n");
    /* release the FPGA */
    release_fpga();
}

static void hypercall_fpgareq(struct ukvm_hv *hv, ukvm_gpa_t gpa)
{
    printf("UKVM: handle requests...\n");

    int* retired_reqs =
        UKVM_CHECKED_GPA_P(hv, gpa, sizeof (int));

    *retired_reqs = handle_fpga_requests(hv);

    printf("UKVM: %d requests are processed.\n", *retired_reqs);
}

static int handle_cmdarg(char *cmdarg)
{
    if (strncmp("--fpga=", cmdarg, 7))
        return -1;
    fpganame = cmdarg + 7;

    return 0;
}

static int setup(struct ukvm_hv *hv)
{
    printf("monitor: register FPGA hypercalls.\n");

    assert(ukvm_core_register_hypercall(UKVM_HYPERCALL_FPGAINFO,
                hypercall_fpgainfo) == 0);

    assert(ukvm_core_register_hypercall(UKVM_HYPERCALL_FPGAINIT,
                hypercall_fpgainit) == 0);

    assert(ukvm_core_register_hypercall(UKVM_HYPERCALL_FPGAREQ,
                hypercall_fpgareq) == 0);

    assert(ukvm_core_register_hypercall(UKVM_HYPERCALL_FPGAFREE,
                hypercall_fpgafree) == 0);

    return 0;
}

static char *usage(void)
{
    return "--fpga=FPGA (virt FPGA name exposed to the unikernel)";
}

struct ukvm_module ukvm_module_fpga = {
    .name = "fpga",
    .setup = setup,
    .handle_cmdarg = handle_cmdarg,
    .usage = usage
};
