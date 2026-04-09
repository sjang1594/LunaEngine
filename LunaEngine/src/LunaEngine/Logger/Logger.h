#pragma once

#include <cstdarg>
#include <cstdio>

// ---------------------------------------------------------------------------
// Lightweight printf-style logger.
//
// Usage:
//   LUNA_LOG_INFO("Adapter selected: %s", adapterName);
//   LUNA_LOG_ERROR("HRESULT 0x%08X in %s", hr, __FUNCTION__);
//   LUNA_LOG_WARN("Resize skipped — zero extent");
// ---------------------------------------------------------------------------

namespace Luna
{
inline void Log(const char *level, FILE *stream, const char *fmt, va_list args)
{
    fputs(level, stream);
    vfprintf(stream, fmt, args);
    fputc('\n', stream);
}
} // namespace Luna

#define LUNA_LOG_INFO(fmt, ...)                                         \
    do {                                                                \
        va_list _va; (void)_va;                                         \
        ::fprintf(stdout, "[INFO]  " fmt "\n", ##__VA_ARGS__);          \
    } while (0)

#define LUNA_LOG_WARN(fmt, ...)                                         \
    do {                                                                \
        ::fprintf(stdout, "[WARN]  " fmt "\n", ##__VA_ARGS__);          \
    } while (0)

#define LUNA_LOG_ERROR(fmt, ...)                                        \
    do {                                                                \
        ::fprintf(stderr, "[ERROR] " fmt "\n", ##__VA_ARGS__);          \
    } while (0)
