/*
 * Apple Device Address Resolution Table.
 *
 * Copyright (c) 2024-2026 Visual Ehrmanntraut (VisualEhrmanntraut).
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

#pragma once

#include "qemu/osdep.h"
#include "hw/arm/dt.h"
#include "qom/object.h"

typedef struct AppleDARTState AppleDARTState;

#define TYPE_APPLE_DART "apple-dart"
OBJECT_DECLARE_SIMPLE_TYPE(AppleDARTState, APPLE_DART)

#define TYPE_APPLE_DART_IOMMU_MEMORY_REGION "apple-dart-iommu"
OBJECT_DECLARE_SIMPLE_TYPE(AppleDARTIOMMUMemoryRegion, APPLE_DART_IOMMU_MEMORY_REGION)

#define DART_DART_FORCE_ACTIVE "dart-dart_force_active"
#define DART_DART_REQUEST_SID  "dart-dart_request_sid"
#define DART_DART_RELEASE_SID  "dart-dart_release_sid"
#define DART_DART_SELF         "dart-dart_self"

IOMMUMemoryRegion* apple_dart_iommu_mr(AppleDARTState* dart, uint32_t sid);
void               apple_dart_iommu_table_pages(IOMMUMemoryRegion* mr, GArray* out);
IOMMUMemoryRegion* apple_dart_instance_iommu_mr(AppleDARTState* s, uint32_t instance, uint32_t sid);
AppleDARTState*    apple_dart_from_node(AppleDTNode* node);

/*
 * Map [@iova, @iova + @size) to @pa for @sid with an L1 table and, unless
 * @size is 0, an L2 table in the pages at @tables, and translate through them
 * from now on, including after reset, as iBoot leaves it.
 */
void apple_dart_map_boot_range(AppleDARTState* dart, uint32_t sid, hwaddr tables, hwaddr iova, hwaddr pa, hwaddr size);
/* Forget the tables of `apple_dart_map_boot_range` from the next reset on. */
void apple_dart_clear_boot_ranges(AppleDARTState* dart);
