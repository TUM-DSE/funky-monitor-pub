/*
 * Copyright (c) 2015-2017 Contributors as noted in the AUTHORS file
 *
 * This file is part of Solo5, a unikernel base layer.
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

#include "kernel.h"

solo5_result_t solo5_fpga_info(void)
{
    struct ukvm_fpgainfo vfpga;

    ukvm_do_hypercall(UKVM_HYPERCALL_FPGAINFO, &vfpga);

    return (vfpga.ret == 0) ? SOLO5_R_OK : SOLO5_R_EUNSPEC;
}

solo5_result_t solo5_fpga_init(struct solo5_fpgainit* init_info)
{
  struct ukvm_fpgainit vfpga;

  vfpga.bs        = init_info->bs;
  vfpga.wr_queue  = init_info->wr_queue;
  vfpga.rd_queue  = init_info->rd_queue;

  vfpga.bs_len       = init_info->bs_len;
  vfpga.wr_queue_len = init_info->wr_queue_len;
  vfpga.rd_queue_len = init_info->rd_queue_len;

  ukvm_do_hypercall(UKVM_HYPERCALL_FPGAINIT, &vfpga);

  return (vfpga.ret == 0) ? SOLO5_R_OK : SOLO5_R_EUNSPEC;
}

int solo5_fpga_handle_request(void)
{
  int retired_reqs=0;
  ukvm_do_hypercall(UKVM_HYPERCALL_FPGAREQ, &retired_reqs);

  return retired_reqs;
}


int solo5_fpga_free(void)
{
  ukvm_do_hypercall(UKVM_HYPERCALL_FPGAFREE, NULL);

  return 0;
}
