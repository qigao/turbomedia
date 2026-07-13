#ifndef TURBO_EXPORT_H
#define TURBO_EXPORT_H

#define CXX_EXTERN_C extern "C"

#if defined(_MSC_VER) || defined(__MINGW32__) || defined(__MINGW64__)
  #define CXX_DLL_EXPORT __declspec(dllexport)
  #define CXX_DLL_LOCAL
#elif defined(__GNUC__) && __GNUC__ >= 4
  #define CXX_DLL_EXPORT __attribute__((visibility("default")))
  #define CXX_DLL_LOCAL __attribute__((visibility("hidden")))
#else
  #define CXX_DLL_EXPORT
  #define CXX_DLL_LOCAL
#endif

#ifndef CXX_API
  #if defined(SHARED_CXX)
    #define CXX_API CXX_DLL_EXPORT
  #else
    #define CXX_API
  #endif
#endif

#ifdef __cplusplus
  #define CXX_C_API CXX_EXTERN_C CXX_API
#else
  #define CXX_C_API CXX_API
#endif

#endif
