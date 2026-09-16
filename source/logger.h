/*
 * Small, CRT-independent logger for Windows user mode and kernel mode.
 *
 * The formatter intentionally accepts a documented, integer/string subset of
 * printf.  This keeps the same call site usable in a driver, where floating
 * point formatting and the user-mode CRT are not available.
 */
#ifndef _LOGGER_H_INCLUDED
#define _LOGGER_H_INCLUDED

#if defined(_KERNEL_MODE)
#include <ntddk.h>
#else
#include <windows.h>
#endif

#include <stdarg.h>

#ifndef LOGGER_CALL
#if defined(_MSC_VER)
#define LOGGER_CALL __stdcall
#elif defined(__i386__) && defined(_WIN32) && \
      (defined(__GNUC__) || defined(__clang__))
#define LOGGER_CALL __attribute__((__stdcall__))
#else
#define LOGGER_CALL
#endif
#endif

/* The buffer is stack allocated by Logger_PrintEx. */
#ifndef LOGGER_MAX_MESSAGE_CHARS
#define LOGGER_MAX_MESSAGE_CHARS 1024U
#endif

typedef enum _DBGLEVEL {
    LEVEL_FATAL = 0x1,
    LEVEL_ERROR = 0x2,
    LEVEL_WARNING = 0x3,
    LEVEL_IMPORTANT = 0x4,
    LEVEL_NOTIFY = 0x5,
    LEVEL_INFO = 0x6,
    LEVEL_DEBUG = 0x7,
    LEVEL_MAX = 0x8
} DBGLEVEL;

/* Legacy callback ABI.  The text is valid only during the callback. */
typedef void (LOGGER_CALL *typedef_LoggerPrintImpl)(LPCWSTR wszLogContent);

/* New callback ABI.  Length excludes the terminating null character. */
typedef void (LOGGER_CALL *LOGGER_OUTPUT_FN)(
    void *context, LPCWSTR text, ULONG length);

/*
 * LOGOPT remains exactly the historical two-field structure for source and
 * binary compatibility.  New code that needs callback context can use
 * LOGGER_CONFIG and Logger_CreateEx.
 */
typedef struct _LOGOPT {
    DBGLEVEL OutputLevel;
    typedef_LoggerPrintImpl pfnLoggerPrintImpl;
} LOGOPT, *PLOGOPT;

typedef struct _LOGGER_CONFIG {
    DBGLEVEL OutputLevel;
    LOGGER_OUTPUT_FN pfnOutput;
    void *Context;
    /* Zero selects LOGGER_MAX_MESSAGE_CHARS; nonzero must not exceed it. */
    ULONG MaxMessageChars;
} LOGGER_CONFIG, *PLOGGER_CONFIG;

typedef enum _LOGGER_RESULT {
    LOGGER_RESULT_WRITTEN = 0,
    LOGGER_RESULT_FILTERED = 1,
    LOGGER_RESULT_TRUNCATED = 2,
    LOGGER_RESULT_NO_OUTPUT = 3,
    LOGGER_RESULT_INVALID_PARAMETER = -1,
    LOGGER_RESULT_NO_MEMORY = -2,
    LOGGER_RESULT_INVALID_LEVEL = -3,
    LOGGER_RESULT_FORMAT_ERROR = -4,
    LOGGER_RESULT_IRQL_NOT_SUPPORTED = -5
} LOGGER_RESULT;

/*
 * Supported format syntax is intentionally finite: %c/%C, %s/%S, %d/%i,
 * %u/%o/%x/%X/%b, %p and %%.  Width/precision, *, #, 0, -, + and space are
 * supported, as are h, hh, l, ll, z, j, t, I32, I64 and w modifiers.  Since
 * the format itself is wide, %s is wide text and %S/%hs is byte text; %ws is
 * an explicit wide-text spelling.  Kernel builds additionally accept %wZ for
 * PUNICODE_STRING.  Floating point and %n are rejected in every build.
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _LOGGER_SESSION *PLOGGER_SESSION;

/* Logger_Create preserves the historical callback ABI. */
PLOGGER_SESSION Logger_Create(LOGOPT option);
PLOGGER_SESSION Logger_CreateEx(const LOGGER_CONFIG *config);
/* Destroy only after all callers have stopped using the session. */
void Logger_Destroy(PLOGGER_SESSION pSession);

/* Invalid levels are ignored by this legacy void API. */
void Logger_SetLevel(PLOGGER_SESSION pSession, DBGLEVEL level);
DBGLEVEL Logger_GetLevel(PLOGGER_SESSION pSession);
int Logger_IsEnabled(PLOGGER_SESSION pSession, DBGLEVEL level);

/*
 * The V/Ex entry points return a useful result.  Logger_Print is retained as
 * a source-compatible void wrapper for existing callers.
 * The callback runs synchronously and receives a temporary null-terminated
 * buffer.  In kernel mode these functions return IRQL_NOT_SUPPORTED above
 * APC_LEVEL.
 */
LOGGER_RESULT Logger_PrintV(PLOGGER_SESSION pSession, LPCSTR func,
                            ULONG line, DBGLEVEL level, LPCWSTR format,
                            va_list args);
LOGGER_RESULT Logger_PrintEx(PLOGGER_SESSION pSession, LPCSTR func,
                             ULONG line, DBGLEVEL level, LPCWSTR format, ...);
void Logger_Print(PLOGGER_SESSION pSession, LPCSTR func, ULONG line,
                  DBGLEVEL level, LPCWSTR format, ...);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* _LOGGER_H_INCLUDED */
