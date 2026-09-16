#ifndef API_RESOLVER_H
#define API_RESOLVER_H

/*
 * Public PE export resolver interface.
 *
 * The header intentionally does not include Windows SDK or WDK headers.  It
 * can therefore be included by both C and C++ translation units without
 * pulling in conflicting user/kernel definitions.  The implementation uses
 * the native pointer width selected by the target Windows architecture.
 * Define _KERNEL_MODE when compiling the implementation for a driver; omit
 * it for a normal user-mode build.
 *
 * api_resolver.c consumes the team's freestanding CRT directly, so make
 * libc.h available on its include path and link the matching libc object
 * (compile it with LIBC_KERNEL_MODE/_KERNEL_MODE for a driver).  A kernel
 * build must also place the linked CRT code in nonpaged memory when resolving
 * at elevated IRQL; loader-list/PEB paths themselves are intended for
 * PASSIVE_LEVEL.
 */
#if defined(_WIN64) || defined(__x86_64__) || defined(__amd64__) || \
	defined(__aarch64__) || defined(_M_X64) || defined(_M_ARM64) || \
	defined(_AMD64_) || defined(_ARM64_)
typedef unsigned long long API_RESOLVER_ADDRESS;
#else
typedef unsigned long API_RESOLVER_ADDRESS;
#endif

#ifndef API_RESOLVER_API
#define API_RESOLVER_API
#endif

#ifndef API_RESOLVER_CALL
#define API_RESOLVER_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Resolve FunctionName from an already mapped PE image (headers and sections
 * laid out at their RVAs; this is not a raw on-disk file buffer).
 * ServiceTableBase and any forwarded target must be accessible in the current address
 * space; a kernel caller resolving a user image must first attach to its
 * process at PASSIVE_LEVEL.
 *
 * FunctionName accepts a regular export name (for example "RtlCopyMemory")
 * or an ordinal string (for example "#123").  A zero return value means the
 * image is invalid, the export is absent, or a forwarder could not be
 * resolved.  The returned address is borrowed from the mapped image (or from
 * an already loaded forwarder target); callers must keep that image/module
 * loaded.
 */
API_RESOLVER_API API_RESOLVER_ADDRESS API_RESOLVER_CALL
ApiResolver_GetFuncAddress(void *Base, const char *FunctionName);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* API_RESOLVER_H */
