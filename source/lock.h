/*
 * Generic recursive/shared lock.
 *
 * User mode uses a recursive CRITICAL_SECTION.  Kernel mode uses ERESOURCE,
 * whose exclusive acquisition is recursive for the owning thread and whose
 * release routine handles both shared and exclusive acquisitions.  LPC has a
 * separate, non-recursive lock because its critical sections are deliberately
 * kept free of callbacks and external calls.
 */
#ifndef MEMORY_POOL_LOCK_H_INCLUDED
#define MEMORY_POOL_LOCK_H_INCLUDED

#if !defined(__OXDRV_KSTDINT_H__) && !defined(_STDINT_H) && \
    !defined(_STDINT_H_) && !defined(_GCC_WRAP_STDINT_H)
#  if defined(LOCK_USE_STDINT2) || \
      (defined(_MSC_VER) && !defined(__clang__) && !defined(__GNUC__))
#    if defined(__GNUC__) || defined(__clang__)
#      pragma GCC diagnostic push
#      pragma GCC diagnostic ignored "-Wunknown-pragmas"
#    endif
#    include "stdint2.h"
#    if defined(__GNUC__) || defined(__clang__)
#      pragma GCC diagnostic pop
#    endif
#  else
#    include <stdint.h>
#  endif
#endif

#if defined(_KERNEL_MODE)
#  if defined(_MSC_VER)
#    include <ntifs.h>
#    define MEMORY_POOL_LOCK_USE_ERESOURCE 1
#  elif defined(__has_include)
#    if __has_include(<ntifs.h>)
#      include <ntifs.h>
#      define MEMORY_POOL_LOCK_USE_ERESOURCE 1
#    endif
#  endif
#elif defined(_WIN32)
#  if defined(_MSC_VER)
#    include <windows.h>
#    define MEMORY_POOL_LOCK_USE_CRITICAL_SECTION 1
#  elif defined(__has_include)
#    if __has_include(<windows.h>)
#      include <windows.h>
#      define MEMORY_POOL_LOCK_USE_CRITICAL_SECTION 1
#    endif
#  endif
#endif

#if defined(POOL_USE_CUSTOM_LOCK)
/* pool.c historically opts into its scalar lock protocol explicitly. */
typedef volatile uint32_t LOCK;
#  define MEMORY_POOL_LOCK_USE_LEGACY_SCALAR 1
#  define LOCK_FREE 0x00000000U
#  define LOCK_EXCLUSIVE 0x80000000U
#elif defined(MEMORY_POOL_LOCK_USE_ERESOURCE)
typedef struct LOCK {
    ERESOURCE native;
    uint8_t initialized;
} LOCK;
#elif defined(MEMORY_POOL_LOCK_USE_CRITICAL_SECTION)
typedef struct LOCK {
    CRITICAL_SECTION native;
    uint8_t initialized;
} LOCK;
#else
typedef struct LOCK {
    volatile uint32_t native;
    uint8_t initialized;
} LOCK;
#endif

/* Compatibility alias used by older callers. */
typedef LOCK LPC_LOCK;

#ifdef __cplusplus
extern "C" {
#endif

int Lock_Init(LOCK *lock);
void Lock_Destroy(LOCK *lock);

/* The variadic tail accepts the historical, unused LockName argument. */
void Lock_Exclusive(LOCK *lock, ...);
void Lock_Share(LOCK *lock, ...);
void Lock_Unlock(LOCK *lock, ...);

#ifdef __cplusplus
}
#endif

#endif /* MEMORY_POOL_LOCK_H_INCLUDED */
