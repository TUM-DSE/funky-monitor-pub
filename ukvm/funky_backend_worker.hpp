#ifndef __FUNKY_BACKEND_WORKER__
#define __FUNKY_BACKEND_WORKER__

#include "funky_backend_core.h"
#include "funky_backend_context.hpp"

#include "backend/buffer.hpp"
#include "backend/funky_msg.hpp"

#include <iostream>
#include <thread>
#include <functional>
#include <cstring>
#include <cstdlib>
#include <algorithm>

namespace funky_backend {

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
  int handle_fpga_requests(struct ukvm_hv *hv, funky_backend::XoclContext* ctx)
  {
    int retired_reqs=0;
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

  class Worker
  {
    public:
      // TODO: add mpd_t for msg queue
      Worker(struct fpga_thr_info& thr_info) 
        // : m_thr_info(thr_info), m_fpga_context(thr_info.wr_queue, thr_info.rd_queue), m_worker(&funky_backend::Worker::fpga_worker_thread, nullptr)
        : m_thr_info(thr_info), m_fpga_context(thr_info.wr_queue, thr_info.rd_queue)
      {}

      ~Worker()
      {
        // if(m_worker.joinable()) 
        //   m_worker.join();  
      }

      void run(void)
      {
        /* TODO
         * First, create backend_context
         * 1. if this thread is created by hypercall from guest, 
         *    create the context before polling. 
         *
         * 2. if this thread is created by 'loadvm' request from ukvm monitor thread, 
         *    create the context and restore saved FPGA data.  
         * */
        // TODO: move this to constructor of XoclContext
        m_fpga_context.save_bitstream((uint64_t)m_thr_info.bs, m_thr_info.bs_len);

        /* Reconfigure FPGA */
        auto ret = m_fpga_context.reconfigure_fpga(m_thr_info.bs, m_thr_info.bs_len);
        if(ret != CL_SUCCESS)
        {
          std::cout << "UKVM: failed to program device. \n";
          exit(EXIT_FAILURE);
        }

        /* TODO
         * Second, do polling cmd queues until any of the following events occurs: 
         *
         * 1. The guest does hypercall_fpgafree(): 
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
          handle_fpga_requests(m_thr_info.hv, &m_fpga_context);
      }

    private:
      struct fpga_thr_info m_thr_info;
      funky_backend::XoclContext m_fpga_context;
      // std::thread m_worker;
      std::vector<unsigned char> m_save_data; 

  }; // Worker

} // funky_backend_worker

#endif  // __FUNKY_BACKEND_WORKER__
