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
#pragma once

#include "qemu/osdep.h"

/*
 * Find where the APT M3 firmware in @imem keeps the OutCmdQ and the ActiveQ
 * flags array, as offsets into its data memory.
 */
bool apple_apt_m3_find_queues(const uint8_t* imem, size_t size, uint32_t* outcmdq, uint32_t* activeq_flags);
