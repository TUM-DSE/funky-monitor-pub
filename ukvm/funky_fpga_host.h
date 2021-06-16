/*
 * funky_fpga_host.h: Virtual FPGA API definitions.
 *
 */

#ifndef FUNKY_FPGA_HOST_H
#define FUNKY_FPGA_HOST_H

#ifdef __cplusplus
extern "C" {
#endif

#include<stddef.h>

int register_cmd_queues(void* wr_queue_addr, void* rd_queue_addr);
int hello_fpga(char* bin_name);
int reconfigure_fpga(void* bin, size_t bin_size);

#ifdef __cplusplus
}
#endif

#endif /* FUNKY_FPGA_HOST_H */

