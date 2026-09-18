#include "errcode.h"

#include <stddef.h>

/* ---------------------------------------------------------------------------
 * Slot configuration
 * ------------------------------------------------------------------------- */
#define ERR_SLOT_BITS      16U                   /* bits per slot            */
#define ERR_SLOT_COUNT     4U                    /* number of slots          */
#define ERR_SLOT_MASK      UINT64_C(0xFFFF)      /* per-slot mask            */

#define ERR_VALUE_EMPTY    UINT32_C(0x0000)      /* empty-slot marker        */
#define ERR_VALUE_MAX      UINT32_C(0xFFFE)      /* max valid payload        */
#define ERR_VALUE_OVERFLOW UINT32_C(0xFFFF)      /* overflow marker          */

/* ---------------------------------------------------------------------------
 * ErrCode_Pack
 * ---------------------------------------------------------------------------
 * Push value into the next free 16-bit slot of *code.
 *
 *   =====================================================================
 *                          uint64_t Error Code Layout (64-bit)
 *   =====================================================================
 *   |  slot[3]    |  slot[2]    |  slot[1]    |  slot[0]    |
 *   | bits 63..48 | bits 47..32 | bits 31..16 | bits 15..0  |
 *   =====================================================================
 *         ^             ^             ^             ^
 *         |             |             |             |
 *    outermost      3rd level     2nd level    innermost (first record)
 *    (last call)                              (earliest failure point)
 *
 *   Fill direction:
 *
 *        1st call                 2nd call          3rd call          4th call
 *    +------------+      +------------+   +------------+   +------------+
 *    |  slot[0]   | ---> |  slot[1]   |-->|  slot[2]   |-->|  slot[3]   |
 *    +------------+      +------------+   +------------+   +------------+
 *      __LINE__            __LINE__         __LINE__         __LINE__
 *
 *   Value convention per slot:
 *     ERR_VALUE_EMPTY    (0x0000)         : slot is empty
 *     ERR_VALUE_MAX      (0x0001..0xFFFE) : valid payload
 *     ERR_VALUE_OVERFLOW (0xFFFF)         : overflow marker
 *
 * Parameters:
 *   code  : caller-owned 64-bit error code (one per component)
 *   value : 16-bit payload (line number / error code).
 *           Declared uint32_t so that values > 0xFFFE can be detected
 *           instead of being silently truncated at the call site.
 *
 * Returns:
 *   ERR_PUSH_OK   (1) : pushed successfully, free slots remain
 *   ERR_PUSH_FAIL (0) : failure
 *                       - code == NULL
 *                       - all ERR_SLOT_COUNT slots are full
 *                         (existing data is not overwritten)
 *                       - value == 0 or value > 0xFFFE; an overflow
 *                         marker is written into the current free slot
 * ------------------------------------------------------------------------- */
int ErrCode_Pack(uint64_t* code, uint32_t value)
{
    unsigned i = 0;

    if (code == NULL) 
    {
        return ERR_PUSH_FAIL;
    }

    for (i = 0U; i < ERR_SLOT_COUNT; ++i) 
    {
        unsigned shift = i * ERR_SLOT_BITS;
        uint64_t mask = ERR_SLOT_MASK << shift;

        if ((*code & mask) == 0U) 
        {
            /* Invalid payload: write overflow marker, report failure. */
            if (value == ERR_VALUE_EMPTY || value > ERR_VALUE_MAX) 
            {
                *code |= (uint64_t)ERR_VALUE_OVERFLOW << shift;
                return ERR_PUSH_FAIL;
            }

            /* Valid payload: write it, report success. */
            *code |= (uint64_t)value << shift;
            return ERR_PUSH_OK;
        }
    }

    /* All slots are full; do not overwrite existing data. */
    return ERR_PUSH_FAIL;
}

/* ---------------------------------------------------------------------------
 * ErrCode_Unpack
 * ---------------------------------------------------------------------------
 * Unpack a 64-bit error code into its four 16-bit slots.
 *
 *   e1 -> slot[0] (bits 15..0)  : innermost  (first call, earliest failure)
 *   e2 -> slot[1] (bits 31..16) : 2nd level
 *   e3 -> slot[2] (bits 47..32) : 3rd level
 *   e4 -> slot[3] (bits 63..48) : outermost (last call)
 *
 * A slot that is ERR_VALUE_EMPTY (0x0000) means "no record at that level".
 * A slot equal to ERR_VALUE_OVERFLOW (0xFFFF) means the value pushed at
 * that level was invalid (0 or > 0xFFFE).
 *
 * Any output pointer may be NULL; the corresponding slot is then ignored.
 * ------------------------------------------------------------------------- */
void ErrCode_Unpack(uint64_t code, uint16_t* e1, uint16_t* e2, uint16_t* e3, uint16_t* e4)
{
    if (e1 != NULL) 
    {
        *e1 = (uint16_t)((code >> (0U * ERR_SLOT_BITS)) & ERR_SLOT_MASK);
    }
    if (e2 != NULL) 
    {
        *e2 = (uint16_t)((code >> (1U * ERR_SLOT_BITS)) & ERR_SLOT_MASK);
    }
    if (e3 != NULL) 
    {
        *e3 = (uint16_t)((code >> (2U * ERR_SLOT_BITS)) & ERR_SLOT_MASK);
    }
    if (e4 != NULL) 
    {
        *e4 = (uint16_t)((code >> (3U * ERR_SLOT_BITS)) & ERR_SLOT_MASK);
    }
}
