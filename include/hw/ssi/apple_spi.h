/*
 * Apple SPI Controller.
 *
 * Copyright (c) 2024-2026 Visual Ehrmanntraut (VisualEhrmanntraut).
 * Copyright (c) 2023-2026 Christian Inci (chris-pcguy).
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

#include "hw/dma/apple_sio.h"
#include "hw/sysbus.h"
#include "qom/object.h"

#define APPLE_SPI_MMIO_SIZE (0x4000)

#define TYPE_APPLE_SPI "apple-spi"
OBJECT_DECLARE_SIMPLE_TYPE(AppleSPIState, APPLE_SPI)

SSIBus* apple_spi_get_bus(AppleSPIState* s);

AppleSPIState* apple_spi_create(hwaddr addr, qemu_irq irq, AppleSIO* sio, uint32_t tx_chan_id, uint32_t rx_chan_id);
