#ifndef __FUNKY_CMD_HPP__
#define __FUNKY_CMD_HPP__

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <assert.h>

/*
 * The request/response classes are designed to be independent from OpenCL. 
 * Some FPGA platforms or Shells (e.g., Coyote, AmorphOS) do not support OpenCL but their original interfaces. 
 * OpenCL-dependent info is generalized here (unique to FPGA characteristics) so that 
 * platform-dependent backend functions can be easily developed. 
 * This makes porting Funky system to any existing/future vendor-dependent FPGA platform.
 *
 */

// TODO: change namespace? (e.g., funky_cmd? msg?)
namespace funky_msg {
  enum ReqType {MEMORY, TRANSFER, EXECUTE, SYNC};
  enum MemType {BUFFER, PIPE, IMAGE};

  /**
   * mem_info is used to initialize memory objects on FPGA with input/output data.
   * data flow/depenency of tasks is defined by arg_info in the subsequent EXEC resuests.
   *
   * */
  struct mem_info {
    int id;
    MemType type;
    uint64_t flags; /* read-only, write-only, ... */
    void* src;
    size_t size;

    mem_info(int mem_id, MemType mem_type, uint64_t mem_flags, void* mem_src, size_t mem_size)
      : id(mem_id), type(mem_type), flags(mem_flags), src(mem_src), size(mem_size)
    {}
  };

  /**
   * mem_info is used to transfer data between Host and FPGA memory. 
   * Memory objects must be already created in the backend context. 
   *
   * */
  struct transfer_info {
    int* ids; /* ids of memory to be transferred */
    size_t num; /* number of memory to be transferred */
    uint64_t flags; /* host to device, or device to host */

    transfer_info(int* mem_ids, size_t mem_num, uint64_t trans_flags)
      : ids(mem_ids), num(mem_num), flags(trans_flags)
    {}
  };

  /* 
   * arg_info is used to initialize arguments of FPGA tasks (e.g., input/output data).
   * OpenCL does not care a type of args, but ukvm must know if they are OpenCL memory objects or not
   * because OpenCL objects are recreated by ukvm for an actual task execution. 
   * On the other hand, if an arg is a non-OpenCL object, guest memory is referred to obtain the value.
   *
   */
  struct arg_info {
    int index;
    int mem_id; // mem_id is -1 if it's not the OpenCL memory object
    void* src; 
    size_t size;

    // arg_info::arg_info(void* src, size_t size, ArgType type)
    //   : arg_src(src), arg_size(size), arg_type(type)
    arg_info(int arg_index, int mid, void* arg_src=nullptr, size_t arg_size=0)
      : index(arg_index), mem_id(mid), src(arg_src), size(arg_size)
    {
      // TODO: error if mem_id is -1 but arg_src is null
    }
  };

  class request {
    private:
      ReqType req_type;

      /* for MEMORY request */
      uint32_t mem_num; 
      mem_info **mems; 

      /* for TRANSFER request */
      transfer_info *trans; 
      
      /* for EXECUTE request */
      const char* kernel_name;
      size_t name_size;
      uint32_t arg_num; 
      arg_info *args; 

      /* for SYNC request */
      // uint32_t event_num;
      // event_info events;

    public:
      // delegating constructor (and for SYNC request)
      request(ReqType type) 
        : req_type(type), mem_num(0), mems(NULL), 
          kernel_name(NULL), name_size(0), arg_num(0), args(NULL)// , event_num(0)
      {}

      // TODO: merge these constructors into one  
      /* for MEMORY request */
      request(ReqType type, uint32_t num, void** ptr)
        : request(type)
      {
        // TODO: error if type != MEMORY
        mem_num = num;
        mems    = (mem_info**) ptr;
      }

      /* for TRANSFER request */
      request(ReqType type, void* ptr)
        : request(type)
      {
        // TODO: error if type != TRANSFER
        trans   = (transfer_info*) ptr;
      }

      /* for EXEC request */
      request(ReqType type, const char* name, size_t size, uint32_t num, void* ptr)
        : request(type)
      {
        // TODO: error if type != EXEC
        kernel_name = name;
        name_size = size;
        arg_num = num;
        args    = (arg_info*) ptr;
      }

      ReqType get_request_type(void) {
        return req_type;
      }

      const char* get_kernel_name(size_t& size) {
        size = name_size;
        return kernel_name;
      }

      // TODO: merge the following functions into get_metadata() ?
      void* get_meminfo_array(int& num) {
        if(mem_num == 0)
          return NULL;

        num = mem_num;
        return (void *)mems;
      }

      void* get_transferinfo() {
        return (void *)trans;
      }

      void* get_arginfo_array(int& num) {
        if(arg_num == 0)
          return NULL;

        num = arg_num;
        return (void *)args;
      }
  };

  class response {
    private:
      ReqType res_type;

    public:
      response(ReqType type)
        : res_type(type)
      {}

      ReqType get_response_type(void) {
        return res_type;
      }
  };
}

#endif // __FUNKY_CMD_HPP__

