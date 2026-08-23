// bios_call_names.cpp — names for the PSX kernel's A0/B0/C0 call tables.
//
// A PSY-Q-built title reaches the kernel through three-instruction dispatch
// thunks (`addiu $t2, $zero, 0xA0; jr $t2; addiu $t1, $zero, <index>`). Static
// analysis sees those as tiny prologue-less stubs with no jr $ra, which is
// exactly the shape it trusts least — yet they are among the most-called
// functions in any image. Naming them turns the single worst-looking cluster in
// the report into the best-understood one.
//
// Table source: the A0/B0/C0 function lists in psx-spx (Nocash's PlayStation
// Specifications), "BIOS Function Summary". Entries left empty below are ones
// not carried here; those render as `bios_A0_XX` with no name claim rather than
// a guess.

#include <cstddef>
#include <cstdint>

namespace PSXRecomp::Analysis {

namespace {

const char* const kA0[] = {
    /*00*/ "FileOpen", "FileSeek", "FileRead", "FileWrite", "FileClose",
    /*05*/ "FileIoctl", "exit", "FileGetDeviceFlag", "FileGetc", "FilePutc",
    /*0A*/ "todigit", "atof", "strtoul", "strtol", "abs", "labs",
    /*10*/ "atoi", "atol", "atob", "setjmp", "longjmp", "strcat", "strncat",
    /*17*/ "strcmp", "strncmp", "strcpy", "strncpy", "strlen", "index",
    /*1D*/ "rindex", "strchr", "strrchr",
    /*20*/ "strpbrk", "strspn", "strcspn", "strtok", "strstr", "toupper",
    /*26*/ "tolower", "bcopy", "bzero", "bcmp", "memcpy", "memset", "memmove",
    /*2D*/ "memcmp", "memchr", "rand",
    /*30*/ "srand", "qsort", "strtod", "malloc", "free", "lsearch", "bsearch",
    /*37*/ "calloc", "realloc", "InitHeap", "SystemErrorExit", "std_in_getchar",
    /*3C*/ "std_out_putchar", "std_in_gets", "std_out_puts", "printf",
    /*40*/ "SystemErrorUnresolvedException", "LoadExeHeader", "LoadExeFile",
    /*43*/ "DoExecute", "FlushCache", "init_a0_b0_c0_vectors", "GPU_dw",
    /*47*/ "gpu_send_dma", "SendGP1Command", "GPU_cw", "GPU_cwp",
    /*4B*/ "send_gpu_linked_list", "gpu_abort_dma", "GetGPUStatus", "gpu_sync",
    /*4F*/ "SystemError",
    /*50*/ "SystemError", "LoadAndExecute", "SystemError", "SystemError",
    /*54*/ "CdInit", "_bu_init", "CdRemove", "", "", "", "",
    /*5B*/ "dev_tty_init", "dev_tty_open", "dev_tty_in_out", "dev_tty_ioctl",
    /*5F*/ "dev_cd_open",
    /*60*/ "dev_cd_read", "dev_cd_close", "dev_cd_firstfile", "dev_cd_nextfile",
    /*64*/ "dev_cd_chdir", "dev_card_open", "dev_card_read", "dev_card_write",
    /*68*/ "dev_card_close", "dev_card_firstfile", "dev_card_nextfile",
    /*6B*/ "dev_card_erase", "dev_card_undelete", "dev_card_format",
    /*6E*/ "dev_card_rename", "card_clear_error",
    /*70*/ "_bu_init", "CdInit", "CdRemove", "", "", "", "", "",
    /*78*/ "CdAsyncSeekL", "", "", "", "CdAsyncGetStatus", "",
    /*7E*/ "CdAsyncReadSector", "",
    /*80*/ "", "CdAsyncSetMode", "", "", "", "", "", "",
    /*88*/ "", "", "", "", "", "", "", "",
    /*90*/ "CdromIoIrqFunc1", "CdromDmaIrqFunc1", "CdromIoIrqFunc2",
    /*93*/ "CdromDmaIrqFunc2", "CdromGetInt5errCode", "CdInitSubFunc",
    /*96*/ "AddCDROMDevice", "AddMemCardDevice", "AddDuartTtyDevice",
    /*99*/ "AddDummyTtyDevice", "SystemError", "SystemError", "SetConf",
    /*9D*/ "GetConf", "SetCdromIrqAutoAbort", "SetMemSize",
};

const char* const kB0[] = {
    /*00*/ "alloc_kernel_memory", "free_kernel_memory", "init_timer",
    /*03*/ "get_timer", "enable_timer_irq", "disable_timer_irq",
    /*06*/ "restart_timer", "DeliverEvent", "OpenEvent", "CloseEvent",
    /*0A*/ "WaitEvent", "TestEvent", "EnableEvent", "DisableEvent",
    /*0E*/ "OpenThread", "CloseThread",
    /*10*/ "ChangeThread", "", "InitPad", "StartPad", "StopPad",
    /*15*/ "OutdatedPadInitAndStart", "OutdatedPadGetButtons",
    /*17*/ "ReturnFromException", "SetDefaultExitFromException",
    /*19*/ "SetCustomExitFromException", "", "", "", "", "", "",
    /*20*/ "UnDeliverEvent", "", "", "", "", "", "", "",
    /*28*/ "", "", "", "", "", "", "", "",
    /*30*/ "", "",
    /*32*/ "FileOpen", "FileSeek", "FileRead", "FileWrite", "FileClose",
    /*37*/ "FileIoctl", "exit", "FileGetDeviceFlag", "FileGetc", "FilePutc",
    /*3C*/ "std_in_getchar", "std_out_putchar", "std_in_gets", "std_out_puts",
    /*40*/ "chdir", "FormatDevice", "firstfile", "nextfile", "FileRename",
    /*45*/ "FileDelete", "FileUndelete", "AddDevice", "RemoveDevice",
    /*49*/ "PrintInstalledDevices", "InitCard", "StartCard", "StopCard",
    /*4D*/ "_card_info_subfunc", "_card_write", "_card_read",
    /*50*/ "_new_card", "Krom2RawAdd", "SystemError", "Krom2Offset",
    /*54*/ "GetLastError", "GetLastFileError", "GetC0Table", "GetB0Table",
    /*58*/ "get_bu_callback_port", "testdevice", "SystemError",
    /*5B*/ "ChangeClearPad", "get_card_status", "wait_card_status",
};

const char* const kC0[] = {
    /*00*/ "EnqueueTimerAndVblankIrqs", "EnqueueSyscallHandler", "SysEnqIntRP",
    /*03*/ "SysDeqIntRP", "get_free_EvCB_slot", "get_free_TCB_slot",
    /*06*/ "ExceptionHandler", "InstallExceptionHandlers", "SysInitMemory",
    /*09*/ "SysInitKernelVariables", "ChangeClearRCnt", "SystemError",
    /*0C*/ "InitDefInt", "SetIrqAutoAck", "", "",
    /*10*/ "", "", "InstallDevices", "FlushStdInOutPut", "SystemError",
    /*15*/ "tty_cdevinput", "tty_cdevscan", "tty_circgetc", "tty_circputc",
    /*19*/ "ioabort", "set_card_find_mode", "KernelRedirect", "AdjustA0Table",
    /*1D*/ "get_card_find_mode",
};

template <std::size_t N>
const char* lookup(const char* const (&tab)[N], uint32_t idx) {
    if (idx >= N) return "";
    return tab[idx];
}

} // namespace

// Returns "" when the index is out of range or intentionally uncarried.
const char* bios_call_name(uint32_t table, uint32_t index) {
    switch (table) {
    case 0xA0: return lookup(kA0, index);
    case 0xB0: return lookup(kB0, index);
    case 0xC0: return lookup(kC0, index);
    default:   return "";
    }
}

} // namespace PSXRecomp::Analysis
