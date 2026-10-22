/*
 * ChefKiss Patcher (PenguinWizardryC).
 *
 * Copyright (c) 2025-2026 Visual Ehrmanntraut (VisualEhrmanntraut).
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

#include "hw/arm/patcher.h"
#include "qemu/bitops.h"
#include "qemu/bswap.h"
#include "qemu/error-report.h"

CKPatcherRange* ck_patcher_range_from_ptr(const char* name, uint8_t* ptr, vaddr size)
{
    CKPatcherRange* range = g_new0(CKPatcherRange, 1);
    range->addr           = 0x0;
    range->length         = size;
    range->ptr            = ptr;
    range->name           = name;
    return range;
}

bool ck_patcher_find_callback_ctx(CKPatcherRange* range, const char* name, const uint8_t* pattern, const uint8_t* mask,
                                  size_t len, size_t align, uint32_t count, void* ctx, CKPatcherCallback callback)
{
    size_t   i;
    size_t   match_i;
    uint8_t* match;
    bool     found;
    uint32_t applied = 0;

    if (align == 0) { align = 1; }
    else {
        assert_cmpuint(len % align, ==, 0);
    }

    if (range->length < len) {
        error_report("`%s` patch is bigger than `%s`.", name, range->name);
        return false;
    }

    if (mask == NULL) {
        for (i = 0; i < range->length; i += align) {
            found = true;
            match = range->ptr + i;
            for (match_i = 0; match_i < len; ++match_i) {
                if (match[match_i] != pattern[match_i]) {
                    found = false;
                    break;
                }
            }
            if (found && callback(ctx, match) && ++applied == count) { break; }
        }
    }
    else {
        for (i = 0; i < len; ++i) { assert_cmphex(pattern[i] & mask[i], ==, pattern[i]); }

        for (i = 0; i < range->length; i += align) {
            found = true;
            match = range->ptr + i;
            for (match_i = 0; match_i < len; ++match_i) {
                if ((match[match_i] & mask[match_i]) != pattern[match_i]) {
                    found = false;
                    break;
                }
            }
            if (found && callback(ctx, match) && ++applied == count) { break; }
        }
    }

    if (applied == 0) {
        error_report("`%s` patch did not apply in `%s`.", name, range->name);
        return false;
    }
    if (count != 0 && applied != count) {
        error_report("`%s` patch applied %u of %u times in `%s`.", name, applied, count, range->name);
        return false;
    }
    if (applied == 1) { info_report("`%s` patch applied in `%s`.", name, range->name); }
    else {
        info_report("`%s` patch applied %u times in `%s`.", name, applied, range->name);
    }

    return true;
}

bool ck_patcher_find_callback(CKPatcherRange* range, const char* name, const uint8_t* pattern, const uint8_t* mask,
                              size_t len, size_t align, uint32_t count, CKPatcherCallback callback)
{ return ck_patcher_find_callback_ctx(range, name, pattern, mask, len, align, count, NULL, callback); }

typedef struct
{
    const uint8_t* replacement;
    const uint8_t* mask;
    size_t         offset;
    size_t         len;
} CKPatcherFindReplaceContext;

static bool ck_patcher_find_replace_callback(void* ctx, uint8_t* buffer)
{
    CKPatcherFindReplaceContext* repl_ctx;
    size_t                       i;

    repl_ctx = ctx;
    if (repl_ctx->mask == NULL) { memcpy(buffer + repl_ctx->offset, repl_ctx->replacement, repl_ctx->len); }
    else {
        for (i = 0; i < repl_ctx->len; ++i) {
            buffer[repl_ctx->offset + i] =
                (buffer[repl_ctx->offset + i] & repl_ctx->mask[i]) | repl_ctx->replacement[i];
        }
    }
    return true;
}

bool ck_patcher_find_replace(CKPatcherRange* range, const char* name, const uint8_t* pattern, const uint8_t* mask,
                             size_t len, size_t align, uint32_t count, const uint8_t* replacement,
                             const uint8_t* replacement_mask, size_t replace_off, size_t replace_len)
{
    CKPatcherFindReplaceContext ctx;

    assert_cmphex(replace_off + replace_len, <=, len);

    ctx.replacement = replacement;
    ctx.mask        = replacement_mask;
    ctx.offset      = replace_off;
    ctx.len         = replace_len;

    return ck_patcher_find_callback_ctx(range, name, pattern, mask, len, align, count, &ctx,
                                        ck_patcher_find_replace_callback);
}

uint8_t* ck_patcher_find_next_insn(uint8_t* buffer, uint32_t num, uint32_t insn, uint32_t mask, uint32_t skip)
{
    assert_cmphex(insn & mask, ==, insn);

    for (uint32_t i = 0; i < num; ++i) {
        uint8_t* cur = buffer + (i * sizeof(uint32_t));
        if ((ldl_le_p(cur) & mask) == insn) {
            if (skip == 0) { return cur; }
            --skip;
        }
    }

    return NULL;
}

uint8_t* ck_patcher_find_prev_insn(uint8_t* buffer, uint32_t num, uint32_t insn, uint32_t mask, uint32_t skip)
{
    assert_cmphex(insn & mask, ==, insn);

    for (uint32_t i = 0; i < num; ++i) {
        uint8_t* cur = buffer - (i * sizeof(uint32_t));
        if ((ldl_le_p(cur) & mask) == insn) {
            if (skip == 0) { return cur; }
            --skip;
        }
    }

    return NULL;
}

bool ck_patcher_adrp_add_va(const uint8_t* buffer, vaddr va, vaddr* out)
{
    uint32_t adrp = ldl_le_p(buffer);
    uint32_t add  = ldl_le_p(buffer + sizeof(uint32_t));
    int64_t  imm;
    uint32_t imm12;

    // adrp x?, #?
    if ((adrp & 0x9F000000) != 0x90000000) { return false; }
    // add x?, x?, #? (64-bit, immediate)
    if ((add & 0xFF800000) != 0x91000000) { return false; }

    imm   = ((int64_t)sextract32(adrp, 5, 19) << 2) | extract32(adrp, 29, 2);
    imm12 = extract32(add, 10, 12);
    if (extract32(add, 22, 1)) { imm12 <<= 12; }

    *out = ((va & ~(vaddr)0xFFF) + (imm << 12)) + imm12;
    return true;
}

bool ck_patcher_thumb_movw(const uint8_t* buffer, uint16_t* imm)
{
    uint16_t hw1 = lduw_le_p(buffer);
    uint16_t hw2 = lduw_le_p(buffer + 2);

    if ((hw1 & 0xFBF0) != 0xF240 || (hw2 & 0x8000) != 0) { return false; }

    *imm = extract32(hw1, 0, 4) << 12 | extract32(hw1, 10, 1) << 11 | extract32(hw2, 12, 3) << 8 | extract32(hw2, 0, 8);
    return true;
}

bool ck_patcher_thumb_str_imm12(const uint8_t* buffer, uint16_t* imm)
{
    if ((lduw_le_p(buffer) & 0xFFF0) != 0xF8C0) { return false; }

    *imm = extract32(lduw_le_p(buffer + 2), 0, 12);
    return true;
}

bool ck_patcher_thumb_branch(const uint8_t* buffer, uint32_t pc, uint32_t* target)
{
    uint16_t hw1 = lduw_le_p(buffer);
    uint16_t hw2 = lduw_le_p(buffer + 2);
    uint32_t s;
    uint32_t off;

    if ((hw1 & 0xF800) != 0xF000 || (hw2 & 0x9000) != 0x9000) { return false; }

    s       = extract32(hw1, 10, 1);
    off     = s << 24 | (!(extract32(hw2, 13, 1) ^ s)) << 23 | (!(extract32(hw2, 11, 1) ^ s)) << 22
              | extract32(hw1, 0, 10) << 12 | extract32(hw2, 0, 11) << 1;
    *target = pc + 4 + sextract32(off, 0, 25);
    return true;
}
