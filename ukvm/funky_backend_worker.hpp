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
  int handle_fpga_requests(struct ukvm_hv *hv, funky_backend::XoclContext* ctx, bool& fpga_sync_flag)
  {
    int retired_reqs=0;
    using namespace funky_msg;

    auto req = ctx->read_request();
    while(req != NULL) 
    {
      auto req_type = req->get_request_type();
      (req_type == SYNC)? fpga_sync_flag = true: false;

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

  // TODO: Rename this to "Handler"?
  // TODO: Distinguish the Handler for "guest", and Handler for "monitor"
  class Worker
  {
    public:
      Worker(struct fpga_thr_info& thr_info, void* rq_addr, void* wq_addr) 
        : m_thr_info(thr_info), 
          m_fpga_context(UKVM_CHECKED_GPA_P(thr_info.hv, thr_info.wr_queue, thr_info.wr_queue_len), 
              UKVM_CHECKED_GPA_P(thr_info.hv, thr_info.rd_queue, thr_info.rd_queue_len), thr_info.mig_data, thr_info.mig_size), 
          m_save_data(0), msg_read_queue(wq_addr), msg_write_queue(rq_addr), fpga_sync_flag(true)
      {
        // TODO: check if this is safe
        if(thr_info.mig_data != NULL)
          free(thr_info.mig_data);
      }

      ~Worker()
      {}

      /* Reconfigure FPGA */
      void reconfigure_fpga()
      {
        void* bitstream = UKVM_CHECKED_GPA_P(m_thr_info.hv, m_thr_info.bs, m_thr_info.bs_len);

        auto ret = m_fpga_context.reconfigure_fpga(bitstream, m_thr_info.bs_len);
        if(ret != CL_SUCCESS)
        {
          std::cout << "UKVM: failed to program device. \n";
          exit(EXIT_FAILURE);
        }
      }

      int handle_fpga_requests()
      {
        auto num = funky_backend::handle_fpga_requests(m_thr_info.hv, &m_fpga_context, fpga_sync_flag);

        return num;
      }

      bool is_fpga_updated()
      {
        // TODO: develop here
        return false;
      }

      std::vector<uint8_t>& save_fpga()
      {
        // TODO: 
        // 1. pass m_save_data to XoclContext
        // 2. XoclContext calculates total data size and resize m_save_data
        // 3. copy data

        /* calculate total data size */
        size_t bytes = 128;

        /* copy data */
        m_save_data.resize(bytes, 0xab);

        return m_save_data;
      }

      bool handle_migration_requests(void)
      {
        auto req = msg_read_queue.pop();

        bool migration_flag = false;
        if(req->msg_type == MSG_SAVEFPGA)
        {
          std::cout << "!!! Receive SAVEFPGA req from monitor !!! \n";
          migration_flag = true;
        }

        return migration_flag;
      }

      /** 
       * receive a request by polling the cmd queue 
       */
      struct thr_msg* recv_msg()
      {
        return msg_read_queue.pop();
      }

      /**  
       * sending a response to the guest via cmd queue
       */
      bool send_msg(struct thr_msg& msg)
      {
        return msg_write_queue.push(msg);
      }

      bool is_fpga_synced()
      {
        return fpga_sync_flag;
      }

      void* get_thr_info()
      {
        return (void *)&m_thr_info;
      }

    private:
      struct fpga_thr_info m_thr_info;
      funky_backend::XoclContext m_fpga_context;
      // std::thread m_worker;
      std::vector<uint8_t> m_save_data; 
      buffer::Reader<struct thr_msg> msg_read_queue;
      buffer::Writer<struct thr_msg> msg_write_queue;

      bool fpga_sync_flag;

  }; // Worker

} // funky_backend_worker

#endif  // __FUNKY_BACKEND_WORKER__
