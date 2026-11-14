// SPDX-License-Identifier: BSD-2-Clause
// Reviewed TB710FU board table from bmk-keyeast/UniXNU-tb710fu@23235249d71604f8a6f743680ef8fd1003fe7189.
// All 40 source ranges are preserved. New hardware qualification is still required.
#include <Library/MemoryMapLib.h>

STATIC
EFI_MEMORY_REGION_DESCRIPTOR
gMemoryDescriptor[] = {
  // Name, Address, Length, HobOption, ResourceType, ResourceAttribute, MemoryType, ArmAttribute

  // DDR Regions
  {"RSRV0",               0x816E0000, 0x00320000, AddMem, SYS_MEM, SYS_MEM_CAP, Reserv, WRITE_BACK_XN},
  {"XBL_DT",              0x81A00000, 0x00040000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK_XN},
  {"XBL_Ramdump",         0x81A40000, 0x001C0000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN},
  {"AOP",                 0x81C00000, 0x000A0000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN},
  {"UEFI_Log",            0x81CE4000, 0x00010000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN},
  {"SMEM",                0x81D00000, 0x00200000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN},
  {"PvmFw",               0x824A0000, 0x00100000, AddMem, SYS_MEM, SYS_MEM_CAP, Reserv, WRITE_BACK_XN},
  // Stock TB710FU map (xbl_config, scripts/stock-memory-map.py): PIL is 0x16F80000 and the
  // demura panel-calibration buffer follows it at 0xA3600000.
  {"PIL_Reserved",        0x8BC00000, 0x16F80000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN},
  {"Display_Demura",      0xA3600000, 0x02B00000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_THROUGH_XN},
  {"DBI_Dump",           0xA6100000, 0x00F00000, NoHob,  MMAP_IO, INITIALIZED, Conv,   UNCACHED_UNBUFFERED_XN},
  {"FD_Reserved",         0xA7000000, 0x00100000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK},
  {"UEFI_FD",             0xA7100000, 0x00300000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK},
  {"UEFI_FD_Reserved",    0xA7400000, 0x00200000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK},
  {"Info_Blk",            0xA7601000, 0x00001000, AddMem, SYS_MEM, SYS_MEM_CAP, RtData, WRITE_BACK_XN},
  {"MMU_PageTables",      0xA7602000, 0x00003000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK_XN},
  {"Log_Buffer",          0xA7605000, 0x00008000, AddMem, SYS_MEM, SYS_MEM_CAP, RtData, WRITE_BACK_XN},
  {"UEFI_Stack",          0xA760D000, 0x00040000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK_XN},
  {"SEC_Heap",            0xA764D000, 0x0008C000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK_XN},
  {"Sched_Heap",          0xA76D9000, 0x00400000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK_XN},
  {"FV_Region",           0xA7AD9000, 0x00400000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK_XN},
  {"UEFI_RESV",           0xA7ED9000, 0x00127000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK_XN},
  {"Kernel",              0xA8000000, 0x10000000, AddMem, SYS_MEM, SYS_MEM_CAP, Reserv, WRITE_BACK_XN},
  // Stock heap runs to 0xD3A00000; it stops at 0xD3000000 here because the mainline
  // board DT places rmtfs there (stock Android allocates rmtfs dynamically).
  {"DXE_Heap",            0xB8000000, 0x1B000000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK_XN},
  {"Display_Reserved",    0xD5100000, 0x02B00000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_THROUGH_XN},
  {"TZApps_Reserved",     0xD8800000, 0x1B000000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN},
  // ref23235249 author observation: a live FDT on that reference device reported
  // DDR at 0x8C0000000 + 0x140000000. That author selected this 3 GiB subset
  // for an aligned 2 GiB DarwinBoot owned window without vendor RamManagerDxe.
  // This clean project has no independently qualified live FDT or physical RAM
  // range evidence for this entry. Preserve it as reference policy pending proof.
  {"RAM_High",            0x8C0000000, 0xC0000000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK_XN},

  // Other Memory Regions
  {"IMEM_Base",           0x14680000, 0x0002A000, NoHob,  MMAP_IO, INITIALIZED, Conv,   NS_DEVICE},
  {"IMEM_Cookie_Base",    0x146AA000, 0x00016000, AddDev, MMAP_IO, INITIALIZED, Conv,   NS_DEVICE},

  // Register Regions
  {"IPC_ROUTER_TOP",      0x00400000, 0x00100000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"SECURITY_CONTROL",    0x00780000, 0x00007000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"QUP",                 0x00800000, 0x00300000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"PRNG_CFG_PRNG",       0x010C0000, 0x0000C000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"CRYPTO0_CRYPTO",      0x01DC0000, 0x00040000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"CLK_TCSR_TCSR_REGS",  0x01F00000, 0x00100000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"PERIPH_SS",           0x08800000, 0x00100000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"USB",                 0x0A600000, 0x00200000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"AOSS",                0x0B000000, 0x04000000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"TLMM",                0x0F000000, 0x01000000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"SMMU",                0x15000000, 0x00200000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"APSS_HM",             0x17000000, 0x02000000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE}
};

VOID
GetMemoryMap (
  OUT EFI_MEMORY_REGION_DESCRIPTOR **MemoryDescriptor,
  OUT UINT8                         *MemoryDescriptorCount)
{
  // Pass Data
  *MemoryDescriptor      = gMemoryDescriptor;
  *MemoryDescriptorCount = ARRAY_SIZE (gMemoryDescriptor);
}
