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
#include "funky_debug_c.h"

/* multi-threading */
struct fpga_thr_info
{
  struct ukvm_hv *hv;
  uint64_t bs;
  size_t bs_len;
  uint64_t wr_queue;
  size_t wr_queue_len;
  uint64_t rd_queue;
  size_t rd_queue_len;

  void* mig_data;
  size_t mig_size;
};

void create_fpga_worker(struct fpga_thr_info thr_info);
void destroy_fpga_worker(void);
int is_fpga_worker_alive(void);


/* inter-thread communication via msg queue */
enum ThrMsgType {
  // msg from vCPU to Worker
  MSG_KILLWORKER,
  // msg from Monitor to Worker
  MSG_SAVEFPGA, MSG_LOADFPGA, 
  // msg from Worker to Monitor/vCPU
  MSG_INIT, MSG_SYNCED, MSG_UPDATED, MSG_SAVED, MSG_END
};

struct thr_msg {
  enum ThrMsgType msg_type;
  void* data;
  size_t size;
};

struct fpga_data_header {
  unsigned int sb_fpgainit: 1; // status bit
  unsigned long data_size: 63;
};

#define MSG_QUEUE_MAX_CAPACITY 8

// struct thr_msg* recv_msg_from_worker(void);
int recv_msg_from_worker(struct thr_msg* msg);
int send_msg_to_worker(struct thr_msg *msg);


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

