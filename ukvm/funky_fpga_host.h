/*
 * funky_fpga_host.h: Virtual FPGA API definitions.
 *
 */

#ifndef FUNKY_FPGA_HOST_H
#define FUNKY_FPGA_HOST_H

#ifdef __cplusplus
extern "C" {
#endif

int register_cmd_queues(void* wr_queue_addr, void* rd_queue_addr);
int hello_fpga(char* binfile);

#ifdef __cplusplus
}
#endif

#endif /* FUNKY_FPGA_HOST_H */

