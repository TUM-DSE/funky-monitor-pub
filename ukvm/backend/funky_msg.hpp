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
  enum TransType {MIGRATE, WRITE, READ};
  enum SyncType {FINISH, PROFILE, WAITEVENTS};

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
    TransType trans;  /* transfer type */
    int* ids;         /* ids of memory to be transferred */
    size_t num;       /* number of memory to be transferred */
    uint64_t flags;   /* migrate: host to device, or device to host 
                         write/read: blocking or non-blocking */
    size_t offset;    /* for write/read */
    size_t size;      /* for write/read */
    const void* ptr;  /* for write/read */

    /* MIGRATE transfers */
    transfer_info(TransType trans_type, int* mem_ids, size_t mem_num, uint64_t trans_flags)
      : trans(trans_type), ids(mem_ids), num(mem_num), flags(trans_flags)
    {}

    /* READ/WRITE transfers */
    transfer_info(TransType trans_type, int* mem_ids, size_t mem_num, uint64_t trans_flags, size_t obj_offset, size_t host_size, const void* host_ptr)
      : trans(trans_type), ids(mem_ids), num(mem_num), flags(trans_flags), offset(obj_offset), size(host_size), ptr(host_ptr)
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

  struct event_info {
    int id; 
    uint32_t wait_event_num;
    int* wait_event_ids;
    uint32_t param_name;
    void* param; // size is fixed (64 bits, cl_ulong)

    event_info(int event_id, uint32_t we_num, int* we_ids)
      : id(event_id), wait_event_num(we_num), wait_event_ids(we_ids)
    {}

    event_info(int event_id, uint32_t we_num, int* we_ids, uint32_t pname, void* pvalue)
      : event_info(event_id, we_num, we_ids)
    {
      param_name = pname;
      param = pvalue;
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

      /* for TRANSFER, EXECUTE, SYNC request */
      uint32_t cmdq_id;

      /* for SYNC request */
      SyncType sync_type;
      event_info *evinfo;

    public:
      // delegating constructor (and for SYNC request)
      request(ReqType type) 
        : req_type(type), mem_num(0), mems(NULL), 
          kernel_name(NULL), name_size(0), arg_num(0), args(NULL), evinfo(NULL)
      {}

      /* 
       * TODO: use overriding to prepare for different message classes 
       * create base 'request' class derived by the others (memory_req, transfer_req, ...)
       */
      /* for MEMORY request */
      request(ReqType type, uint32_t num, void** ptr)
        : request(type)
      {
        // TODO: error if type != MEMORY
        mem_num = num;
        mems    = (mem_info**) ptr;
      }

      /* for TRANSFER request */
      request(ReqType type, void* ptr, uint32_t cl_cmdq_id, event_info* event_info)
        : request(type)
      {
        // TODO: error if type != TRANSFER
        trans   = (transfer_info*) ptr;
        cmdq_id = cl_cmdq_id;
        evinfo = event_info;
      }

      /* for EXEC request */
      request(ReqType type, const char* name, size_t size, uint32_t num, void* ptr, uint32_t cl_cmdq_id, event_info* event_info)
        : request(type)
      {
        // TODO: error if type != EXEC
        kernel_name = name;
        name_size = size;
        arg_num = num;
        args    = (arg_info*) ptr;
        cmdq_id = cl_cmdq_id;
        evinfo = event_info;
      }

      /* for SYNC request */
      request(ReqType type, SyncType s_type, uint32_t cl_cmdq_id, event_info* event_info)
        : request(type)
      {
        // TODO: error if type != SYNC
        cmdq_id = cl_cmdq_id;
        evinfo = event_info;
        sync_type = s_type;
      }

      ReqType get_request_type(void) {
        return req_type;
      }

      SyncType get_sync_type(void) {
        return sync_type;
      }

      int get_cmdq_id(void) {
        // return (cmdq_id >= 0)? (int) cmdq_id: -1;
        return cmdq_id;
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

      void* get_eventinfo(void) {
        // return (cmdq_id >= 0)? (int) cmdq_id: -1;
        return (void *)evinfo;
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

