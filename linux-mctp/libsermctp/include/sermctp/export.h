#pragma once
#if defined(__GNUC__) || defined(__clang__)
#  define SERMCTP_API __attribute__((visibility("default")))
#else
#  define SERMCTP_API
#endif
