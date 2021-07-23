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
void create_fpga_thread(struct fpga_thr_info* thr_info_ptr);
void destroy_fpga_thread(void);

/* just for test */
int hello_fpga(char* bin_name);
int register_cmd_queues(void* wr_queue_addr, void* rd_queue_addr);
int test_program_fpga(void* bin, size_t bin_size);

/* FPGA resource allocation */
int allocate_fpga(void* wr_queue_addr, void* rd_queue_addr);
int reconfigure_fpga(void* bin, size_t bin_size);
int release_fpga(void);

/* Request handler */
int handle_fpga_requests(struct ukvm_hv *hv);

/* FPGA migration */
// int save_bitstream(uint64_t addr, size_t size);
// int save_hw_context();
// int resume_hw_context();

#ifdef __cplusplus
}
#endif

#endif /* FUNKY_BACKEND_API_H */

