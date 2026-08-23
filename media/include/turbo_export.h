#ifndef TURBO_EXPORT_H
#define TURBO_EXPORT_H

#if defined(_MSC_VER) || defined(__MINGW32__) || defined(__MINGW64__)
  #define TURBO_MEDIA_DLL_EXPORT __declspec(dllexport)
  #define TURBO_MEDIA_DLL_LOCAL
#elif defined(__GNUC__) && __GNUC__ >= 4
  #define TURBO_MEDIA_DLL_EXPORT __attribute__((visibility("default")))
  #define TURBO_MEDIA_DLL_LOCAL __attribute__((visibility("hidden")))
#else
  #define TURBO_MEDIA_DLL_EXPORT
  #define TURBO_MEDIA_DLL_LOCAL
#endif

#ifndef TURBO_MEDIA_API
  #if defined(SHARED_CXX)
    #define TURBO_MEDIA_API TURBO_MEDIA_DLL_EXPORT
  #else
    #define TURBO_MEDIA_API
  #endif
#endif

#ifdef __cplusplus
  #define TURBO_MEDIA_C_API extern "C" TURBO_MEDIA_API
#else
  #define TURBO_MEDIA_C_API TURBO_MEDIA_API
#endif

#endif
