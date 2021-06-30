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
  enum ReqType {EXEC, TRANSFER, SYNC};
  enum MemType {BUFFER, PIPE, IMAGE};

  /*
   * mem_info is used to initialize FPGA memory by input/output data.
   * data flow/depenency of tasks is defined by arg_info in the subsequent EXEC resuests.
   *
   * */
  struct mem_info {
    int mem_id;
    MemType mem_type;
    void* mem_src;
    size_t mem_size;

    mem_info(int id, MemType type, void* src, size_t size)
      : mem_id(id), mem_type(type), mem_src(src), mem_size(size)
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
    void* src; 
    size_t size;
    int index;
    int mem_id; // mem_id is -1 if it's not the OpenCL memory object

    // arg_info::arg_info(void* src, size_t size, ArgType type)
    //   : arg_src(src), arg_size(size), arg_type(type)
    arg_info(void* arg_src, size_t arg_size, int arg_index, int id)
      : src(arg_src), size(arg_size), index(arg_index), mem_id(id)
    {}
  };

  class request {
    private:
      ReqType req_type;

      /* for TRANSFER request */
      uint32_t mem_num; 
      mem_info **mems; 
      
      /* for EXEC request */
      /* The pointer specifies a guest memory address */
      const char* kernel_name;
      uint32_t arg_num; 
      arg_info **args; 

      /* for SYNC request */
      uint32_t event_num;
      // event_info events;

    public:
      // delegating constructor
      request(ReqType type) 
        : req_type(type), mem_num(0), mems(NULL), 
          kernel_name(NULL), arg_num(0), args(NULL), event_num(0)
      {}

      /* for TRANSFER request */
      request(ReqType type, uint32_t num, void** ptr)
        : request(type)
      {
        // TODO: error if type != TRANSFER
        mem_num = num;
        mems    = (mem_info**) ptr;
      }

      /* for EXEC request */
      request(ReqType type, const char* name, uint32_t num, void** ptr)
        : request(type)
      {
        // TODO: error if type != EXEC
        kernel_name = name;
        arg_num = num;
        args    = (arg_info**) ptr;
      }

      ReqType get_request_type(void) {
        return req_type;
      }

      const char* get_kernel_name(void) {
        return kernel_name;
      }

      /* 
       * TODO: use iterator
       * TODO: address translation from guest to ukvm
       */
      arg_info* get_arg_info(uint32_t num){
        if(num >= arg_num)
          return NULL;

        return args[num];
      }

      /* 
       * TODO: use iterator
       * TODO: address translation from guest to ukvm
       */
      mem_info* get_mem_info(uint32_t num){
        if(num >= mem_num)
          return NULL;

        return mems[num];
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

