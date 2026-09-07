#include "lock.h"

#if !defined(MEMORY_POOL_LOCK_USE_ERESOURCE) && \
    !defined(MEMORY_POOL_LOCK_USE_CRITICAL_SECTION)
#  if defined(_MSC_VER)
#    include <intrin.h>
#  endif
#endif

int Lock_Init(LOCK *lock)
{
    if (!lock) {
        return 0;
    }
#if defined(MEMORY_POOL_LOCK_USE_LEGACY_SCALAR)
    *lock = LOCK_FREE;
#elif defined(MEMORY_POOL_LOCK_USE_ERESOURCE)
    if (!NT_SUCCESS(ExInitializeResourceLite(&lock->native))) {
        return 0;
    }
#elif defined(MEMORY_POOL_LOCK_USE_CRITICAL_SECTION)
    InitializeCriticalSection(&lock->native);
#else
    lock->native = 0;
#endif
#if !defined(MEMORY_POOL_LOCK_USE_LEGACY_SCALAR)
    lock->initialized = 1;
#endif
    return 1;
}

void Lock_Destroy(LOCK *lock)
{
#if defined(MEMORY_POOL_LOCK_USE_LEGACY_SCALAR)
    (void)lock;
    return;
#else
    if (!lock || !lock->initialized) {
        return;
    }
#if defined(MEMORY_POOL_LOCK_USE_ERESOURCE)
    ExDeleteResourceLite(&lock->native);
#elif defined(MEMORY_POOL_LOCK_USE_CRITICAL_SECTION)
    DeleteCriticalSection(&lock->native);
#else
    lock->native = 0;
#endif
    lock->initialized = 0;
#endif
}

void Lock_Exclusive(LOCK *lock, ...)
{
#if defined(MEMORY_POOL_LOCK_USE_LEGACY_SCALAR)
    uint32_t oldValue = 0u;
    if (!lock) {
        return;
    }
    for (;;) {
#  if defined(_MSC_VER)
        oldValue = (uint32_t)_InterlockedCompareExchange((volatile long *)lock,
                                                          (long)LOCK_EXCLUSIVE,
                                                          (long)LOCK_FREE);
#  elif defined(__GNUC__) || defined(__clang__)
        oldValue = __sync_val_compare_and_swap(lock, LOCK_FREE, LOCK_EXCLUSIVE);
#  else
        oldValue = *lock;
        if (oldValue == LOCK_FREE) {
            *lock = LOCK_EXCLUSIVE;
        }
#  endif
        if (oldValue == LOCK_FREE) {
            return;
        }
#  if defined(_MSC_VER)
        YieldProcessor();
#  endif
    }
#else
    if (!lock || !lock->initialized) {
        return;
    }
#if defined(MEMORY_POOL_LOCK_USE_ERESOURCE)
    (void)ExAcquireResourceExclusiveLite(&lock->native, TRUE);
#elif defined(MEMORY_POOL_LOCK_USE_CRITICAL_SECTION)
    EnterCriticalSection(&lock->native);
#else
#  if defined(_MSC_VER)
    while (_InterlockedCompareExchange((volatile long *)&lock->native, 1, 0) != 0) {
        YieldProcessor();
    }
#  elif defined(__GNUC__) || defined(__clang__)
    while (__sync_lock_test_and_set(&lock->native, 1U) != 0U) {
    }
#  else
    while (lock->native) {
    }
    lock->native = 1U;
#  endif
#endif
#endif
}

void Lock_Share(LOCK *lock, ...)
{
#if defined(MEMORY_POOL_LOCK_USE_LEGACY_SCALAR)
    /* The legacy pool path only requires exclusive ownership. */
    Lock_Exclusive(lock);
#else
    if (!lock || !lock->initialized) {
        return;
    }
#if defined(MEMORY_POOL_LOCK_USE_ERESOURCE)
    (void)ExAcquireResourceSharedLite(&lock->native, TRUE);
#elif defined(MEMORY_POOL_LOCK_USE_CRITICAL_SECTION)
    /* CRITICAL_SECTION has no shared mode; serialize readers as well. */
    EnterCriticalSection(&lock->native);
#else
    /* The fallback is intentionally exclusive. */
#  if defined(_MSC_VER)
    while (_InterlockedCompareExchange((volatile long *)&lock->native, 1, 0) != 0) {
        YieldProcessor();
    }
#  elif defined(__GNUC__) || defined(__clang__)
    while (__sync_lock_test_and_set(&lock->native, 1U) != 0U) {
    }
#  else
    while (lock->native) {
    }
    lock->native = 1U;
#  endif
#endif
#endif
}

void Lock_Unlock(LOCK *lock, ...)
{
#if defined(MEMORY_POOL_LOCK_USE_LEGACY_SCALAR)
    if (!lock) {
        return;
    }
#  if defined(_MSC_VER)
    (void)_InterlockedExchange((volatile long *)lock, (long)LOCK_FREE);
#  elif defined(__GNUC__) || defined(__clang__)
    __sync_lock_release(lock);
#  else
    *lock = LOCK_FREE;
#  endif
#else
    if (!lock || !lock->initialized) {
        return;
    }
#if defined(MEMORY_POOL_LOCK_USE_ERESOURCE)
    ExReleaseResourceLite(&lock->native);
#elif defined(MEMORY_POOL_LOCK_USE_CRITICAL_SECTION)
    LeaveCriticalSection(&lock->native);
#else
#  if defined(_MSC_VER)
    _InterlockedExchange((volatile long *)&lock->native, 0);
#  elif defined(__GNUC__) || defined(__clang__)
    __sync_lock_release(&lock->native);
#  else
    lock->native = 0U;
#  endif
#endif
#endif
}
