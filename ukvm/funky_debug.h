#ifndef __DEBUG_H
#define __DEBUG_H

// for debug
#include <cstdio>
#include <iostream>

// #define DEBUG

#ifdef DEBUG
#define DEBUG_PRINT(fmt, ...) \
  printf("[DEBUG-UKVM] %s(): ", __FUNCTION__); \
  printf(fmt, ##__VA_ARGS__); \
  printf("\n");
  // printf("%s:%d: in %s(): " #msg "\n", __FILE__, __LINE__, __FUNCTION__);
#define DEBUG_STREAM( msg ) \
  std::cout << "[DEBUG-UKVM] " << __FUNCTION__ << "(): " << msg << std::endl
#else
#define DEBUG_PRINT(fmt, ...)                                                                   
#define DEBUG_STREAM(msg)
#endif

#endif // __DEBUG_H
