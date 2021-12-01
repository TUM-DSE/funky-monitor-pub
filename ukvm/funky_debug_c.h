#ifndef __DEBUG_C_H
#define __DEBUG_C_H

// for debug
#include <stdio.h>

#define DEBUG_C

#ifdef DEBUG_C
#define DEBUG_PRINT_C(fmt, ...) \
  printf("[DEBUG-UKVM] %s(): ", __FUNCTION__); \
  printf(fmt, ##__VA_ARGS__); \
  printf("\n");
  // printf("%s:%d: in %s(): " #msg "\n", __FILE__, __LINE__, __FUNCTION__);
#else
#define DEBUG_PRINT_C(fmt, ...)                                                                   
#endif

#endif // __DEBUG_C_H

