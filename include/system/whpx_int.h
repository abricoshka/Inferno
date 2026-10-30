/*
 * QEMU Windows Hypervisor Platform accelerator (WHPX) support
 *
 * Copyright Microsoft, Corp. 2017
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

/* header to be included in WHPX-specific code */

#pragma once

#include <windows.h>
#include <winhvplatform.h>

#define WHPX_FUNCTIONS(X)                                                                                              \
    X(HRESULT, WHvGetCapability,                                                                                       \
      (WHV_CAPABILITY_CODE CapabilityCode, VOID * CapabilityBuffer, UINT32 CapabilityBufferSizeInBytes,                \
       UINT32 * WrittenSizeInBytes))                                                                                   \
    X(HRESULT, WHvCreatePartition, (WHV_PARTITION_HANDLE * Partition))                                                 \
    X(HRESULT, WHvSetupPartition, (WHV_PARTITION_HANDLE Partition))                                                    \
    X(HRESULT, WHvDeletePartition, (WHV_PARTITION_HANDLE Partition))                                                   \
    X(HRESULT, WHvSetPartitionProperty,                                                                                \
      (WHV_PARTITION_HANDLE Partition, WHV_PARTITION_PROPERTY_CODE PropertyCode, const VOID* PropertyBuffer,           \
       UINT32 PropertyBufferSizeInBytes))                                                                              \
    X(HRESULT, WHvMapGpaRange,                                                                                         \
      (WHV_PARTITION_HANDLE Partition, VOID * SourceAddress, WHV_GUEST_PHYSICAL_ADDRESS GuestAddress,                  \
       UINT64 SizeInBytes, WHV_MAP_GPA_RANGE_FLAGS Flags))                                                             \
    X(HRESULT, WHvUnmapGpaRange,                                                                                       \
      (WHV_PARTITION_HANDLE Partition, WHV_GUEST_PHYSICAL_ADDRESS GuestAddress, UINT64 SizeInBytes))                   \
    X(HRESULT, WHvCreateVirtualProcessor, (WHV_PARTITION_HANDLE Partition, UINT32 VpIndex, UINT32 Flags))              \
    X(HRESULT, WHvDeleteVirtualProcessor, (WHV_PARTITION_HANDLE Partition, UINT32 VpIndex))                            \
    X(HRESULT, WHvRunVirtualProcessor,                                                                                 \
      (WHV_PARTITION_HANDLE Partition, UINT32 VpIndex, VOID * ExitContext, UINT32 ExitContextSizeInBytes))             \
    X(HRESULT, WHvCancelRunVirtualProcessor, (WHV_PARTITION_HANDLE Partition, UINT32 VpIndex, UINT32 Flags))           \
    X(HRESULT, WHvGetVirtualProcessorRegisters,                                                                        \
      (WHV_PARTITION_HANDLE Partition, UINT32 VpIndex, const WHV_REGISTER_NAME* RegisterNames, UINT32 RegisterCount,   \
       WHV_REGISTER_VALUE* RegisterValues))                                                                            \
    X(HRESULT, WHvSetVirtualProcessorRegisters,                                                                        \
      (WHV_PARTITION_HANDLE Partition, UINT32 VpIndex, const WHV_REGISTER_NAME* RegisterNames, UINT32 RegisterCount,   \
       const WHV_REGISTER_VALUE* RegisterValues))                                                                      \
    X(HRESULT, WHvRequestInterrupt,                                                                                    \
      (WHV_PARTITION_HANDLE Partition, const WHV_INTERRUPT_CONTROL* Interrupt, UINT32 InterruptControlSize))           \
    X(HRESULT, WHvGetVirtualProcessorState,                                                                            \
      (WHV_PARTITION_HANDLE Partition, UINT32 VpIndex, WHV_VIRTUAL_PROCESSOR_STATE_TYPE StateType, VOID * Buffer,      \
       UINT32 BufferSizeInBytes, UINT32 * BytesWritten))                                                               \
    X(HRESULT, WHvSetVirtualProcessorState,                                                                            \
      (WHV_PARTITION_HANDLE Partition, UINT32 VpIndex, WHV_VIRTUAL_PROCESSOR_STATE_TYPE StateType, const VOID* Buffer, \
       UINT32 BufferSizeInBytes))

#define WHPX_DECLARE_FUNCTION(type, name, args) type(WINAPI* name) args;

/// WinHvPlatform.dll, which is only there with the Windows Hypervisor Platform feature, so it's loaded at run time.
typedef struct
{
    WHPX_FUNCTIONS(WHPX_DECLARE_FUNCTION)
} WHPXDispatch;

#undef WHPX_DECLARE_FUNCTION

extern WHPXDispatch         whp;
extern WHV_PARTITION_HANDLE whpx_partition;

struct AccelCPUState
{
    WHV_RUN_VP_EXIT_CONTEXT exit;
    // The state `env` doesn't hold is put too on the next entry.
    bool reset;
    // The names of the system registers in the cpreg list, in its order.
    WHV_REGISTER_NAME* cpreg_names;
    // A timer's PPI is disabled until it stops firing.
    bool timers_masked;
    // FIQs are held off with the priority mask until they're unmasked.
    bool fiq_held;
};

void assert_whpx_ok_impl(HRESULT hr, const char* file, unsigned int line, const char* exp);
#define assert_whpx_ok(EX) assert_whpx_ok_impl((EX), __FILE__, __LINE__, #EX)

/// Configures the partition before it's set up.
int whpx_arch_configure(MachineState* ms);
/// Initialises the accelerator after the partition is set up.
int  whpx_arch_init(MachineState* ms);
int  whpx_arch_init_vcpu(CPUState* cpu);
void whpx_arch_vcpu_destroy(CPUState* cpu);
void whpx_arch_get_registers(CPUState* cpu);
/// With `reset`, also puts the state `env` doesn't hold, like the GIC's.
void whpx_arch_put_registers(CPUState* cpu, bool reset);
/// Runs the vCPU until something needs the BQL; returns an `EXCP_*`, which the caller handles.
int whpx_arch_vcpu_exec(CPUState* cpu);
