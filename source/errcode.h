#ifndef ERRCODE_H_INCLUDED
#define ERRCODE_H_INCLUDED

#include "stdint2.h"

/* ---------------------------------------------------------------------------
 * Return codes
 * ------------------------------------------------------------------------- */
#define ERR_PUSH_OK        1                     /* pushed, room remains     */
#define ERR_PUSH_FAIL      0                     /* failed (see below)       */

#ifdef __cplusplus
extern "C" {
#endif

 /**
  * @brief  Push a value into the next free 16-bit slot of a 64-bit error code.
  *
  * @param[in,out] errcode  Pointer to the caller-owned 64-bit error code
  *                         accumulator (one per component). Must not be NULL;
  *                         on success the newly recorded value is OR'd into
  *                         the next free slot.
  * @param[in]     value    16-bit payload to record, typically a line number
  *                         or a module-specific error code. Declared as
  *                         uint32_t so that values greater than 0xFFFE can be
  *                         detected instead of being silently truncated at
  *                         the call site. Valid range: 1..0xFFFE.
  *
  * @retval ERR_PUSH_OK   (1)  Value pushed successfully; free slots remain.
  * @retval ERR_PUSH_FAIL (0)  Failure. Possible causes:
  *                            - @p errcode is NULL;
  *                            - all ERR_SLOT_COUNT slots are already full
  *                              (existing data is not overwritten);
  *                            - @p value is 0 or greater than 0xFFFE,
  *                              in which case ERR_VALUE_OVERFLOW is written
  *                              into the current free slot.
  *
  * @note   The function performs no synchronization. If the same error code
  *         may be written from multiple threads, protect it externally or
  *         keep one accumulator per thread/context.
  *
  * @see    ErrCode_Decode
  */
int ErrCode_Pack(uint64_t* errcode, uint32_t value);

/**
 * @brief  Unpack a 64-bit error code into its four 16-bit slots.
 *
 * @param[in]  code  64-bit error code to decode, as produced by
 *                   ErrCode_Push.
 * @param[out] e1    Receives slot[0] (innermost / earliest failure).
 *                   May be NULL, in which case the slot is ignored.
 * @param[out] e2    Receives slot[1] (2nd level). May be NULL.
 * @param[out] e3    Receives slot[2] (3rd level). May be NULL.
 * @param[out] e4    Receives slot[3] (outermost / last call). May be NULL.
 *
 * @return  Nothing.
 *
 * @note   Decoding is a pure bit-extraction; no validation is performed.
 *         To determine the actual call depth, scan from @p e1 upward until
 *         the first ERR_VALUE_EMPTY slot is found.
 *
 * @see    ErrCode_Push
 */
void ErrCode_Unpack(uint64_t code, uint16_t* e1, uint16_t* e2, uint16_t* e3, uint16_t* e4);

#ifdef __cplusplus
}
#endif

#endif // ERRCODE_H_INCLUDED
