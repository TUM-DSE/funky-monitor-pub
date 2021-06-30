#ifndef __BUFFER_HPP__
#define __BUFFER_HPP__

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <assert.h>

/*
 * Usage:
 * lock-free unidirectional ring buffer
 *
 *  base class: RingBuffer
 *    support two constructors
 *    (1) master   (for unikernel): newly allocate memory space for the buffer by malloc
 *    (2) follower (for monitor)  : refer to the master's buffer address
 *
 *  derived classes: Writer and Reader
 *  Writer
 *    - method: push()
 *    - tail is updated only when push() is called (head is read-only)
 *
 *  Reader
 *    - method: pop()
 *    - head is updated only when pop() is called (tail is read-only)
 *
 */

namespace buffer {
  struct Header {
    // capacity: maximum number of elements in the buffer
    //    Note that buffer size is (capacity+1) elements and at least one space is always empty
    // head: pointing to a writable (blank) space
    // tail: pointing to a readable element unless tail == head (buffer is empty)
    //
    // There is always at least one empty space in the buffer
    // if  head    == tail: buffer is empty
    // if (head+1) == tail: buffer is full
    //
    size_t capacity;
    int head;
    int tail;
  };

  template <typename T>
  class RingBuffer {
    // protect:
    private: 
      bool is_owner; // ownership
      void* base; // pointer to the base addr of header/buffer
      size_t mm_size; // allocated memory size 
      T* base_elem;  // pointer to the head of buffer
      size_t num; // num of elements in this buffer
      // TDO: protect??
      Header* header;

      void init_pointer(void* base_addr)
      {
        header = reinterpret_cast<Header *>(base_addr);
        assert(header != NULL);
        base_elem = reinterpret_cast<T *>((char *)base_addr + sizeof(Header));
        assert(base_elem != NULL);
      }

      void init_header(size_t cap)
      {
        assert(header != NULL);
        header->capacity = cap;
        header->head = 0;
        header->tail = 0;
      }

    public:
      // called by guest (unikernel): the buffer is initialized in guest address space
      RingBuffer<T>(size_t cap) : is_owner(true)
      {
        // allocate memory space for (MAX+1) elements 
        cap++;
        mm_size = sizeof(Header) + (sizeof(T) * cap); 
        base = std::malloc(mm_size);

        init_pointer(base);
        init_header(cap);
      }

      // called by host (monitor): base address of the buffer is passed via hypercall
      RingBuffer<T>(void* base_addr) : is_owner(false)
      {
        base = base_addr;
        init_pointer(base);
        mm_size = sizeof(Header) + (sizeof(T) * (header->capacity)); 
      }

      virtual ~RingBuffer()
      {
        assert(base != NULL);

        if(is_owner)
          std::free(base);
      }

      // TODO: count num of data?
      size_t size(){
        assert(header != NULL);

        size_t size;
        if (header->head < header->tail)
          size = (header->capacity - header->tail) + header->head;
        else
          size = header->head - header->tail;

        return size;
      }

      bool empty(){
        assert(header != NULL);

        if (header->head == header->tail)
          return true;
        else 
          return false;
      }

      bool full(){
        assert(header != NULL);

        int next_head = (header->head+1) % header->capacity;
        if ( next_head == header->tail)
          return true;
        else 
          return false;
      }

      // write data to head (data copy is done by Writer)
      T* push_to_head()
      {
        auto head_elem = base_elem + header->head;
        header->head = (header->head+1) % header->capacity;
        return head_elem;
      }

      // read data from tail (data copy is done by keader)
      T* pop_from_tail()
      {
        auto tail_elem = base_elem + header->tail;
        header->tail = (header->tail+1) % header->capacity;
        return tail_elem;
      }

      void* get_baseaddr()
      {
        if(is_owner)
          return base;
        else
          return NULL;
      }

      size_t& get_mmsize()
      {
        return mm_size;
      }
  };

  template <typename T>
  class Reader : public RingBuffer<T> {
    public:
      Reader(size_t cap) : RingBuffer<T>(cap) {}
      Reader(void* addr) : RingBuffer<T>(addr) {}

      // If the buffer is empty, return NULL
      T* pop()
      {
        if(RingBuffer<T>::empty())
          return NULL;

        auto head_addr = RingBuffer<T>::pop_from_tail();
        return head_addr;
      }
  };

  template <typename T>
  class Writer : public RingBuffer<T> {
    public:
      Writer(size_t cap) : RingBuffer<T>(cap) {}
      Writer(void* addr) : RingBuffer<T>(addr) {}

      // If the buffer is full, return false
      bool push(const T& elem)
      {
        if(RingBuffer<T>::full())
          return false;

        auto head_addr = RingBuffer<T>::push_to_head();
        std::memcpy(head_addr, &elem, sizeof(T));
        return true;
      }
  };
}

#endif // __BUFFER_HPP__



