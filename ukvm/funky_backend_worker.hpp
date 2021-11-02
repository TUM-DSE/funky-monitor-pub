#ifndef __FUNKY_BACKEND_WORKER__
#define __FUNKY_BACKEND_WORKER__

#include "funky_backend_core.h"
#include "funky_backend_context.hpp"

#include "backend/buffer.hpp"
#include "backend/funky_msg.hpp"
#include "funky_debug.h"

#include <iostream>
#include <thread>
#include <functional>
#include <cstring>
#include <cstdlib>
#include <algorithm>

// #include "timer.h" // for evaluation

// TIMER_INIT(3)

namespace funky_backend {

  /**
   * Initialize and allocate a memory space on FPGA to the guest 
   */
  int handle_memory_request(struct ukvm_hv *hv, funky_backend::XoclContext* context, funky_msg::request& req)
  {
    DEBUG_STREAM("received a MEMORY request.");

    /* read meminfo from the guest memory */
    int mem_num=0;
    auto ptr  = req.get_meminfo_array(mem_num);
    auto mems = (funky_msg::mem_info**) UKVM_CHECKED_GPA_P(hv, (ukvm_gpa_t) ptr, sizeof(funky_msg::mem_info*) * mem_num);

    for (auto i=context->get_created_buffer_num(); i<mem_num; i++)
    {
      auto m = (funky_msg::mem_info*) UKVM_CHECKED_GPA_P(hv, (ukvm_gpa_t) mems[i], sizeof(funky_msg::mem_info));
      DEBUG_STREAM("mems[" << i << "], id: " << m->id << ", addr: " << std::hex << m->src << ", size: " << m->size << ", flags: " << m->flags);

      void* host_ptr = (m->src == nullptr)? nullptr: UKVM_CHECKED_GPA_P(hv, (ukvm_gpa_t) m->src, m->size);
      context->create_buffer(m->id, m->flags, m->size, host_ptr, m->src);
    }

    return 0;
  }

  /*
   * Transfer data between guest memory and FPGA
   **/
  int handle_transfer_request(struct ukvm_hv *hv, funky_backend::XoclContext* context, funky_msg::request& req)
  {
    DEBUG_STREAM("received a TRANSFER request.");

    /* read transfer info from the guest memory */
    auto ptr     = req.get_transferinfo();
    auto trans   = (funky_msg::transfer_info*) UKVM_CHECKED_GPA_P(hv, (ukvm_gpa_t) ptr, sizeof(funky_msg::transfer_info*));
    auto mem_ids = (int*) UKVM_CHECKED_GPA_P(hv, (ukvm_gpa_t) trans->ids, sizeof(int) * trans->num);

    /* read event info */
    unsigned int num_events = 0;
    int* event_list_ids = nullptr;
    int event_id = -1;
    auto einfo_ptr = req.get_eventinfo();
    if(einfo_ptr != nullptr)
    {
      auto einfo = (funky_msg::event_info*) UKVM_CHECKED_GPA_P(hv, (ukvm_gpa_t) einfo_ptr, sizeof(funky_msg::event_info*));
      num_events = einfo->wait_event_num;
      event_list_ids = (num_events > 0)? (int*) UKVM_CHECKED_GPA_P(hv, (ukvm_gpa_t) einfo->wait_event_ids, sizeof(int) * num_events): nullptr;
      event_id = einfo->id; 
    }

    /* MIGRATE or READ/WRITE */
    if(trans->trans == funky_msg::MIGRATE) {
      DEBUG_STREAM("transfer type: MIGRATE ");
      context->enqueue_transfer(req.get_cmdq_id(), mem_ids, trans->num, trans->flags, num_events, event_list_ids, event_id);
    }
    else {
      DEBUG_STREAM("transfer type: READ/WRITE ");
      bool is_write = (trans->trans == funky_msg::WRITE)? true: false;
      auto host_ptr = (void*) UKVM_CHECKED_GPA_P(hv, (ukvm_gpa_t) trans->ptr, sizeof(uint8_t) * trans->size);
      context->enqueue_transfer(req.get_cmdq_id(), mem_ids, trans->num, trans->flags, trans->offset, trans->size, host_ptr, is_write, num_events, event_list_ids, event_id);
    }

    return 0;
  }

  /**
   * Execute a kernel on FPGA
   */
  int handle_exec_request(struct ukvm_hv *hv, funky_backend::XoclContext* context, funky_msg::request& req)
  {
    // std::cout << "UKVM: received an EXECUTE request." << std::endl;
    DEBUG_STREAM("received EXEC request. ");

    /* read arginfo from the guest memory */
    int arg_num=0;
    auto ptr  = req.get_arginfo_array(arg_num);
    auto args = (funky_msg::arg_info*) UKVM_CHECKED_GPA_P(hv, (ukvm_gpa_t) ptr, sizeof(funky_msg::arg_info) * arg_num);

    /* create kernel */
    size_t name_size=0;
    auto name_ptr = req.get_kernel_name(name_size);
    auto kernel_name = (const char*) UKVM_CHECKED_GPA_P(hv, (ukvm_gpa_t) name_ptr, name_size);
    context->create_kernel(kernel_name);

    /* set arguments of kernel */
    for (auto i=0; i<arg_num; i++)
    {
      auto arg = &(args[i]);

      /* if the argument is variable (mem_id == -1), do the address translation */
      void* src=nullptr;
      if(arg->mem_id < 0)
        src = UKVM_CHECKED_GPA_P(hv, (ukvm_gpa_t) arg->src, arg->size);

      context->set_arg(kernel_name, arg, src);
    }

    /* read event info */
    unsigned int num_events = 0;
    int* event_list_ids = nullptr;
    int event_id = -1;
    auto einfo_ptr = req.get_eventinfo();
    if(einfo_ptr != nullptr)
    {
      auto einfo = (funky_msg::event_info*) UKVM_CHECKED_GPA_P(hv, (ukvm_gpa_t) einfo_ptr, sizeof(funky_msg::event_info*));
      num_events = einfo->wait_event_num;
      event_list_ids = (num_events > 0)? (int*) UKVM_CHECKED_GPA_P(hv, (ukvm_gpa_t) einfo->wait_event_ids, sizeof(int) * num_events): nullptr;
      event_id = einfo->id; 
    }

    /* execute the kernel */
    context->enqueue_kernel(req.get_cmdq_id(), kernel_name, num_events, event_list_ids, event_id);

    DEBUG_STREAM("EXEC request is finished. ");
    return 0;
  }

  /**
   * Wait for a completion of all ongoing tasks running on FPGA
   */
  int handle_sync_request(struct ukvm_hv *hv, funky_backend::XoclContext* context, funky_msg::request& req)
  {
    // TIMER_START(2);
    DEBUG_STREAM("received a SYNC request.");
    /* clFinish() */
    switch(req.get_sync_type())
    {
      case funky_msg::FINISH:
      {
        DEBUG_STREAM("calling clFinish()...");
        context->sync_fpga(req.get_cmdq_id());
        break;
      }
      case funky_msg::PROFILE:
      {
        DEBUG_STREAM("calling clGetEventProfilingInfo()...");
        auto ptr     = req.get_eventinfo();
        if(ptr == nullptr) {
          DEBUG_STREAM("Error: ptr to event info is null.");
          exit(1);
        }

        auto einfo   = (funky_msg::event_info*) UKVM_CHECKED_GPA_P(hv, (ukvm_gpa_t) ptr, sizeof(funky_msg::event_info*));
        auto eparam  = (uint64_t *) UKVM_CHECKED_GPA_P(hv, (ukvm_gpa_t) einfo->param, sizeof(uint64_t));
        DEBUG_STREAM("event id: " << einfo->id << ", param name: " << std::hex << einfo->param_name << ", param ptr: " << einfo->param << ", eparam: " << eparam);
        context->get_profiling_info(einfo->id, (cl_profiling_info) einfo->param_name, (void*) eparam);
        DEBUG_STREAM("profiling is finished.");

        break;
      }
      case funky_msg::WAITEVENTS:
      {
        DEBUG_STREAM("calling clWaitForEvents()...");
        auto ptr     = req.get_eventinfo();
        auto einfo   = (funky_msg::event_info*) UKVM_CHECKED_GPA_P(hv, (ukvm_gpa_t) ptr, sizeof(funky_msg::event_info*));
        auto elist   = (int*) UKVM_CHECKED_GPA_P(hv, (ukvm_gpa_t) einfo->wait_event_ids, sizeof(int) * einfo->wait_event_num);
        
        context->wait_for_events(einfo->wait_event_num, elist);
        break;
      }
    }

    context->send_response(funky_msg::SYNC);
    // TIMER_STOP_ID(2);

    // printf(" %s       : %12.4f ms\n", __func__, TIMER_REPORT_MS(2));
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

  // TODO: Rename this to "Handler"?
  // TODO: Distinguish the Handler for "guest", and Handler for "monitor"
  class Worker
  {
    public:
      Worker(struct fpga_thr_info& thr_info, void* rq_addr, void* wq_addr) 
        : m_thr_info(thr_info), 
          m_fpga_context(
              UKVM_CHECKED_GPA_P(thr_info.hv, thr_info.wr_queue, thr_info.wr_queue_len), 
              UKVM_CHECKED_GPA_P(thr_info.hv, thr_info.rd_queue, thr_info.rd_queue_len)
              ), 
          m_save_data(0), msg_read_queue(wq_addr), msg_write_queue(rq_addr)
      {}

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
        auto num = funky_backend::handle_fpga_requests(m_thr_info.hv, &m_fpga_context);
        return num;
      }

      bool is_fpga_updated()
      {
        return m_fpga_context.get_updated_flag();
      }

      /* return true if FPGA is at a sync point */
      bool check_sync_point()
      {
        return m_fpga_context.get_sync_flag();
      }

      std::vector<uint8_t>& save_fpga()
      {
        return m_fpga_context.save_fpga_memory();
      }

      bool load_fpga()
      {
        if(m_thr_info.mig_data==NULL)
          return false;

        auto ret = m_fpga_context.load_fpga_memory(
            m_thr_info.hv, 
            m_thr_info.mig_data, 
            m_thr_info.mig_size);

        std::free(m_thr_info.mig_data);
        return ret;
      }

      void sync_fpga_memory()
      {
        m_fpga_context.sync_shared_buffers();
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

      void* get_thr_info()
      {
        return (void *)&m_thr_info;
      }

    private:
      struct fpga_thr_info m_thr_info;
      funky_backend::XoclContext m_fpga_context;
      std::vector<uint8_t> m_save_data; 
      buffer::Reader<struct thr_msg> msg_read_queue;
      buffer::Writer<struct thr_msg> msg_write_queue;
      bool fpga_sync_flag;

  }; // Worker

} // funky_backend_worker

#endif  // __FUNKY_BACKEND_WORKER__
