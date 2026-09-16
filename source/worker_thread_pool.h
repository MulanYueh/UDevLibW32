#ifndef WORKER_THREAD_POOL_H_INCLUDED
#define WORKER_THREAD_POOL_H_INCLUDED

/* Use the private fixed-width typedefs for native MSVC/WDK builds.  Hosted
 * GCC/Clang already provide stdint.h; mixing the two headers changes the
 * int_fast16_t typedef and is rejected by C++ compilers. */
#if defined(WORKER_THREAD_POOL_USE_STDINT2) || \
    (defined(_MSC_VER) && !defined(__clang__) && !defined(__GNUC__))
#  if defined(__GNUC__) || defined(__clang__)
#    pragma GCC diagnostic push
#    pragma GCC diagnostic ignored "-Wunknown-pragmas"
#  endif
#  include "stdint2.h"
#  if defined(__GNUC__) || defined(__clang__)
#    pragma GCC diagnostic pop
#  endif
#else
#  include <stdint.h>
#endif

#if defined(_KERNEL_MODE)
#  if defined(_MSC_VER)
#    include <ntifs.h>
#    define WORKER_THREAD_POOL_HAS_PLATFORM_HEADERS 1
#  elif defined(__has_include)
#    if __has_include(<ntifs.h>)
#      include <ntifs.h>
#      define WORKER_THREAD_POOL_HAS_PLATFORM_HEADERS 1
#    endif
#  endif
#else
#  if defined(_MSC_VER)
#    include <windows.h>
#    define WORKER_THREAD_POOL_HAS_PLATFORM_HEADERS 1
#  elif defined(__has_include)
#    if __has_include(<windows.h>)
#      include <windows.h>
#      define WORKER_THREAD_POOL_HAS_PLATFORM_HEADERS 1
#    endif
#  endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*worker_thread_pool_task_fn)(void *context);
typedef struct worker_thread_pool worker_thread_pool;

#define WORKER_THREAD_POOL_WAIT_INFINITE ((uint32_t)0xffffffffU)

#define WORKER_THREAD_POOL_SUCCESS           ((int32_t)0)
#define WORKER_THREAD_POOL_INVALID_PARAMETER ((int32_t)-1)
#define WORKER_THREAD_POOL_NO_MEMORY         ((int32_t)-2)
#define WORKER_THREAD_POOL_QUEUE_FULL        ((int32_t)-3)
#define WORKER_THREAD_POOL_STOPPED           ((int32_t)-4)
#define WORKER_THREAD_POOL_TIMEOUT           ((int32_t)0x00000102L)

/* Creates and starts worker_count threads. Both counts must be non-zero. */
worker_thread_pool *worker_thread_pool_create(uint32_t worker_count,
                                              uint32_t queue_limit);

/*
 * Stops all workers after completing already accepted tasks. New submissions
 * are rejected once stopping starts. This function is safe for concurrent
 * callers, but must not be called by a pool worker.
 */
int32_t worker_thread_pool_stop(worker_thread_pool *pool);
/*
 * Stops the pool if needed, then releases all pool resources. The caller must
 * ensure that no other thread accesses the pool after this call begins and
 * must not call it from a pool worker.
 */
void worker_thread_pool_destroy(worker_thread_pool *pool);

/*
 * Submits one task.  The task and finish callback execute on a worker.  If
 * wait_for_completion is non-zero, timeout_ms applies to the completion wait;
 * WORKER_THREAD_POOL_WAIT_INFINITE waits without a deadline.
 * In kernel mode, create/submit/stop/destroy must be called at PASSIVE_LEVEL.
 * Callbacks must return normally and must not use the pool after destroy.
 */
int32_t worker_thread_pool_submit(worker_thread_pool *pool,
                                  worker_thread_pool_task_fn task,
                                  worker_thread_pool_task_fn finish,
                                  void *context,
                                  int wait_for_completion,
                                  uint32_t timeout_ms);

uint32_t worker_thread_pool_worker_count(const worker_thread_pool *pool);
int worker_thread_pool_is_worker(const worker_thread_pool *pool,
                                 ULONG_PTR thread_id);

#ifdef __cplusplus
}
#endif

#endif /* WORKER_THREAD_POOL_H_INCLUDED */
