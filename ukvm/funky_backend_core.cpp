/**
* Copyright (C) 2020 Xilinx, Inc
*
* Licensed under the Apache License, Version 2.0 (the "License"). You may
* not use this file except in compliance with the License. A copy of the
* License is located at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
* WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
* License for the specific language governing permissions and limitations
* under the License.
*/


#include "funky_backend_core.h"

#include "backend/buffer.hpp"
#include "backend/funky_msg.hpp"

#include "funky_xcl2.hpp"
#include "funky_backend_context.hpp"

#include "pthread.h"

#include <cstdint>
#include <csignal>
#include <memory>
#include <algorithm>
#include <vector>
#include <thread>
#define DATA_SIZE 4096

/* Xocl backend context class to save temp data & communicate with xocl lib */
// TODO: if being used by a worker thread, the thread instantiates this context
// TODO: This must be explicitly released if the guest release FPGA (Otherwise, SIGSEGV may occur)
std::unique_ptr<funky_backend::XoclContext> bk_context;

/* save worker thread id */
pthread_t g_worker_thr;
// std::thread::id g_thr_id;
 
int allocate_fpga(void* wr_queue_addr, void* rd_queue_addr) {
  if(bk_context != nullptr) {
    std::cout << "Warning: xocl context already exists." << std::endl;
    return -1;
  }

  bk_context = std::make_unique<funky_backend::XoclContext>(wr_queue_addr, rd_queue_addr);

  return 0;
}

/** 
 * explicitly release the backend context.
 */
int release_fpga()
{
  bk_context.release();
  return 0;
}

int save_bitstream(uint64_t addr, size_t size)
{
  bk_context->save_bitstream(addr, size);
  return 0;
}

int reconfigure_fpga(void* bin, size_t bin_size)
{
  return bk_context->reconfigure_fpga(bin, bin_size);
}

/**
 * Initialize and allocate a memory space on FPGA to the guest 
 */
int handle_memory_request(struct ukvm_hv *hv, funky_backend::XoclContext* context, funky_msg::request& req)
{
  std::cout << "UKVM: received a MEMORY request." << std::endl;

  /* read meminfo from the guest memory */
  int mem_num=0;
  auto ptr  = req.get_meminfo_array(mem_num);
  auto mems = (funky_msg::mem_info**) UKVM_CHECKED_GPA_P(hv, (ukvm_gpa_t) ptr, sizeof(funky_msg::mem_info*) * mem_num);

  for (auto i=context->get_created_buffer_num(); i<mem_num; i++)
  {
    auto m = (funky_msg::mem_info*) UKVM_CHECKED_GPA_P(hv, (ukvm_gpa_t) mems[i], sizeof(funky_msg::mem_info));
    // std::cout << "mems[" << i << "], id: " << m->id << ", addr: " << m->src << ", size: " << m->size << std::endl;

    void* src = UKVM_CHECKED_GPA_P(hv, (ukvm_gpa_t) m->src, m->size);
    context->create_buffer(m->id, m->flags, m->size, src);
  }

  return 0;
}

/*
 * Transfer data between guest memory and FPGA
 **/
int handle_transfer_request(struct ukvm_hv *hv, funky_backend::XoclContext* context, funky_msg::request& req)
{
  std::cout << "UKVM: received a TRANSFER request." << std::endl;

  /* read transfer info from the guest memory */
  auto ptr     = req.get_transferinfo();
  auto trans   = (funky_msg::transfer_info*) UKVM_CHECKED_GPA_P(hv, (ukvm_gpa_t) ptr, sizeof(funky_msg::transfer_info*));
  auto mem_ids = (int*) UKVM_CHECKED_GPA_P(hv, (ukvm_gpa_t) trans->ids, sizeof(int) * trans->num);

  context->enqueue_transfer(mem_ids, trans->num, trans->flags);

  return 0;
}

/**
 * Execute a kernel on FPGA
 */
int handle_exec_request(struct ukvm_hv *hv, funky_backend::XoclContext* context, funky_msg::request& req)
{
  std::cout << "UKVM: received an EXECUTE request." << std::endl;
  
  /* read arginfo from the guest memory */
  int arg_num=0;
  auto ptr  = req.get_arginfo_array(arg_num);
  auto args = (funky_msg::arg_info*) UKVM_CHECKED_GPA_P(hv, (ukvm_gpa_t) ptr, sizeof(funky_msg::arg_info) * arg_num);

  /* create kernel */
  size_t name_size=0;
  auto name_ptr = req.get_kernel_name(name_size);
  auto kernel_name = (const char*) UKVM_CHECKED_GPA_P(hv, (ukvm_gpa_t) name_ptr, name_size);

  context->create_kernel(kernel_name);

  for (auto i=0; i<arg_num; i++)
  {
    auto arg = &(args[i]);

    /* if the argument is variable (mem_id == -1), do the address translation */
    void* src=nullptr;
    if(arg->mem_id < 0)
      src = UKVM_CHECKED_GPA_P(hv, (ukvm_gpa_t) arg->src, arg->size);

    context->set_arg(kernel_name, arg, src);
  }

  /* execute the kernel */
  context->enqueue_kernel(kernel_name);

  return 0;
}

/**
 * Wait for a completion of all ongoing tasks running on FPGA
 */
int handle_sync_request(struct ukvm_hv *hv, funky_backend::XoclContext* context, funky_msg::request& req)
{
  std::cout << "UKVM: received a SYNC request." << std::endl;

  context->sync_fpga();
  context->send_response(funky_msg::SYNC);

  return 0;
}

/**
 * read all the requests in the queue and call corresponding request handlers. 
 *
 * @return the total number of retired requests. 
 */
int handle_fpga_requests(struct ukvm_hv *hv, void* context)
{
  int retired_reqs=0;

  auto ctx = (context!=NULL)? (funky_backend::XoclContext*) context: bk_context.get();

  using namespace funky_msg;

  auto req = ctx->read_request();
  while(req != NULL) 
  {
    auto req_type = req->get_request_type();
    switch (req_type)
    {
      case MEMORY: 
        handle_memory_request(hv, ctx, *req);
        break;
      case TRANSFER: 
        handle_transfer_request(hv, ctx, *req);
        break;
      case EXECUTE: 
        handle_exec_request(hv, ctx, *req);
        break;
      case SYNC: 
        handle_sync_request(hv, ctx, *req);
        break;
      default:
        std::cout << "UKVM: Warning: an unknown request." << std::endl;
        break;
    }

    /* get the next request */
    retired_reqs++;
    req = ctx->read_request();
  }

  return retired_reqs;
}

int handle_fpga_requests(struct ukvm_hv *hv)
{
  return handle_fpga_requests(hv, NULL);
}


void* fpga_worker_thread(void* arg)
// void fpga_worker_thread(std::unique_ptr<struct fpga_thr_info> thr_info)
{
  // get arguments: fpga_thr_info
  std::unique_ptr<struct fpga_thr_info> thr_info = 
    std::make_unique<struct fpga_thr_info>( *(struct fpga_thr_info*)arg );
  std::free(arg);

  /* TODO
   * First, create backend_context
   * 1. if this thread is created by hypercall from guest, 
   *    create the context before polling. 
   *
   * 2. if this thread is created by 'loadvm' request from ukvm monitor thread, 
   *    create the context and restore saved FPGA data.  
   * */
  funky_backend::XoclContext fpga_context(thr_info->wr_queue, thr_info->rd_queue);
  auto ret = fpga_context.reconfigure_fpga(thr_info->bs, thr_info->bs_len);
  if(ret != CL_SUCCESS)
  {
    std::cout << "UKVM: failed to program device. \n";
    exit(EXIT_FAILURE);
  }

  /* TODO
   * Second, do polling cmd queues until any of the following events occurs: 
   *
   * 1. The guest called hypercall_fpgafree(): 
   *    The hypercall do pthread_kill()? To do this, we need to save the thread ID of the worker thread somewhere. 
   *    Or the guest does not perform the hypercall but send a kind of 'free' request via cmd queue?
   *    This case the worker thread can exit by itself. Which is better?
   *
   * 2. The monitor thread sends a 'savevm' request. 
   *    When the worker thread receives the request, it reads data from FPGA memory and save them into a contiguous region, and exit (an actual FPGA is released then). 
   *    The data size and a pointer to the head is sent to the monitor thread and the monitor write data into a file. 
   * 
   * */
  while(true)
    handle_fpga_requests(thr_info->hv, (void*)&fpga_context);

}

void create_fpga_thread(struct fpga_thr_info* thr_info)
// void create_fpga_thread(void* thr_info_ptr)
{
  std::cout << "UKVM: create a worker thread..." << std::endl;

  auto arg = (struct fpga_thr_info*) std::malloc(sizeof(fpga_thr_info));
  std::memcpy(arg, thr_info, sizeof(fpga_thr_info));

  pthread_create(&g_worker_thr, NULL, fpga_worker_thread, (void *)arg);

  // std::thread worker_thr(fpga_worker_thread, std::move(thr_info));
  // worker_thr.get_id()

  return;
}

void destroy_fpga_thread()
{
  std::cout << "UKVM: destroy a worker thread... (TBD)" << std::endl;

  // TODO: kill the worker thread. use signals?
  // pthread_kill(g_worker_thr, SIGINT);
  return;
}

