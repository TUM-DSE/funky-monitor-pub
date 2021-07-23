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
// TODO: if being used by a worker thread, the thread instanciates this context
// TODO: This must be explicitly released if the guest release FPGA (if not, SIGSEGV may occur)
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





/* test functions */
std::unique_ptr<buffer::Reader<funky_msg::request>>  request_q;
std::unique_ptr<buffer::Writer<funky_msg::response>> response_q;

int register_cmd_queues(void* wr_queue_addr, void* rd_queue_addr) {
  request_q = std::make_unique<buffer::Reader<funky_msg::request>>(wr_queue_addr);
  response_q = std::make_unique<buffer::Writer<funky_msg::response>>(rd_queue_addr);

  /* receive an TRANSFER request and try to read it */
  std::cout << "UKVM: test a cmd queue..." << std::endl;
  auto req = request_q->pop();
  while(req == NULL) // buffer is empty
    req = request_q->pop();

  if(req->get_request_type() == funky_msg::TRANSFER)
    std::cout << "received a TRANSFER request." << std::endl;

  /* receive an EXEC request and try to read it */
  req = request_q->pop();
  while(req == NULL) // buffer is empty
    req = request_q->pop();

  if(req->get_request_type() == funky_msg::EXECUTE)
    std::cout << "received an EXEC request." << std::endl;

  return 0;
}

int test_program_fpga(void* bin, size_t bin_size)
{
  cl_int err;
  cl::Context context;
  // cl::Kernel kernel;
  // cl::CommandQueue q;

  auto devices = xcl::get_xil_devices();
  cl::Program::Binaries bins{{bin, bin_size}};

  bool valid_device = false;
  for (unsigned int i = 0; i < devices.size(); i++) {
    auto device = devices[i];
    // Creating Context and Command Queue for selected Device
    OCL_CHECK(err, context = cl::Context(device, NULL, NULL, NULL, &err));
    // OCL_CHECK(err, q = cl::CommandQueue(context, device, CL_QUEUE_PROFILING_ENABLE, &err));

    std::cout << "Trying to program device[" << i << "]: " << device.getInfo<CL_DEVICE_NAME>() << std::endl;
    cl::Program program(context, {device}, bins, NULL, &err);
    if (err != CL_SUCCESS) {
      std::cout << "Failed to program device[" << i << "] with xclbin file!\n";
    } else {
      std::cout << "Device[" << i << "]: program successful!\n";
      // OCL_CHECK(err, krnl_vector_add = cl::Kernel(program, "vadd", &err));
      valid_device = true;
      break; // we break because we found a valid device
    }
  }
  if (!valid_device) {
    std::cout << "Failed to program any device found, exit!\n";
    exit(EXIT_FAILURE);
  }

  return 0;
}

int hello_fpga(char* bin_name) {
    std::string binaryFile = bin_name;
    size_t vector_size_bytes = sizeof(int) * DATA_SIZE;
    cl_int err;
    cl::Context context;
    cl::Kernel krnl_vector_add;
    cl::CommandQueue q;
    // Allocate Memory in Host Memory
    // When creating a buffer with user pointer (CL_MEM_USE_HOST_PTR), under the
    // hood user ptr
    // is used if it is properly aligned. when not aligned, runtime had no choice
    // but to create
    // its own host side buffer. So it is recommended to use this allocator if
    // user wish to
    // create buffer using CL_MEM_USE_HOST_PTR to align user buffer to page
    // boundary. It will
    // ensure that user buffer is used when user create Buffer/Mem object with
    // CL_MEM_USE_HOST_PTR
    std::vector<int, aligned_allocator<int> > source_in1(DATA_SIZE);
    std::vector<int, aligned_allocator<int> > source_in2(DATA_SIZE);
    std::vector<int, aligned_allocator<int> > source_hw_results(DATA_SIZE);
    std::vector<int, aligned_allocator<int> > source_sw_results(DATA_SIZE);

    // Create the test data
    std::generate(source_in1.begin(), source_in1.end(), std::rand);
    std::generate(source_in2.begin(), source_in2.end(), std::rand);
    for (int i = 0; i < DATA_SIZE; i++) {
        source_sw_results[i] = source_in1[i] + source_in2[i];
        source_hw_results[i] = 0;
    }

    // OPENCL HOST CODE AREA START
    // get_xil_devices() is a utility API which will find the xilinx
    // platforms and will return list of devices connected to Xilinx platform
    auto devices = xcl::get_xil_devices();

    // read_binary_file() is a utility API which will load the binaryFile
    // and will return the pointer to file buffer.
    auto fileBuf = xcl::read_binary_file(binaryFile);

    cl::Program::Binaries bins{{fileBuf.data(), fileBuf.size()}};
    bool valid_device = false;
    for (unsigned int i = 0; i < devices.size(); i++) {
        auto device = devices[i];
        // Creating Context and Command Queue for selected Device
        OCL_CHECK(err, context = cl::Context(device, NULL, NULL, NULL, &err));
        OCL_CHECK(err, q = cl::CommandQueue(context, device, CL_QUEUE_PROFILING_ENABLE, &err));
        std::cout << "Trying to program device[" << i << "]: " << device.getInfo<CL_DEVICE_NAME>() << std::endl;
        cl::Program program(context, {device}, bins, NULL, &err);
        if (err != CL_SUCCESS) {
            std::cout << "Failed to program device[" << i << "] with xclbin file!\n";
        } else {
            std::cout << "Device[" << i << "]: program successful!\n";
            OCL_CHECK(err, krnl_vector_add = cl::Kernel(program, "vadd", &err));
            valid_device = true;
            break; // we break because we found a valid device
        }
    }
    if (!valid_device) {
        std::cout << "Failed to program any device found, exit!\n";
        exit(EXIT_FAILURE);
    }

    // Allocate Buffer in Global Memory
    // Buffers are allocated using CL_MEM_USE_HOST_PTR for efficient memory and
    // Device-to-host communication
    OCL_CHECK(err, cl::Buffer buffer_in1(context, CL_MEM_USE_HOST_PTR | CL_MEM_READ_ONLY, vector_size_bytes,
                                         source_in1.data(), &err));
    OCL_CHECK(err, cl::Buffer buffer_in2(context, CL_MEM_USE_HOST_PTR | CL_MEM_READ_ONLY, vector_size_bytes,
                                         source_in2.data(), &err));
    OCL_CHECK(err, cl::Buffer buffer_output(context, CL_MEM_USE_HOST_PTR | CL_MEM_WRITE_ONLY, vector_size_bytes,
                                            source_hw_results.data(), &err));

    int size = DATA_SIZE;
    OCL_CHECK(err, err = krnl_vector_add.setArg(0, buffer_in1));
    OCL_CHECK(err, err = krnl_vector_add.setArg(1, buffer_in2));
    OCL_CHECK(err, err = krnl_vector_add.setArg(2, buffer_output));
    OCL_CHECK(err, err = krnl_vector_add.setArg(3, size));

    // Copy input data to device global memory
    OCL_CHECK(err, err = q.enqueueMigrateMemObjects({buffer_in1, buffer_in2}, 0 /* 0 means from host*/));

    // Launch the Kernel
    // For HLS kernels global and local size is always (1,1,1). So, it isi recommended
    // to always use enqueueTask() for invoking HLS kernel
    OCL_CHECK(err, err = q.enqueueTask(krnl_vector_add));

    // Copy Result from Device Global Memory to Host Local Memory
    OCL_CHECK(err, err = q.enqueueMigrateMemObjects({buffer_output}, CL_MIGRATE_MEM_OBJECT_HOST));

    q.finish();
    // OPENCL HOST CODE AREA END

    // Compare the results of the Device to the simulation
    bool match = true;
    for (int i = 0; i < DATA_SIZE; i++) {
        if (source_hw_results[i] != source_sw_results[i]) {
            std::cout << "Error: Result mismatch" << std::endl;
            std::cout << "i = " << i << " CPU result = " << source_sw_results[i]
                      << " Device result = " << source_hw_results[i] << std::endl;
            match = false;
            break;
        }
    }

    std::cout << "TEST " << (match ? "PASSED" : "FAILED") << std::endl;
    return (match ? EXIT_SUCCESS : EXIT_FAILURE);
}
