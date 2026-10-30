/*
 * General Apple XNU memory utilities.
 *
 * Copyright (c) 2023-2026 Visual Ehrmanntraut (VisualEhrmanntraut).
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
#include "exec/hwaddr.h"
#include "hw/arm/mem.h"
#include "qapi/error.h"
#include "system/memory.h"

hwaddr vtop_bases(vaddr va, hwaddr phys_base, vaddr virt_base)
{
    assert_cmphex(phys_base, !=, 0);
    assert_cmphex(virt_base, !=, 0);

    return va - virt_base + phys_base;
}

vaddr ptov_bases(hwaddr pa, hwaddr phys_base, vaddr virt_base)
{
    assert_cmphex(phys_base, !=, 0);
    assert_cmphex(virt_base, !=, 0);

    return pa - phys_base + virt_base;
}

void allocate_ram(MemoryRegion* top, const char* name, hwaddr addr, hwaddr size, int priority)
{
    MemoryRegion* sec = g_new(MemoryRegion, 1);
    memory_region_init_ram(sec, NULL, name, size, &error_fatal);
    memory_region_add_subregion_overlap(top, addr, sec, priority);
}
