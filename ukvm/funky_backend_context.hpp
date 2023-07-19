#ifndef __FUNKY_BACKEND_CONTEXT__
#define __FUNKY_BACKEND_CONTEXT__

#include <CL/cl2.hpp>
#include <CL/cl_ext_xilinx.h>
#include "funky_xcl2.hpp"

// TODO: change file name to funky_msg.hpp
#include "backend/buffer.hpp"
#include "backend/funky_msg.hpp"
#include "funky_debug.h"

#include "ukvm.h" // UKVM_CHECKED_GPA_P()

#include <memory> // make_unique
#include <algorithm>
#include <vector>
#include <map>
#include <chrono>

// TODO: user funky_hw_context to save/restore data on FPGA
// #include "funky_hw_context.hpp"

namespace funky_backend {
  class XoclContext {
    public:
      /* used for migration */
      struct memobj_header
      {
        int mem_id;
        ukvm_gpa_t gpa;
        size_t mem_size;
        cl_mem_object_type mem_type;
        cl_mem_flags mem_flags;
        bool onfpga_flag; //if true, data needs to be copied from src FPGA mem to dest FPGA mem
      };

      struct eventobj_header
      {
        int event_id;
        cl_ulong profiling_info[5];
      };

    private:
      buffer::Reader<funky_msg::request>  request_q;
      buffer::Writer<funky_msg::response> response_q;

      uint64_t bin_guest_addr;
      size_t bin_size;
      cl::Device device;
      cl::Context context;
      std::unique_ptr<cl::Program> program;
      std::map<int, cl::CommandQueue> queues;

      // TODO: kernels that have been initialized once will be reused in the future? If not, we don't need to keep them here
      std::map<const char*, cl::Kernel> kernels;

      // memory obj
      // TODO: consider cl::Pipe, cl::Image
      std::map<int, cl::Buffer> buffers;
      std::map<int, bool> buffer_onfpga_flags;
      std::map<int, ukvm_gpa_t> buffer_gpas;

      // event obj
      std::map<int, cl::Event> events;
      std::map<int, struct eventobj_header> migrated_events;

      // for migration
      std::vector<uint8_t> mig_save_data;
      bool sync_flag;
      bool updated_flag;

    public:
      XoclContext(void* wr_queue_addr, void* rd_queue_addr) 
        : request_q(wr_queue_addr), 
          response_q(rd_queue_addr), 
          bin_guest_addr(0), bin_size(0), 
          mig_save_data(0), sync_flag(true), updated_flag(false)
      {
        cl_int err;

        // TODO: assign as many devices to the guest as requested  
        //       Currently, only one device (devices[0]) is assigned to the guest.
        auto devices = xcl::get_xil_devices();
        if(devices.size() == 0) {
          std::cout << "Error: no device is found.\n";
          exit(EXIT_FAILURE);
        }

        // auto device = devices[0];
        device = devices[0];

        // TODO: command queue option depends on the guest, so it shouldn't be created in advance?
        // Creating Context and Command Queue for selected Device
        OCL_CHECK(err, context = cl::Context(device, NULL, NULL, NULL, &err));
        // OCL_CHECK(err, queue = cl::CommandQueue(context, device, CL_QUEUE_PROFILING_ENABLE, &err));
        OCL_CHECK(err,  queues.emplace(0, cl::CommandQueue(context, device, CL_QUEUE_PROFILING_ENABLE, &err)));
        // OCL_CHECK(err,  queues.emplace(0, cl::CommandQueue(context, device, CL_QUEUE_PROFILING_ENABLE | CL_QUEUE_OUT_OF_ORDER_EXEC_MODE_ENABLE, &err)));
      }

      ~XoclContext()
      {}

      /** 
       * receive a request by polling the cmd queue 
       */
      funky_msg::request* pop_request()
      {
        return request_q.pop();
      }

      funky_msg::request* read_request()
      {
        return request_q.read();
      }

      void update_readq()
      {
        request_q.update();
      }

      /**  
       * sending a response to the guest via cmd queue
       */
      bool send_response(funky_msg::ReqType type)
      {
        DEBUG_STREAM("send a response to the guest. type: " << type);

        funky_msg::response response(type);
        return response_q.push(response);
      }

      /** 
       * reconfigure FPGA with a bitstream 
       */
      int reconfigure_fpga(void* bin, size_t bin_size, double *reconfigured)
      {
        cl_int err = CL_SUCCESS;

        cl::Program::Binaries bins{{bin, bin_size}};
        auto devices = xcl::get_xil_devices();
        std::vector <cl::Device> p_devices = {devices[0]};

        auto start = std::chrono::high_resolution_clock::now();

          /* program bistream to the device (FPGA) */
        program = std::make_unique<cl::Program>(context, p_devices, bins, nullptr, &err);

        auto end = std::chrono::high_resolution_clock::now();

        if (err != CL_SUCCESS) {
          return err;
        }

        // TODO Improve reconfiguration detection
        if (reconfigured != nullptr) {
            auto reconfigure_time = std::chrono::duration<double, std::milli>(end - start).count();
            // XRT does not expose whether the FPGA was reconfigured or not, so we use a heuristic
            // to detect if the FPGA was reconfigured by comparing the reconfiguration time to a
            // threshold of 100ms. This is not a robust solution, but it works for now.
            if (reconfigure_time > 100) {
                *reconfigured = reconfigure_time;
            } else {
                std::cout << "FPGA was reused (duration " << reconfigure_time << "ms)" << std::endl;
            }
        }

        return err;
      }

      /**
       * create buffer in global memory 
       */
      void create_buffer(int mem_id, uint64_t mem_flags, size_t size, void* host_ptr, void* gpa)
      {
        cl_int err;

        /* 
         * When clEnqueueMapBuffer() is called by the guest, Funky OpenCL reserves memory region 
         * for the buffer on the guest and nothing is done on FPGA side. 
         * This case, the specified memory object might be initialized without CL_MEM_USE_HOST_PTR. 
         * The backend adds CL_MEM_USE_HOST_PTR here for the particular case. 
         */
        if(host_ptr != nullptr)
          mem_flags = mem_flags | CL_MEM_USE_HOST_PTR;

        OCL_CHECK(err,  buffers.emplace(mem_id, cl::Buffer(context, (cl_mem_flags) mem_flags, size, host_ptr, &err)));
        // std::cout << "Succeeded to create buffer " << mem_id << std::endl;
        
        buffer_onfpga_flags.emplace(mem_id, false);
        buffer_gpas.emplace(mem_id, (ukvm_gpa_t) gpa);
        sync_flag = false;
        updated_flag = true;
      }

      int get_created_buffer_num()
      {
        return buffers.size();
      }

      /* create a new event */
      cl::Event* create_event(unsigned int event_id)
      {
        cl::Event new_event;
        auto result = events.emplace(event_id, new_event);
        if(result.second == false)
          DEBUG_STREAM("INFO: event (id: " << event_id << ") already exists.");
        else
          DEBUG_STREAM("INFO: event (id: " << event_id << ") is newly created.");

        return &events[event_id];
      }

      /* create an event wait list */
      void update_event_list(std::vector<cl::Event>& event_list, unsigned int num_events, int* event_list_ids)
      {
        // std::vector<cl::Event> event_list;
        for(unsigned int i=0; i<num_events; i++)
        {
          DEBUG_STREAM("INFO: searching for the event (id: " << event_list_ids[i] << ")...");

          auto event_in_map = events.find(event_list_ids[i]);
          if(event_in_map == events.end()) {
            std::cout << "UKVM (ERROR): event " << event_list_ids[i] << " is not found. " << std::endl;
            exit(1);
          }

          event_list.push_back((cl::Event) event_in_map->second());
        }

        // return event_list;
        return;
      }

      /* create a new kernel */
      // TODO: return the kernel instance itself?
      // set_arg() and create_kernel() will be also changed to use the instance as an argument
      void create_kernel(const char* kernel_name)
      {
        cl_int err;

        auto search = kernels.find(kernel_name);
        if(search == kernels.end()) {
          /* use kernel name as an index */
          OCL_CHECK(err, kernels.emplace(kernel_name, cl::Kernel(*program, kernel_name, &err)));
          return;
        }

        // if the same kernel already exists, skip the creation and use the existing one. 
        DEBUG_STREAM("The specified kernel " << kernel_name << " is found. Nothing is done here. ");
      }

      /**
       * set OpenCL memory objects as kernel arguments 
       */
      void set_arg(const char* kernel_name, funky_msg::arg_info* arg, void* src=nullptr)
      {
        cl_int err;
        auto id = kernel_name;

        if(arg->mem_id == -1) {
          /* variables other than OpenCL memory objects */
          if(src == nullptr) {
            std::cout << "arg addr error." << std::endl;
            return;
          }

          DEBUG_STREAM("set scalar (addr: " << src << ", size: " << arg->size << ") to arg[" << arg->index << "]" );
          DEBUG_STREAM("scalar value: " << *(unsigned int*)src);
          OCL_CHECK(err, err = kernels[id].setArg(arg->index, arg->size, (const void*)src));
        }
        else {
          DEBUG_STREAM("set memobj[" << arg->mem_id << "] to arg[" << arg->index << "]" );
          /* OpenCL memory objects */
          OCL_CHECK(err, err = kernels[id].setArg(arg->index, buffers[arg->mem_id]));

          /* Memory objects specified as kernel arguments must be on FPGA */
          buffer_onfpga_flags[arg->mem_id] = true;
        }
      }

      /**
       * launch the kernel 
       */
      void enqueue_kernel(int cmdq_id, const char* kernel_name, size_t ndparams[3], unsigned int num_events, int* event_list_ids, int event_id)
      {
        cl_int err;
        auto id = kernel_name;

        /* create a cmd queue if not exists */
        auto queue_in_map = queues.find(cmdq_id);
        if(queue_in_map == queues.end()) {
          OCL_CHECK(err,  queues.emplace(cmdq_id, cl::CommandQueue(context, device, CL_QUEUE_PROFILING_ENABLE, &err)));
          DEBUG_STREAM("UKVM: new cmd queue (id: " << cmdq_id << ") is created. ");
        }

        /* create a new event */
        DEBUG_STREAM("event id: " << event_id);
        auto event_ptr = (event_id  >= 0)? create_event(event_id): nullptr;

        /* wait for events in the waiting list */
        std::vector<cl::Event> wait_list;
        update_event_list(wait_list, num_events, event_list_ids);
        auto list_ptr  = (wait_list.size() > 0)? &wait_list: nullptr;

        /* TODO: support for enqueueNDRangeKernel() */
        // For HLS kernels global and local size is always (1,1,1). So, it is recommended
        // to always use enqueueTask() for invoking HLS kernel
        // OCL_CHECK(err, err = queues[cmdq_id].enqueueTask(kernels[id], list_ptr, event_ptr));
        OCL_CHECK(err, err = queues[cmdq_id].enqueueNDRangeKernel(kernels[id], ndparams[0], ndparams[1], ndparams[2], list_ptr, event_ptr));

        sync_flag = false;
        updated_flag = true;
      }

      /**
       * Launch data transfer between Device Global memory and Host Local Memory 
       *
       * @param (mem_ids) : vector containing IDs of cl_mem objects being transferred from/to Host
       * @param (flags)   : 0 means from host, CL_MIGRATE_MEM_OBJECT_HOST means to host 
       *                     cl_mem_migration_flags, a bitfield based on unsigned int64
       */
      void enqueue_transfer(int cmdq_id, int mem_ids[], size_t id_num, uint64_t flags, unsigned int num_events, int* event_list_ids, int event_id)
      {
        /* create a cmd queue if not exists */
        auto queue_in_map = queues.find(cmdq_id);
        if(queue_in_map == queues.end()) {
          cl_int err;
          OCL_CHECK(err,  queues.emplace(cmdq_id, cl::CommandQueue(context, device, CL_QUEUE_PROFILING_ENABLE, &err)));
          DEBUG_STREAM("new cmd queue (id: " << cmdq_id << ") is created. ");
        }

        /* create a list of memory objects */
        std::vector<cl::Memory> trans_buffers;
        for(size_t i=0; i<id_num; i++)
        {
          auto id = mem_ids[i];
          trans_buffers.emplace_back(buffers[id]);
          DEBUG_STREAM("Buffer (id: " << id << ") is added to the transfer list. ");

          /* 0 means data transfers from Host to FPGA */
          if( flags == 0 )
            buffer_onfpga_flags[id] = true;
        }

        /* create a new event */
        auto event_ptr = (event_id  >= 0)? create_event(event_id): nullptr;

        /* wait for events in the waiting list */
        std::vector<cl::Event> wait_list;
        update_event_list(wait_list, num_events, event_list_ids);
        auto list_ptr  = (wait_list.size() > 0)? &wait_list: nullptr;

        cl_int err;
        OCL_CHECK(err, err = queues[cmdq_id].enqueueMigrateMemObjects(trans_buffers, (cl_mem_migration_flags)flags, list_ptr, event_ptr));

        if(flags == 0)
          DEBUG_STREAM("Writing data to GMEM... ");
        else
          DEBUG_STREAM("Reading data from GMEM... ");

        sync_flag = false;
        updated_flag = true;
      }

      /**
       * Launch data transfer (Read/Write) between Device Global memory and Host Local Memory 
       *
       * @param (mem_ids) : vector containing IDs of cl_mem objects being transferred from/to Host
       * @param (flags)   : indicates blocking or non-blocking read/write.
       * @param (offset)  : offset of the memory object where to be read from or written to.
       * @param (size)    : size of data in host memory. 
       * @param (ptr)     : a pointer to host memory where data is read from or written to.
       *
       */
      void enqueue_transfer(int cmdq_id, int mem_ids[], size_t id_num, uint64_t flags, size_t offset, size_t size, void *ptr, bool is_write, unsigned int num_events, int* event_list_ids, int event_id)
      {
        /* create a cmd queue if not exists */
        auto queue_in_map = queues.find(cmdq_id);
        if(queue_in_map == queues.end()) {
          cl_int err;
          OCL_CHECK(err,  queues.emplace(cmdq_id, cl::CommandQueue(context, device, CL_QUEUE_PROFILING_ENABLE, &err)));
          DEBUG_STREAM("UKVM: new cmd queue (id: " << cmdq_id << ") is created. ");
        }

        /* create a new event */
        auto event_ptr = (event_id  >= 0)? create_event(event_id): nullptr;

        /* wait for events in the waiting list */
        std::vector<cl::Event> wait_list;
        update_event_list(wait_list, num_events, event_list_ids);
        auto list_ptr  = (wait_list.size() > 0)? &wait_list: nullptr;

        for(size_t i=0; i<id_num; i++)
        {
          // TODO: consider when enqueueReadBuffer or enqueueWriteBuffer is called
          cl_int err;
          auto id = mem_ids[i];

          if(is_write) {
            OCL_CHECK(err, err = queues[cmdq_id].enqueueWriteBuffer(buffers[id], (cl_bool)flags, offset, size, ptr, list_ptr, event_ptr));

            /* if write, set a dirty flag */
            buffer_onfpga_flags[id] = true;
          }
          else {
            OCL_CHECK(err, err = queues[cmdq_id].enqueueReadBuffer(buffers[id], (cl_bool)flags, offset, size, ptr, list_ptr, event_ptr));
          }

        }

        sync_flag = false;
        updated_flag = true;
      }

      /**
       * call clGetEventProfilingInfo()
       */
      void get_profiling_info(int event_id, cl_profiling_info param_name, void* param_value)
      {
        cl_int err;

        auto event_in_map = events.find(event_id);
        if(event_in_map != events.end()) {
          DEBUG_STREAM("event[" << event_id << "] is found.");
          OCL_CHECK(err, err = events[event_id].getProfilingInfo<uint64_t>(param_name, (uint64_t*) param_value));

          return;
        }

        /* if event is not found, search migrated events */
        auto eheader_in_map = migrated_events.find(event_id);
        if(eheader_in_map != migrated_events.end()) {
          DEBUG_STREAM("event[" << event_id << "] is found in migrated events.");
          int param_id;
          switch(param_name)
          {
            case CL_PROFILING_COMMAND_QUEUED:
              param_id = 0;
              break;
            case CL_PROFILING_COMMAND_SUBMIT:
              param_id = 1;
              break;
            case CL_PROFILING_COMMAND_START:
              param_id = 2;
              break;
            case CL_PROFILING_COMMAND_END:
              param_id = 3;
              break;
            default:
              std::cout << "UKVM (warning): unknown param_name." << std::endl;
              param_id = 0;
              break;
          }

          std::memcpy((void*)param_value, 
              (void*)&(migrated_events[event_id].profiling_info[param_id]), 
              sizeof(cl_profiling_info));
          return;
        }

        std::cout << "UKVM (ERROR): event " << event_id << " is not found. " << std::endl;
        exit(1);
      }

      /**
       * call clWaitForEvents()
       */
      void wait_for_events(unsigned int num_events, int* event_list_ids)
      {
        cl_int err;

        if(num_events == 0 || event_list_ids == nullptr) {
          std::cout << "Error: No events are in the list. at least one event must be specified. " << std::endl;
          exit(1);
        }

        std::vector<cl::Event> wait_list;
        update_event_list(wait_list, num_events, event_list_ids);

        if(wait_list.size() == 0) {
          std::cout << "Warning: no events to be waited. " << std::endl;
          return;
        }
        OCL_CHECK(err, err = cl::Event::waitForEvents(wait_list));

        sync_flag=true;
      }


      /**
       * wait for the completion of enqueued tasks 
       */
      void sync_fpga(void)
      {
        DEBUG_STREAM("sync all cmdq.");
        std::cout << "MIG: sync all cmdq. \n";

        for (auto queue : queues)
        {
          DEBUG_STREAM("sync queue[" << queue.first << "]...");
          queue.second.finish();
        }

        sync_flag=true;
      }

      void sync_fpga(int cmdq_id)
      {
        DEBUG_STREAM("sync cmdq (id: " << cmdq_id << ")");

        auto queue_in_map = queues.find(cmdq_id);
        if(queue_in_map == queues.end()) {
          std::cout << "UKVM (ERROR): cmd queue " << cmdq_id << " is not found. " << std::endl;
          exit(1);
        }
        queue_in_map->second.finish();

        /* TODO: change here: if the guest creates multiple cmd queues, 
         * the context has to manage the same number of sync flags. 
         */
        sync_flag=true;
      }

      /** 
       * functions for task migration 
       */
      void save_bitstream(uint64_t addr, size_t size)
      {
        bin_guest_addr = addr;
        bin_size = size;
      }

      bool get_sync_flag()
      {
        return sync_flag;
      }

      bool get_updated_flag()
      {
        return updated_flag;
      }

      void sync_shared_buffers()
      {
        auto queue = queues[0];

        for(auto it : buffers)
        {
          auto id = it.first;
          auto buffer = it.second;

          cl_mem_flags mem_flags;
          buffer.getInfo(CL_MEM_FLAGS, &(mem_flags));

          /* data transfer from FPGA to Host */
          if( (mem_flags & CL_MEM_USE_HOST_PTR) && buffer_onfpga_flags[id] ) {
            cl_int err;
            OCL_CHECK(err, err = queue.enqueueMigrateMemObjects({buffer}, CL_MIGRATE_MEM_OBJECT_HOST));
          }
        }

        queue.finish();
      }

      std::vector<uint8_t>& save_fpga_memory()
      {
        size_t event_num;
        std::vector<struct eventobj_header> eheaders;
        std::vector<struct memobj_header> headers;
        std::map<int, std::vector<uint8_t>> saved_obj;
        size_t total_size = 0;

        /* read event objects (profiling info) */
        /*
         * FIXME: save migrated_events as well. 
         * Otherwise, when second or more migration happens, 
         * eventobj info saved by the previous migration is thrown away. 
         */
        event_num = events.size();
        total_size += sizeof(event_num);
        for(auto it : events)
        {
          auto id = it.first;
          auto event = it.second;
          struct eventobj_header eheader;
          eheader.event_id = id;
          event.getProfilingInfo<uint64_t>(CL_PROFILING_COMMAND_QUEUED,  &eheader.profiling_info[0]);
          event.getProfilingInfo<uint64_t>(CL_PROFILING_COMMAND_SUBMIT,  &eheader.profiling_info[1]);
          event.getProfilingInfo<uint64_t>(CL_PROFILING_COMMAND_START,   &eheader.profiling_info[2]);
          event.getProfilingInfo<uint64_t>(CL_PROFILING_COMMAND_END,     &eheader.profiling_info[3]);
          // event.getProfilingInfo<uint64_t>(CL_PROFILING_COMMAND_COMPLETE,&eheader.profiling_info[4]);

          eheaders.emplace_back(eheader);
          total_size += sizeof(struct eventobj_header);
        }

        /* read memory objects */
        for(auto it : buffers)
        {
          auto id = it.first;

          struct memobj_header header = {
            id, buffer_gpas[id], 0, 0, 0, buffer_onfpga_flags[id]
          };

          auto buffer = it.second; 
          buffer.getInfo(CL_MEM_SIZE, &(header.mem_size));
          buffer.getInfo(CL_MEM_TYPE, &(header.mem_type));
          buffer.getInfo(CL_MEM_FLAGS, &(header.mem_flags));
          DEBUG_STREAM("buffer[" << id << "]: \n" 
          << "gpa: 0x" << std::hex << header.gpa << ", size: " << header.mem_size 
          << ", type: " << header.mem_type << ", flag: " << header.mem_flags 
          << ", onfpga: " << header.onfpga_flag);

          headers.emplace_back(header);
          total_size += sizeof(struct memobj_header);

          /* 
           * if CL_MEM_USE_HOST_PTR is valid, 
           * the object is already written in guest memory 
           */
          if(header.mem_flags & CL_MEM_USE_HOST_PTR)
            continue;

          /* Read data on FPGA */
          if (header.onfpga_flag) {
            saved_obj.emplace(id, std::vector<uint8_t>(header.mem_size));
            auto data_ptr = saved_obj.find(id)->second.data();

            cl_int err;
            OCL_CHECK(err, err = queues[0].enqueueReadBuffer(buffer, CL_TRUE, 0, 
                  header.mem_size, data_ptr, nullptr, nullptr));
            total_size += header.mem_size;
          }
        }
        
        /* sync FPGA */
        queues[0].finish();

        /* save data into a single buffer */
        mig_save_data.resize(total_size);
        uint8_t* mig_data_ptr = mig_save_data.data();

        /* save eventobj */
        std::memcpy((void*)mig_data_ptr, (void*)&event_num, sizeof(event_num));
        mig_data_ptr += sizeof(event_num);
        for(auto eheader : eheaders)
        {
          /* copy header */
          std::memcpy((void*)mig_data_ptr, (void*)&eheader, sizeof(struct eventobj_header));
          mig_data_ptr += sizeof(struct eventobj_header);
        }

        /* save memobj */
        for(auto header : headers)
        {
          /* copy header */
          std::memcpy((void*)mig_data_ptr, (void*)&header, sizeof(struct memobj_header));
          mig_data_ptr += sizeof(struct memobj_header);

          /* copy data */
          // TODO: data copies here are redundant?
          auto obj = saved_obj.find(header.mem_id);
          if(obj != saved_obj.end()) {
            auto src_data_ptr = saved_obj.find(header.mem_id)->second.data();
            std::memcpy(mig_data_ptr, src_data_ptr, header.mem_size);
            mig_data_ptr += header.mem_size;
          }
        }

        DEBUG_STREAM("saved file size: " << total_size << " Bytes.");
        DEBUG_STREAM("event num: " << event_num << ", memobj num: " << mig_save_data.size()); 
        return mig_save_data;
      }

      bool load_fpga_memory(struct ukvm_hv *hv, void* load_data, size_t load_data_size)
      {
        auto current_ptr = (uint8_t *)load_data;
        auto end_ptr = (uint8_t *)load_data + load_data_size;

        /* load eventobj */
        size_t event_num;
        std::memcpy((void*)&event_num, (void*)current_ptr, sizeof(size_t));
        current_ptr += sizeof(size_t);
        DEBUG_STREAM("event num: " << event_num); 
        while(event_num > 0)
        {
          struct eventobj_header eh;
          std::memcpy((void*)&eh, (void*)current_ptr, sizeof(struct eventobj_header));
          current_ptr += sizeof(struct eventobj_header);

          migrated_events.emplace(eh.event_id, eh);
          DEBUG_STREAM("event (id: " << eh.event_id << "is loaded.");
          event_num--;
        }

        /* load memobj */
        while(current_ptr < end_ptr)
        {
          auto h = (struct memobj_header*) current_ptr;
          current_ptr += sizeof(struct memobj_header);

          void* host_ptr = UKVM_CHECKED_GPA_P(hv, h->gpa, h->mem_size);
          create_buffer(h->mem_id, h->mem_flags, h->mem_size, host_ptr, (void *)h->gpa);

          /* write memobj back into FPGA memory */
          if(h->onfpga_flag) 
          {
            cl_int err;
            /* load data from a host-side buffer in guest memory */
            if(h->mem_flags & CL_MEM_USE_HOST_PTR) {
              OCL_CHECK(err, err = queues[0].enqueueMigrateMemObjects({buffers[h->mem_id]}, 0));
            }
            /* load data from a migration file */
            else {
              OCL_CHECK(err, err = queues[0].enqueueWriteBuffer(buffers[h->mem_id], 
                    CL_TRUE, 0, h->mem_size, current_ptr, nullptr, nullptr));
              current_ptr += h->mem_size;
            }
          }

          /* sync FPGA */
          queues[0].finish();
        }

        if(current_ptr != end_ptr)
          std::cout << "Warning: read data size is not equal to the original size: " 
            << (uint64_t) current_ptr << ", " << (uint64_t) end_ptr << std::endl;

        updated_flag = true;
        return true;
      }

  }; // XoclContext

} // funky_backend

#endif  // __FUNKY_BACKEND_CONTEXT__
