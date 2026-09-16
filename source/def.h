#ifndef DEF_H_INCLUDED
#define DEF_H_INCLUDED

// Pool Definitions

#ifndef LPC_POOL_TAG
#define LPC_POOL_TAG 0x4C6C7063UL /* "cplL" in little-endian pool displays */
#endif

#ifndef ALPC_PORT_POOL_TAG
#define ALPC_PORT_POOL_TAG 0x41706C63UL /* "cplA" */
#endif 

#ifndef CIRCLE_QUEUE_POOL_TAG
#define CIRCLE_QUEUE_POOL_TAG 0x43525155UL /* "UQRC" */
#endif

#ifndef MAP_POOL_TAG
#define MAP_POOL_TAG 0x4170614DUL /* "MapA" */
#endif

#ifndef MEMGUARD_METADATA_TAG
#define MEMGUARD_METADATA_TAG 0x444D474DUL /* "MGMD" */
#endif

#ifndef WTP_POOL_TAG
#define WTP_POOL_TAG 0x50505457UL /* "WTPP" */
#endif

/* Tags used by the remaining framework components.  Keeping all defaults in
 * one header makes pool accounting and debugger leak reports consistent. */
#ifndef MEMPOOL_POOL_TAG
#define MEMPOOL_POOL_TAG 0x4C50534DUL /* "MSPL" */
#endif

#ifndef LOGGER_POOL_TAG
#define LOGGER_POOL_TAG 0x3072674CUL /* "Lgr0" */
#endif

 // NT definitions

#ifndef KeYieldProcessor
#define KeYieldProcessor YieldProcessor
#endif

#endif // DEF_H_INCLUDED
