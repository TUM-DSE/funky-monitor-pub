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
#include "funky_backend_worker.hpp"

#include <cstdint>
#include <csignal>
#include <memory>
#include <algorithm>
#include <vector>
#include <thread>
#define DATA_SIZE 4096

/* Xocl backend context class to save temp data & communicate with xocl lib */
std::unique_ptr<funky_backend::XoclContext> bk_context;

int allocate_fpga(void* wr_queue_addr, void* rd_queue_addr) {
  if(bk_context != nullptr) {
    std::cout << "Warning: xocl context already exists." << std::endl;
    return -1;
  }

  bk_context = std::make_unique<funky_backend::XoclContext>(wr_queue_addr, rd_queue_addr);
  return 0;
}


/** 
 * release the backend context.
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


int handle_fpga_requests(struct ukvm_hv *hv)
{
  return funky_backend::handle_fpga_requests(hv, bk_context.get());
}


/* FPGA worker thread */
std::unique_ptr<std::thread> fpga_worker;

void create_fpga_worker(struct fpga_thr_info thr_info)
{
  std::cout << "UKVM: create a worker thread..." << std::endl;

  auto fpga_worker_thread = [&](void)
  {
    funky_backend::Worker worker(thr_info);
    worker.run();
  };

  fpga_worker = std::make_unique<std::thread>(fpga_worker_thread);
}


void destroy_fpga_worker()
{
  std::cout << "UKVM: destroy a worker thread... (TBD)" << std::endl;
  fpga_worker.release();
}
