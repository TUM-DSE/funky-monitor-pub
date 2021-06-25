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

int hello_fpga(char* bin_name);
int register_cmd_queues(void* wr_queue_addr, void* rd_queue_addr);
int reconfigure_fpga(void* bin, size_t bin_size);

#ifdef __cplusplus
}
#endif

#endif /* FUNKY_BACKEND_API_H */

