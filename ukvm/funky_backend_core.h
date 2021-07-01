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

/* just for test */
int hello_fpga(char* bin_name);
int register_cmd_queues(void* wr_queue_addr, void* rd_queue_addr);
int test_program_fpga(void* bin, size_t bin_size);

/* FPGA resource allocation */
int allocate_fpga(void* wr_queue_addr, void* rd_queue_addr);
int reconfigure_fpga(void* bin, size_t bin_size);
int release_fpga(void);

/* Request handler */
int handle_requests(void);
// int create_buffer();
// int enqueue_task();
// int enqueue_transfer();
// int sync_fpga();

/* FPGA migration */
// int save_hw_context();
// int resume_hw_context();

#ifdef __cplusplus
}
#endif

#endif /* FUNKY_BACKEND_API_H */

