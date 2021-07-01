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

// TODO: user funky_hw_context to save/restore data on FPGA
// #include "funky_hw_context.hpp"

namespace funky_backend {

  class XoclContext {
    private:
      buffer::Reader<funky_msg::request>  request_q;
      buffer::Writer<funky_msg::response> response_q;

      // std::string binaryFile;
      // std::vector<cl::Platform> platforms;
      // std::vector<cl::Device> devices;
      // cl::Device *device;
      cl::Context context;
      std::unique_ptr<cl::Program> program;
      // cl::Program program;
      cl::CommandQueue queue;

      int kernel_num;
      std::vector<cl::Kernel> kernels; 

      // TODO: consider cl::Pipe, cl::Image
      int buffer_num;
      std::vector<cl::Buffer> buffers;
      // TODO: cl::Event

    public:
      XoclContext(void* wr_queue_addr, void* rd_queue_addr) 
        : request_q(wr_queue_addr), response_q(rd_queue_addr), kernel_num(0), buffer_num(0)
      {
        cl_int err;

        std::cout << "initializing Funky backend context....\n";

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

      /* receive a request by polling the cmd queue */
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

      /* program kernels on FPGA */
      int reconfigure_fpga(void* bin, size_t bin_size)
      {
        cl_int err = CL_SUCCESS;

        cl::Program::Binaries bins{{bin, bin_size}};
        auto devices = xcl::get_xil_devices();
        std::vector <cl::Device> p_devices = {devices[0]};

        // std::vector<cl::Device> devices = {device};
        // auto test_program = cl::Program(context, {device}, bins, NULL, &err);
        program = std::make_unique<cl::Program>(context, p_devices, bins, nullptr, &err);
        // auto tmp_program = cl::Program(context, p_devices, bins, nullptr, &err);

        // std::cout << "Trying to program device: " << device.getInfo<CL_DEVICE_NAME>() << std::endl;
        std::cout << "Trying to program device: " << p_devices[0].getInfo<CL_DEVICE_NAME>() << std::endl;

        if (err != CL_SUCCESS) {
          std::cout << "Failed to program device. \n";
          // exit(EXIT_FAILURE);
        } else {
          std::cout << "Succeeded to program device.\n";
        }

        return err;
      }

      /* allocate buffer in global memory */
      void create_buffer(uint64_t mem_flags, size_t size, void* host_ptr)
      {
        cl_int err;
        auto id = buffer_num;
        OCL_CHECK(err,  buffers[id] = cl::Buffer(context, (cl_mem_flags) mem_flags, size, host_ptr, &err));
        buffer_num++;
      }

      /* set OpenCL memory objects as arguments */
      void set_arg(int kernel_id, funky_msg::arg_info* arg)
      {
        cl_int err;
        auto id = kernel_id;

        if(arg->mem_id == -1) {
          /* set variables other than OpenCL memory objects */
          OCL_CHECK(err, err = kernels[id].setArg(arg->index, arg->size, (const void*)arg->src));
        }
        else {
          /* set OpenCL memory objects as arguments */
          OCL_CHECK(err, err = kernels[id].setArg(arg->index, buffers[arg->mem_id]));
        }
      }

      /* Launch the Kernel */
      void enqueue_kernel(char* kernel_name, uint32_t arg_num, funky_msg::arg_info** args)
      {
        cl_int err;

        auto id = kernel_num;

        /* initialize a new kernel */
        // OCL_CHECK(err, kernels[id] = cl::Kernel(program, kernel_name, &err));
        OCL_CHECK(err, kernels[id] = cl::Kernel(*program, kernel_name, &err));

        /* assign arguments to the kernel */
        for (uint32_t i=0; i<arg_num; i++)
          set_arg(id, args[arg_num]);

        /* TODO: support for enqueueNDRangeKernel() */
        // For HLS kernels global and local size is always (1,1,1). So, it is recommended
        // to always use enqueueTask() for invoking HLS kernel
        OCL_CHECK(err, err = queue.enqueueTask(kernels[id]));

        kernel_num++;
      }

      /**
       * Launch data transfer between Device Global memory and Host Local Memory 
       *
       * @param (mem_ids) : vector containing IDs of cl_mem objects being transferred from/to Host
       * @param (flags)   : 0 means from host, CL_MIGRATE_MEM_OBJECT_HOST means to host 
       *                     cl_mem_migration_flags, a bitfield based on unsigned int64
       */
      void enqueue_transfer(std::vector<int> mem_ids, uint64_t flags)
      {
        /* make a list of cl_mem objects to be migrated */
        cl::vector<cl::Memory> mig_buffers(mem_ids.size());
        // TODO: error if buffers[i] does not exist
        for(size_t i=0; i< mem_ids.size(); i++)
          mig_buffers.push_back(buffers[i]);

        /* do the memory migration */
        cl_uint err;
        OCL_CHECK(err, err = queue.enqueueMigrateMemObjects(mig_buffers, (cl_mem_migration_flags)flags) );
      }

      /* wait for the completion of enqueued tasks */
      void sync_fpga()
      {
        queue.finish();
      }
  };


} // funky_backend

#endif  // __FUNKY_BACKEND_CONTEXT__
