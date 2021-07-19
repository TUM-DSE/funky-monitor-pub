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
    private:
      buffer::Reader<funky_msg::request>  request_q;
      buffer::Writer<funky_msg::response> response_q;

      uint64_t bin_guest_addr;
      size_t bin_size;
      cl::Context context;
      std::unique_ptr<cl::Program> program;
      cl::CommandQueue queue;

      // TODO: kernels that have been initialized once will be reused in the future? If not, we don't need to keep them here
      std::map<const char*, cl::Kernel> kernels;

      // TODO: consider cl::Pipe, cl::Image
      std::map<int, cl::Buffer> buffers;
      // TODO: cl::Event

    public:
      XoclContext(void* wr_queue_addr, void* rd_queue_addr) 
        : request_q(wr_queue_addr), response_q(rd_queue_addr), bin_guest_addr(0), bin_size(0)
      {
        cl_int err;

        std::cout << "UKVM: initializing Funky backend context....\n";

        // TODO: assign as many devices to the guest as requested  
        //       Currently, only one device (devices[0]) is assigned to the guest.
        auto devices = xcl::get_xil_devices();
        if(devices.size() == 0) {
          std::cout << "Error: no device is found.\n";
          exit(EXIT_FAILURE);
        }

        auto device = devices[0];

        // TODO: command queue option depends on the guest, so it shouldn't be created in advance?
        // Creating Context and Command Queue for selected Device
        OCL_CHECK(err, context = cl::Context(device, NULL, NULL, NULL, &err));
        OCL_CHECK(err, queue = cl::CommandQueue(context, device, CL_QUEUE_PROFILING_ENABLE, &err));
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
       * TODO: implement here 
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

        std::cout << "UKVM: trying to program device: " << p_devices[0].getInfo<CL_DEVICE_NAME>() << std::endl;

        if (err != CL_SUCCESS) {
          std::cout << "UKVM: failed to program device. \n";
          // exit(EXIT_FAILURE);
        } else {
          std::cout << "UKVM: succeeded to program device.\n";
        }

        return err;
      }

      /**
       * create buffer in global memory 
       */
      void create_buffer(int mem_id, uint64_t mem_flags, size_t size, void* host_ptr)
      {
        cl_int err;

        OCL_CHECK(err,  buffers.emplace(mem_id, cl::Buffer(context, (cl_mem_flags) mem_flags, size, host_ptr, &err)));
        // std::cout << "Succeeded to create buffer " << mem_id << std::endl;
      }

      int get_created_buffer_num()
      {
        return buffers.size();
      }

      /* create a new kernel */
      // TODO: return the kernel instance itself. 
      // set_arg() and create_kernel() will be also changed to use the instance as an argument
      void create_kernel(const char* kernel_name)
      {
        cl_int err;

        // TODO: search for the kernel map and if the same kernel already exists, skip the creation and use the existing one. 

        // OCL_CHECK(err, kernels[id] = cl::Kernel(program, kernel_name, &err));
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
          /* set variables other than OpenCL memory objects */
          if(src == nullptr) {
            std::cout << "arg addr error." << std::endl;
            return;
          }

          OCL_CHECK(err, err = kernels[id].setArg(arg->index, arg->size, (const void*)src));
        }
        else {
          /* set OpenCL memory objects as arguments */
          OCL_CHECK(err, err = kernels[id].setArg(arg->index, buffers[arg->mem_id]));
        }
      }

      /**
       * launch the kernel 
       */
      void enqueue_kernel(const char* kernel_name)
      {
        cl_int err;
        auto id = kernel_name;

        /* TODO: support for enqueueNDRangeKernel() */
        // For HLS kernels global and local size is always (1,1,1). So, it is recommended
        // to always use enqueueTask() for invoking HLS kernel
        OCL_CHECK(err, err = queue.enqueueTask(kernels[id]));
      }

      /**
       * Launch data transfer between Device Global memory and Host Local Memory 
       *
       * @param (mem_ids) : vector containing IDs of cl_mem objects being transferred from/to Host
       * @param (flags)   : 0 means from host, CL_MIGRATE_MEM_OBJECT_HOST means to host 
       *                     cl_mem_migration_flags, a bitfield based on unsigned int64
       */
      void enqueue_transfer(int mem_ids[], size_t id_num, uint64_t flags)
      {
        // TODO: error if buffers[i] does not exist
        cl_int err;
        for(size_t i=0; i<id_num; i++)
        {
          auto id = mem_ids[i];
          // std::cout << "enqueue transfer for object[" << id << "]..." << std::endl;
          /* do the memory migration */
          OCL_CHECK(err, err = queue.enqueueMigrateMemObjects({buffers[id]}, (cl_mem_migration_flags)flags));
        }

        /* make a list of cl_mem objects to be migrated */
        // cl::vector<cl::Memory> mig_buffers(mem_ids.size());

        // TODO: compare the execution time (call MigrateMemObjects() multiple times v.s. single call)
        // for(size_t i=0; i< mem_ids.size(); i++)
        //   mig_buffers.push_back(buffers[i]);

        // /* do the memory migration */
        // cl_uint err;
        // OCL_CHECK(err, err = queue.enqueueMigrateMemObjects(mig_buffers, (cl_mem_migration_flags)flags) );
      }

      /**
       * wait for the completion of enqueued tasks 
       */
      void sync_fpga()
      {
        queue.finish();
      }

      /** 
       * functions for task migration 
       *
       * TODO: design a hw_context class for migration and remove this function to that??
       */
      void save_bitstream(uint64_t addr, size_t size)
      {
        bin_guest_addr = addr;
        bin_size = size;
      }
  };


} // funky_backend

#endif  // __FUNKY_BACKEND_CONTEXT__
