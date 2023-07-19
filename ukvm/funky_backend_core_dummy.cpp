#include "funky_backend_core.h"

int allocate_fpga(void* wr_queue_addr, void* rd_queue_addr) 
{
  return 0;
}

int release_fpga()
{
  return 0;
}


int save_bitstream(uint64_t addr, size_t size)
{
  return 0;
}


int reconfigure_fpga(void* bin, size_t bin_size, int *reconfigured)
{
  return 0;
}


int handle_fpga_requests(struct ukvm_hv *hv)
{
  return 0;
}

void create_fpga_worker(struct fpga_thr_info thr_info)
{
  return;
}


void destroy_fpga_worker()
{
  return;
}

int is_fpga_worker_alive()
{
  return 0;
}

// TODO: mutex is necessary?
int recv_msg_from_worker(struct thr_msg* msg)
{
  return 1;
}

int send_msg_to_worker(struct thr_msg *msg)
{
  return 0;
}
