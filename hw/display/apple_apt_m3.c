/*
 * Apple APT M3 firmware.
 *
 * Copyright (c) 2026 Visual Ehrmanntraut (VisualEhrmanntraut).
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "hw/arm/iboot/patcher.h"
#include "hw/display/apple_apt_m3.h"
#include "qemu/bswap.h"
#include "qemu/cutils.h"

#define THUMB_MOVS_R0_1 (0x2001)

// `push {..., lr}` (T1) or `push.w {..., lr}` (T2).
static bool apple_apt_m3_is_push_lr(const uint8_t* insn)
{
    uint16_t hw1 = lduw_le_p(insn);

    return (hw1 & 0xFF00) == 0xB500 || (hw1 == 0xE92D && (lduw_le_p(insn + 2) & 0x4000) != 0);
}

/*
 * m3_message() names itself in its assertion and stores OutCmdQ entries with
 * `str.w rT, [rN, #outcmdq]`, and the ActiveQ writer loads the flags offset
 * with the last `movw` before `movs r0, #1; b m3_message`.
 */
bool apple_apt_m3_find_queues(const uint8_t* imem, size_t size, uint32_t* outcmdq, uint32_t* activeq_flags)
{
    static const char name[] = "m3_message";
    const uint8_t*    str    = qemu_memmem(imem, size, name, sizeof(name));
    uint32_t          entry  = 0;
    uint32_t          ref    = 0;
    uint32_t          target;
    uint32_t          pc;
    uint32_t          start;
    uint32_t          back;
    uint32_t          prev;
    uint16_t          imm;

    *outcmdq       = 0;
    *activeq_flags = 0;

    if (str == NULL) { return false; }

    for (pc = 0; pc + 4 <= size; pc += 2) {
        if (ck_patcher_thumb_movw(imem + pc, &imm) && imm == str - imem) {
            ref = pc;
            break;
        }
    }
    if (ref == 0) { return false; }

    for (pc = ref; pc != 0 && entry == 0; pc -= 2) {
        if (apple_apt_m3_is_push_lr(imem + pc - 2)) { entry = pc - 2; }
    }
    if (entry == 0) { return false; }

    for (pc = entry; pc + 4 <= size && pc < ref + 0x40; pc += 2) {
        if (ck_patcher_thumb_str_imm12(imem + pc, &imm) && imm >= 0x100) {
            *outcmdq = imm;
            break;
        }
    }

    for (pc = 0; pc + 4 <= size; pc += 2) {
        if (!ck_patcher_thumb_branch(imem + pc, pc, &target) || target != entry) { continue; }

        start = pc > 0x20 ? pc - 0x20 : 4;
        for (back = start; back < pc; back += 2) {
            if (lduw_le_p(imem + back) != THUMB_MOVS_R0_1) { continue; }

            for (prev = back - 4; prev >= start; prev -= 2) {
                if (ck_patcher_thumb_movw(imem + prev, &imm)) {
                    *activeq_flags = imm;
                    break;
                }
            }
        }
    }

    return *outcmdq != 0 && *activeq_flags != 0;
}
