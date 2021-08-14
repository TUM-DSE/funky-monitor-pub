/*
 * funky_backend_core.h: Funky backend core functions.
 *
 */

#ifndef FUNKY_BACKEND_CORE_H
#define FUNKY_BACKEND_CORE_H

#ifdef __cplusplus
extern "C" {
#endif

#include<stddef.h>
#include<stdint.h>

#include "ukvm.h"

struct fpga_thr_info
{
  struct ukvm_hv *hv;
  void *bs;
  size_t bs_len;
  void *wr_queue;
  void *rd_queue;
  // TODO: 
  // void *saved_data;
  // size_t saved_data_size;
};

/* multi-threading */
void create_fpga_worker(struct fpga_thr_info thr_info);
void destroy_fpga_worker(void);

/* FPGA resource allocation */
int allocate_fpga(void* wr_queue_addr, void* rd_queue_addr);
int reconfigure_fpga(void* bin, size_t bin_size);
int release_fpga(void);

/* Request handler */
int handle_fpga_requests(struct ukvm_hv *hv);

#ifdef __cplusplus
}
#endif

#endif /* FUNKY_BACKEND_API_H */

