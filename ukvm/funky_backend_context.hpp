#ifndef __FUNKY_BACKEND_CONTEXT__
#define __FUNKY_BACKEND_CONTEXT__

#include <CL/cl2.hpp>
#include <CL/cl_ext_xilinx.h>
#include "funky_xcl2.hpp"

// TODO: change file name to funky_msg.hpp
#include "backend/buffer.hpp"
#include "backend/funky_msg.hpp"

#include "ukvm.h" // UKVM_CHECKED_GPA_P()

#include <memory> // make_unique
#include <algorithm>
#include <vector>
#include <map>

// TODO: user funky_hw_context to save/restore data on FPGA
// #include "funky_hw_context.hpp"

namespace funky_backend {

  class XoclContext {
    public:
      struct memobj_header
      {
        int mem_id;
        ukvm_gpa_t gpa;
        size_t mem_size;
        cl_mem_object_type mem_type;
        cl_mem_flags mem_flags;
        bool onfpga_flag; //if true, data needs to be copied from src FPGA mem to dest FPGA mem
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
      // cl::CommandQueue queue;

      // TODO: kernels that have been initialized once will be reused in the future? If not, we don't need to keep them here
      std::map<const char*, cl::Kernel> kernels;

      // TODO: consider cl::Pipe, cl::Image
      std::map<int, cl::Buffer> buffers;
      std::map<int, bool> buffer_onfpga_flags;
      std::map<int, ukvm_gpa_t> buffer_gpas;

      // TODO: cl::Event

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
      }

      ~XoclContext()
      {}

      /** 
       * receive a request by polling the cmd queue 
       */
      funky_msg::request* read_request()
      {
        return request_q.pop();
      }

      /**  
       * sending a response to the guest via cmd queue
       */
      bool send_response(funky_msg::ReqType type)
      {
        funky_msg::response response(type);
        return response_q.push(response);
      }

      /** 
       * reconfigure FPGA with a bitstream 
       */
      int reconfigure_fpga(void* bin, size_t bin_size)
      {
        cl_int err = CL_SUCCESS;

        cl::Program::Binaries bins{{bin, bin_size}};
        auto devices = xcl::get_xil_devices();
        std::vector <cl::Device> p_devices = {devices[0]};

        /* program bistream to the device (FPGA) */
        program = std::make_unique<cl::Program>(context, p_devices, bins, nullptr, &err);

        return err;
      }

      /**
       * create buffer in global memory 
       */
      void create_buffer(int mem_id, uint64_t mem_flags, size_t size, void* host_ptr, void* gpa)
      {
        cl_int err;

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

      /* create a new kernel */
      // TODO: return the kernel instance itself?
      // set_arg() and create_kernel() will be also changed to use the instance as an argument
      void create_kernel(const char* kernel_name)
      {
        cl_int err;

        // TODO: search for the kernel map and if the same kernel already exists, skip the creation and use the existing one. 

        /* use kernel name as an index */
        OCL_CHECK(err, kernels.emplace(kernel_name, cl::Kernel(*program, kernel_name, &err)));
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

          OCL_CHECK(err, err = kernels[id].setArg(arg->index, arg->size, (const void*)src));
        }
        else {
          /* OpenCL memory objects */
          OCL_CHECK(err, err = kernels[id].setArg(arg->index, buffers[arg->mem_id]));

          /* Memory objects specified as kernel arguments must be on FPGA */
          buffer_onfpga_flags[arg->mem_id] = true;
        }
      }

      /**
       * launch the kernel 
       */
      void enqueue_kernel(int msgq_id, const char* kernel_name)
      {
        cl_int err;
        auto id = kernel_name;

        /* create a cmd queue if not exists */
        auto queue_in_map = queues.find(msgq_id);
        if(queue_in_map == queues.end()) {
          // auto devices = xcl::get_xil_devices();
          // if(devices.size() == 0) {
          //   exit(EXIT_FAILURE);
          // }
          // auto device = devices[0];

          OCL_CHECK(err,  queues.emplace(msgq_id, cl::CommandQueue(context, device, CL_QUEUE_PROFILING_ENABLE, &err)));
          std::cout << "UKVM: new cmd queue (id: " << msgq_id << ") is created. " << std::endl;
        }

        /* TODO: support for enqueueNDRangeKernel() */
        // For HLS kernels global and local size is always (1,1,1). So, it is recommended
        // to always use enqueueTask() for invoking HLS kernel
        OCL_CHECK(err, err = queues[msgq_id].enqueueTask(kernels[id]));

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
      void enqueue_transfer(int msgq_id, int mem_ids[], size_t id_num, uint64_t flags)
      {
        /* create a cmd queue if not exists */
        auto queue_in_map = queues.find(msgq_id);
        if(queue_in_map == queues.end()) {
          cl_int err;
          OCL_CHECK(err,  queues.emplace(msgq_id, cl::CommandQueue(context, device, CL_QUEUE_PROFILING_ENABLE, &err)));
          std::cout << "UKVM: new cmd queue (id: " << msgq_id << ") is created. " << std::endl;
        }

        for(size_t i=0; i<id_num; i++)
        {

          // TODO: consider when enqueueReadBuffer or enqueueWriteBuffer is called
          cl_int err;
          auto id = mem_ids[i];
          // OCL_CHECK(err, err = queue.enqueueMigrateMemObjects({buffers[id]}, (cl_mem_migration_flags)flags));
          OCL_CHECK(err, err = queues[msgq_id].enqueueMigrateMemObjects({buffers[id]}, (cl_mem_migration_flags)flags));

          /* 0 means data transfers from Host to FPGA */
          if( flags == 0 )
            buffer_onfpga_flags[id] = true;
        }

        sync_flag = false;
        updated_flag = true;
      }

      /**
       * wait for the completion of enqueued tasks 
       */
      void sync_fpga(int cmdq_id)
      {
        auto queue_in_map = queues.find(cmdq_id);
        if(queue_in_map == queues.end()) {
          std::cout << "UKVM (ERROR): cmd queue " << cmdq_id << " is not found. " << std::endl;
          exit(1);
        }

        // queues[cmdq_id].finish();
        // queue_in_map->second.data()->finish();
        queue_in_map->second.finish();

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
        std::vector<struct memobj_header> headers;
        std::map<int, std::vector<uint8_t>> saved_obj;
        size_t total_size = 0;

        /* Generate memory object headers */
        for(auto it : buffers)
        {
          auto id = it.first;

          struct memobj_header header = {
            id, buffer_gpas[id], 0, 0, 0, buffer_onfpga_flags[id]};

          auto buffer = it.second; 
          buffer.getInfo(CL_MEM_SIZE, &(header.mem_size));
          buffer.getInfo(CL_MEM_TYPE, &(header.mem_type));
          buffer.getInfo(CL_MEM_FLAGS, &(header.mem_flags));
          // std::cout << "buffer[" << id << "]: " << std::endl
          //   << "gpa: 0x" << std::hex << header.gpa << ", size: " << header.mem_size 
          //   << ", type: " << header.mem_type << ", flag: " << header.mem_flags 
          //   << ", onfpga: " << header.onfpga_flag << std::endl;

          headers.emplace_back(header);
          total_size += sizeof(struct memobj_header);

          /* if CL_MEM_USE_HOST_PTR is valid, 
           * the object is already written in guest memory */
          if(header.mem_flags & CL_MEM_USE_HOST_PTR)
            continue;

          /* Read data on FPGA */
          if (header.onfpga_flag) {
            saved_obj.emplace(id, std::vector<uint8_t>(header.mem_size));
            auto data_ptr = saved_obj.find(id)->second.data();

            cl_int err;
            // OCL_CHECK(err, err = queue.enqueueReadBuffer(buffer, CL_TRUE, 0, 
            OCL_CHECK(err, err = queues[0].enqueueReadBuffer(buffer, CL_TRUE, 0, 
                  header.mem_size, data_ptr, nullptr, nullptr));
            total_size += header.mem_size;
          }
        }
        
        /* Wait for data transfers from FPGA (sync) */
        // queue.finish();
        queues[0].finish();

        /* Copy data into a single buffer */
        mig_save_data.resize(total_size);
        uint8_t* mig_data_ptr = mig_save_data.data();

        for(auto header : headers)
        {
          std::memcpy((void*)mig_data_ptr, (void*)&header, sizeof(struct memobj_header));
          mig_data_ptr += sizeof(struct memobj_header);

          // TODO: data copies here are redundant?
          auto obj = saved_obj.find(header.mem_id);
          if(obj != saved_obj.end()) {
            auto src_data_ptr = saved_obj.find(header.mem_id)->second.data();
            std::memcpy(mig_data_ptr, src_data_ptr, header.mem_size);
            mig_data_ptr += header.mem_size;
          }
        }

        // std::cout << "total file size: " << total_size << " Bytes. (num of buffer elements: " << mig_save_data.size() << ")" << std::endl; 

        return mig_save_data;
      }

      bool load_fpga_memory(struct ukvm_hv *hv, void* load_data, size_t load_data_size)
      {
        auto current_ptr = (uint8_t *)load_data;
        auto end_ptr = (uint8_t *)load_data + load_data_size;

        while(current_ptr < end_ptr)
        {
          auto h = (struct memobj_header*) current_ptr;
          current_ptr += sizeof(struct memobj_header);

          void* host_ptr = UKVM_CHECKED_GPA_P(hv, h->gpa, h->mem_size);
          create_buffer(h->mem_id, h->mem_flags, h->mem_size, host_ptr, (void *)h->gpa);

          if(h->onfpga_flag) 
          {
            cl_int err;
            if(h->mem_flags & CL_MEM_USE_HOST_PTR) {
              /* Restore data from a cache in guest memory space */
              // OCL_CHECK(err, err = queue.enqueueMigrateMemObjects({buffers[h->mem_id]}, 0));
              OCL_CHECK(err, err = queues[0].enqueueMigrateMemObjects({buffers[h->mem_id]}, 0));
            }
            else {
              /* Restore data from migration file */
              // OCL_CHECK(err, err = queue.enqueueWriteBuffer(buffers[h->mem_id], 
              OCL_CHECK(err, err = queues[0].enqueueWriteBuffer(buffers[h->mem_id], 
                    CL_TRUE, 0, h->mem_size, current_ptr, nullptr, nullptr));
              current_ptr += h->mem_size;
            }
          }

          /* Wait for data transfers from FPGA (sync) */
          // queue.finish();
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
