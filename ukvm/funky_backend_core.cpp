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
#include <mutex>
#include "timer.h"

/* Xocl backend context class to save temp data & communicate with xocl lib */
std::unique_ptr<funky_backend::XoclContext> bk_context;

#include <curl/curl.h>
#include <cstdio>

#include <condition_variable>
#include <deque>
#include <iostream>

long long get_time_ms() {
    auto currentTime = std::chrono::system_clock::now();
    auto duration = currentTime.time_since_epoch();
    auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
    return milliseconds;
}

void push_duration(struct funky_metrics_collector *collector, double duration_ms, long long timestamp_ms, char *bitstream_identifier) {
    if (collector->endpoint == NULL) {
        printf("No endpoint specified for metrics collector, skipping push_duration\n");
        return;
    }

    printf("Pushing fpga usage duration: %f\n", duration_ms);

    CURL *curl;
    CURLcode res;

    curl_global_init(CURL_GLOBAL_ALL);

    curl = curl_easy_init();

    if(curl) {
        curl_easy_setopt(curl, CURLOPT_URL, collector->endpoint);

        // send duration as POST field
        auto post_field = "duration_ms=" + std::to_string(duration_ms) + "&timestamp_ms=" + std::to_string(timestamp_ms) + "&bitstream_identifier=" + std::string(bitstream_identifier);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_field.c_str());

        res = curl_easy_perform(curl);

        if(res != CURLE_OK)
            fprintf(stderr, "curl_easy_perform() failed: %s\n",
                    curl_easy_strerror(res));

        curl_easy_cleanup(curl);
    } else {
        printf("Failed to initialize curl\n");
    }

    curl_global_cleanup();

    printf("Pushed fpga usage duration to %s\n", collector->endpoint);
}

void funky_metrics_push_fpga_usage_duration(struct funky_metrics_collector *collector, double duration_ms, long long timestamp_ms, char *bitstream_identifier) {
    push_duration(collector, duration_ms, timestamp_ms, bitstream_identifier);
}

void push_reconfiguration_time(struct funky_metrics_collector *collector, long long timestamp_ms, double duration_ms, char *bitstream_identifier) {
    if (collector == nullptr || collector->endpoint == nullptr) {
        printf("No endpoint specified for metrics collector, skipping push_reconfiguration\n");
        return;
    }

    printf("Pushing reconfiguration\n");

    CURL *curl;
    CURLcode res;

    curl_global_init(CURL_GLOBAL_ALL);

    curl = curl_easy_init();

    if(curl) {
        curl_easy_setopt(curl, CURLOPT_URL, collector->endpoint);
        auto fields = "reconfiguration_ms=" + std::to_string(duration_ms) + "&timestamp_ms=" + std::to_string(timestamp_ms) + "&bitstream_identifier=" + std::string(bitstream_identifier);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, fields.c_str());

        res = curl_easy_perform(curl);

        if(res != CURLE_OK)
            fprintf(stderr, "curl_easy_perform() failed: %s\n",
                    curl_easy_strerror(res));

        curl_easy_cleanup(curl);
    }

    curl_global_cleanup();
}

void funky_metrics_push_reconfiguration_time(struct funky_metrics_collector *collector, long long timestamp_ms, double duration_ms, char *bitstream_identifier) {
    push_reconfiguration_time(collector, timestamp_ms, duration_ms, bitstream_identifier);
}

struct funky_metrics_collector *funky_metrics_collector_init(char *endpoint) {
    struct funky_metrics_collector *collector = (struct funky_metrics_collector *) malloc(sizeof(struct funky_metrics_collector));
    collector->endpoint = endpoint;

    return collector;
}

struct Request {
    funky_metrics_collector *collector;
    long long timestamp_ms;
    double duration_ms;
    double reconfiguration_ms;
    char *bitstream_identifier;
};

std::deque<Request> requestQueue;
std::mutex mtx;
std::condition_variable cv;
bool quit = false;

void metricsDispatcherThread() {
    while (true) {
        Request request;

        {
            std::unique_lock<std::mutex> lock(mtx);
            cv.wait(lock, []{ return quit || !requestQueue.empty(); });

            if (quit && requestQueue.empty())
                return;

            request = requestQueue.front();
            requestQueue.pop_front();
        }

        printf("Processing metrics dispatcher request\n");

        // Process the request
        if (request.reconfiguration_ms > 0) {
            funky_metrics_push_reconfiguration_time(request.collector, request.timestamp_ms, request.reconfiguration_ms, request.bitstream_identifier);
        } else {
            funky_metrics_push_fpga_usage_duration(request.collector, request.duration_ms, request.timestamp_ms, request.bitstream_identifier);
        }
    }
}


auto metrics_dispatcher_thr = std::thread (metricsDispatcherThread);

void add_dispatcher_request(Request request) {
    std::lock_guard<std::mutex> lock(mtx);
    requestQueue.push_back(request);
    cv.notify_one();
}

void finish_dispatcher() {
    {
        std::lock_guard<std::mutex> lock(mtx);
        quit = true;
    }
    cv.notify_one();
}

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


int reconfigure_fpga(void* bin, size_t bin_size, double *reconfigured)
{
  return bk_context->reconfigure_fpga(bin, bin_size, reconfigured);
}


int handle_fpga_requests(struct ukvm_hv *hv)
{
  return funky_backend::handle_fpga_requests(hv, bk_context.get());
}


/* FPGA worker thread */
std::unique_ptr<std::thread> fpga_worker;
std::unique_ptr<buffer::Reader<struct thr_msg>> msg_read_queue;
std::unique_ptr<buffer::Writer<struct thr_msg>> msg_write_queue;

void create_fpga_worker(struct fpga_thr_info thr_info)
{
  // std::cout << "UKVM: create a worker thread..." << std::endl;

  msg_read_queue  = std::make_unique<buffer::Reader<struct thr_msg>>(MSG_QUEUE_MAX_CAPACITY);
  msg_write_queue = std::make_unique<buffer::Writer<struct thr_msg>>(MSG_QUEUE_MAX_CAPACITY);

  auto fpga_worker_thread = [](struct fpga_thr_info thr_info, void* rq_addr, void* wq_addr)
  {
    // create span and start measuring fpga workload duration
    cPerfTimer *timer = new cPerfTimer();
    timer->start();

    funky_backend::Worker worker(thr_info, rq_addr, wq_addr);

    /* reconfigure FPGA & load FPGA memory from migration file (if mig_file exists) */
    double reconfigured = worker.reconfigure_fpga();
    if (reconfigured > 0) {
      add_dispatcher_request({
        .collector = thr_info.metrics_collector,
        .timestamp_ms = get_time_ms(),
        .duration_ms = 0,
        .reconfiguration_ms = reconfigured,
        .bitstream_identifier = thr_info.bitstream_identifier
      });
    }

    worker.load_fpga();

    /* inform another thread that Worker has been initialized */
    struct thr_msg init_msg = {MSG_INIT, NULL, 0};
    worker.send_msg(init_msg);

    /* 
     * do polling cmd queues until the monitor thread sends a 'savevm' request. 
     *
     * When the worker thread receives the request, it reads data from FPGA memory 
     * and save them into a contiguous region, and exit (an actual FPGA is released then). 
     * The saved data is sent to the monitor and the monitor writes data into a file. 
     * 
     * */
    // auto req = worker.recv_msg();
    enum ThrMsgType recv_msg_type;
    bool req_flag = false;
    auto cb = [&](struct thr_msg* recv_msg){ 
      recv_msg_type = recv_msg->msg_type; 
      return false;
    };

    while(!req_flag) {
      worker.handle_fpga_requests();
      // req = worker.recv_msg();
      req_flag = worker.recv_msg_with_callback(cb);
    }

    // Record one duration for all the handled requests (like a session)
    timer->stop();
    add_dispatcher_request({
        .collector = thr_info.metrics_collector,
        .timestamp_ms = get_time_ms(),
        .duration_ms = timer->get_ms(),
        .reconfiguration_ms = 0,
        .bitstream_identifier = thr_info.bitstream_identifier
    });

    /* Handling messages from the other threads (monitor, vCPU) */
    // switch(req->msg_type) {
    switch(recv_msg_type) {
      case MSG_KILLWORKER:
      {
        // Wait for outstanding requests to complete here
        finish_dispatcher();
        printf("Waiting for metrics dispatcher thread to finish...\n");
        metrics_dispatcher_thr.join();

        std::cout << "MSG_KILLWORKER request has been received.\n";
        struct thr_msg end_msg = {MSG_END, NULL, 0};

        worker.send_msg(end_msg);

        return; // kill the thread by itself
      }
      case MSG_SAVEFPGA:
        break;
      default: 
        std::cout << "Warning: undefined msg type. \n";
        break;
    }

    DEBUG_STREAM("received SAVEFPGA request.");

    /* 
     * start VM (FPGA) migration 
     * */
    /* handle incoming FPGA requests until a sync point (e.g., just after clFinish()) */
    /* if it does not reach to a sync point, the worker thread sync the FPGA by itself. 
     *
     * TODO: this is not allowed if pipelined kernels are enqueued. 
     */
    if(worker.check_sync_point() == false)
      worker.sync_fpga_by_worker();
      // worker.handle_fpga_requests();

    /* 
     * Write back OpenCL memory objects with CL_MEM_USE_HOST_PTR flag to guest memory 
     *
     * If any memory object is initialized with 'CL_MEM_USE_HOST_PTR' flag, 
     * the app and the FPGA share the same memory space i.e., memory 
     * referenced by host_ptr is used as the storage bits for the memory object.
     * Because host_ptr is within the guest VM memory space, object data on FPGA 
     * must be written back in the guest memory before starting to save VM contexts. 
     *
     * */
    worker.sync_fpga_memory();

    /* send 'sync' msg to Monitor */
    struct thr_msg sync_msg = {MSG_SYNCED, NULL, 0};
    worker.send_msg(sync_msg);

    /* callback function for KILLWORKER msg */
    auto cb_for_killworker = [](struct thr_msg* msg) {
      if(msg->msg_type != MSG_KILLWORKER)
        std::cout << "Warning: received a different req: " << msg->msg_type << "\n"; 
    };

    if(worker.is_fpga_updated()) {
      DEBUG_STREAM("worker: saving FPGA data...");

      /* send 'updated' msg to vCPU */
      auto p_thr_info = worker.get_thr_info();
      struct thr_msg updated_msg = {MSG_UPDATED, p_thr_info, sizeof(struct fpga_thr_info)};
      worker.send_msg(updated_msg);

      /* save FPGA data (Meanwhile, vCPU is performing savevm()) */
      auto save_data = worker.save_fpga(); 

      /* send FPGA data to vCPU */
      struct thr_msg data_msg = {MSG_SAVED, save_data.data(), save_data.size()};
      worker.send_msg(data_msg);

      /* wait for the completion of migration/eviction */
      // TODO: check when save_data is released. 
      bool received = false; 
      while(!received)
        received = worker.recv_msg_with_callback(cb_for_killworker);

      DEBUG_STREAM("worker: going to be killed...");
      struct thr_msg end_msg = {MSG_END, NULL, 0};
      worker.send_msg(end_msg);
    }
    else {
      DEBUG_STREAM("worker: there's no data to be saved on FPGA.");
      auto p_thr_info = worker.get_thr_info();
      struct thr_msg init_msg = {MSG_INIT, p_thr_info, sizeof(struct fpga_thr_info)};
      worker.send_msg(init_msg);

      bool received = false;
      while(!received)
        received = worker.recv_msg_with_callback(cb_for_killworker);

      struct thr_msg end_msg = {MSG_END, NULL, 0};
      worker.send_msg(end_msg);
        DEBUG_STREAM("worker: being destroyed...");
    }
  };

  fpga_worker = std::make_unique<std::thread>(
      fpga_worker_thread, 
      thr_info, 
      msg_read_queue->get_baseaddr(), 
      msg_write_queue->get_baseaddr()
      );
}

std::mutex mtx_worker_msgq_read;
int recv_msg_from_worker(struct thr_msg* msg)
{
  std::lock_guard<std::mutex> guard(mtx_worker_msgq_read);
  auto recv_msg = msg_read_queue->read();
  while (recv_msg == nullptr)
    recv_msg = msg_read_queue->read();

  std::memcpy(msg, recv_msg, sizeof(struct thr_msg));
  msg_read_queue->update();
  
  // TODO: return 0 if failed
  return 1;
}

std::mutex mtx_worker_msgq_write;
int send_msg_to_worker(struct thr_msg *msg)
{
  std::lock_guard<std::mutex> guard(mtx_worker_msgq_write);
  auto ret = msg_write_queue->push(*msg);
  return (ret)? 1: 0;
}

void destroy_fpga_worker()
{
  DEBUG_STREAM("UKVM: destroy a worker thread...");

  /* let the worker thread kill itself */
  struct thr_msg destroy_req = {MSG_KILLWORKER, NULL, 0};
  send_msg_to_worker(&destroy_req);

  /* confirm if the worker thread has been killed before releasing msg queues. */
  struct thr_msg recv_msg;
  recv_msg_from_worker(&recv_msg);
  while(recv_msg.msg_type != MSG_END)
  {
    if(recv_msg.msg_type != MSG_INIT)
      std::cout << "Warning: received " << recv_msg.msg_type << ". Read again...\n"; 

    recv_msg_from_worker(&recv_msg);
  }
  std::cout << "UKVM: confirm the worker thread is going to be destroyed.\n";

  /* release the smart pointer */
  msg_read_queue.release();
  msg_write_queue.release();
  fpga_worker.release();
}

int is_fpga_worker_alive()
{
  return (fpga_worker)? 1: 0;
}

