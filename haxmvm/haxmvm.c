#include "haxmvm.h"

#include "../krnl386/kernel16_private.h"

BOOL initflag;
UINT8 *mem;
#define KRNL386 "krnl386.exe16"
BOOL is_single_step = FALSE;
DWORD WINAPI panic_msgbox(LPCVOID data)
{
    MessageBoxA(NULL, (LPCSTR)data, "Hypervisor error", MB_OK | MB_ICONERROR);
    HeapFree(GetProcessHeap(), 0, data);
    return 0;
}
void haxmvm_panic(const char *fmt, ...)
{
    LPSTR buffer = HeapAlloc(GetProcessHeap(), 0, 512);
    DWORD threadId;
    va_list arg;

    va_start(arg, fmt);
    vsnprintf(buffer, 512, fmt, arg);
    va_end(arg);
    buffer[512 - 1] = '\0';
    HANDLE hThread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)panic_msgbox, buffer, 0, &threadId);
    WaitForSingleObject(hThread, INFINITE);
    ExitThread(1);
}
PVOID dynamic_setWOW32Reserved(PVOID w)
{
    static PVOID(*setWOW32Reserved)(PVOID);
    if (!setWOW32Reserved)
    {
        HMODULE krnl386 = LoadLibraryA(KRNL386);
        setWOW32Reserved = (PVOID(*)(PVOID))GetProcAddress(krnl386, "setWOW32Reserved");
    }
    return setWOW32Reserved(w);
}
PVOID dynamic_getWOW32Reserved()
{
    static PVOID(*getWOW32Reserved)();
    if (!getWOW32Reserved)
    {
        HMODULE krnl386 = LoadLibraryA(KRNL386);
        getWOW32Reserved = (PVOID(*)())GetProcAddress(krnl386, "getWOW32Reserved");
    }
    return getWOW32Reserved();
}
WINE_VM86_TEB_INFO *dynamic_getGdiTebBatch()
{
    static WINE_VM86_TEB_INFO*(*getGdiTebBatch)();
    if (!getGdiTebBatch)
    {
        HMODULE krnl386 = LoadLibraryA(KRNL386);
        getGdiTebBatch = (WINE_VM86_TEB_INFO*(*)())GetProcAddress(krnl386, "getGdiTebBatch");
    }
    return getGdiTebBatch();
}
void dynamic__wine_call_int_handler(CONTEXT *context, BYTE intnum)
{
    static void(*__wine_call_int_handler)(CONTEXT *context, BYTE intnum);
    if (!__wine_call_int_handler)
    {
        HMODULE krnl386 = LoadLibraryA(KRNL386);
        __wine_call_int_handler = (void(*)(CONTEXT *context, BYTE intnum))GetProcAddress(krnl386, "__wine_call_int_handler");
    }
    __wine_call_int_handler(context, intnum);
}
/***********************************************************************
*           SELECTOR_SetEntries
*
* Set the LDT entries for an array of selectors.
*/
static BOOL SELECTOR_SetEntries(WORD sel, const void *base, DWORD size, unsigned char flags)
{
    LDT_ENTRY entry;
    WORD i, count;

    wine_ldt_set_base(&entry, base);
    wine_ldt_set_limit(&entry, size - 1);
    wine_ldt_set_flags(&entry, flags);
    count = (size + 0xffff) / 0x10000;
    for (i = 0; i < count; i++)
    {
        if (wine_ldt_set_entry(sel + (i << 3), &entry) < 0) return FALSE;
        wine_ldt_set_base(&entry, (char*)wine_ldt_get_base(&entry) + 0x10000);
        /* yep, Windows sets limit like that, not 64K sel units */
        wine_ldt_set_limit(&entry, wine_ldt_get_limit(&entry) - 0x10000);
    }
    return TRUE;
}
void wine_ldt_free_entries(unsigned short sel, int count);
/***********************************************************************
*           SELECTOR_AllocBlock
*
* Allocate selectors for a block of linear memory.
*/
WORD SELECTOR_AllocBlock(const void *base, DWORD size, unsigned char flags)
{
    WORD sel, count;

    if (!size) return 0;
    count = (size + 0xffff) / 0x10000;
    if ((sel = wine_ldt_alloc_entries(count)))
    {
        if (SELECTOR_SetEntries(sel, base, size, flags)) return sel;
        wine_ldt_free_entries(sel, count);
        sel = 0;
    }
    return sel;
}
_declspec(dllimport) LDT_ENTRY wine_ldt[8192];

HANDLE hSystem;
HANDLE hVM;
HANDLE hVCPU;
struct hax_tunnel *tunnel;

void load_seg(segment_desc_t *segment, WORD sel)
{
    segment->selector = sel;
    segment->base = (uint64)wine_ldt_get_base(wine_ldt + (sel >> 3));
    segment->limit = 0xffffffff;// (uint64)wine_ldt_get_limit(wine_ldt + (sel >> 3));
    segment->type = wine_ldt[sel >> 3].HighWord.Bits.Type;
    segment->present = wine_ldt[sel >> 3].HighWord.Bits.Pres;
    segment->operand_size = wine_ldt[sel >> 3].HighWord.Bits.Default_Big;
    segment->dpl = wine_ldt[sel >> 3].HighWord.Bits.Dpl;
    segment->granularity = wine_ldt[sel >> 3].HighWord.Bits.Granularity;
}

void set_eflags(struct vcpu_state_t *state, DWORD eflags)
{
    state->_eflags = eflags | 2 | 0x3000 | (is_single_step ? 0x100 : 0);
}
void load_context_to_state(CONTEXT *context, struct vcpu_state_t *state)
{
    DWORD bytes;
    load_seg(&state->_gs, (WORD)context->SegGs);
    load_seg(&state->_fs, (WORD)context->SegFs);
    load_seg(&state->_es, (WORD)context->SegEs);
    load_seg(&state->_ds, (WORD)context->SegDs);
    load_seg(&state->_cs, (WORD)context->SegCs);
    load_seg(&state->_ss, (WORD)context->SegSs);

    state->_edi = context->Edi;
    state->_esi = context->Esi;
    state->_ebx = context->Ebx;
    state->_edx = context->Edx;
    state->_ecx = context->Ecx;
    state->_eax = context->Eax;

    state->_ebp = context->Ebp;
    state->_eip = context->Eip;
    set_eflags(state, context->EFlags);
    state->_esp = context->Esp;
}

void load_context(CONTEXT *context)
{
    DWORD bytes;
    struct vcpu_state_t state;
    if (!DeviceIoControl(hVCPU, HAX_VCPU_GET_REGS, NULL, 0, &state, sizeof(state), &bytes, NULL))
        return;
    load_seg(&state._gs, (WORD)context->SegGs);
    load_seg(&state._fs, (WORD)context->SegFs);
    load_seg(&state._es, (WORD)context->SegEs);
    load_seg(&state._ds, (WORD)context->SegDs);
    load_seg(&state._cs, (WORD)context->SegCs);
    load_seg(&state._ss, (WORD)context->SegSs);

    state._edi = context->Edi;
    state._esi = context->Esi;
    state._ebx = context->Ebx;
    state._edx = context->Edx;
    state._ecx = context->Ecx;
    state._eax = context->Eax;

    state._ebp = context->Ebp;
    state._eip = context->Eip;
    set_eflags(&state, context->EFlags);
    state._esp = context->Esp;

    if (!DeviceIoControl(hVCPU, HAX_VCPU_SET_REGS, &state, sizeof(state), NULL, 0, &bytes, NULL))
        return;
}

void save_context_from_state(CONTEXT *context, struct vcpu_state_t *state)
{
    context->SegGs = state->_gs.selector;
    context->SegFs = state->_fs.selector;
    context->SegEs = state->_es.selector;
    context->SegDs = state->_ds.selector;
    context->SegCs = state->_cs.selector;
    context->SegSs = state->_ss.selector;

    context->Edi = state->_edi;
    context->Esi = state->_esi;
    context->Ebx = state->_ebx;
    context->Edx = state->_edx;
    context->Ecx = state->_ecx;
    context->Eax = state->_eax;

    context->Ebp = state->_ebp;
    context->Eip = state->_eip;
    context->EFlags = state->_eflags & ~2;
    context->Esp = state->_esp;
    dynamic_setWOW32Reserved((PVOID)(state->_ss.selector << 16 | state->_sp));
}
void save_context(CONTEXT *context)
{
    DWORD bytes;
    struct vcpu_state_t state;
    if (!DeviceIoControl(hVCPU, HAX_VCPU_GET_REGS, NULL, 0, &state, sizeof(state), &bytes, NULL))
        return;
    save_context_from_state(context, &state);
    context->EFlags &= ~0x200;
}
#include <pshpack1.h>
typedef enum
{
    INT_GATE_TASK = 5,
    INT_GATE_INT16 = 6,
    INT_GATE_TRAP16 = 7,
    INT_GATE_INT32 = 0xE,
    INT_GATE_TRAP32 = 0xF,
} interrupt_gate_type;
typedef struct
{
    WORD offset_low;
    WORD selector;
    BYTE reserved;
    union
    {
        struct
        {
            BYTE type : 4 /* INT_GATE_TASK */, S : 1, DPL : 2, P : 1;
        };
        BYTE data;
    };
    WORD offset_high;
} interrupt_gate;
_STATIC_ASSERT(sizeof(interrupt_gate) == 8);
#include <poppack.h>

#define HAXMVM_STR2(s) #s
#define HAXMVM_STR(s) HAXMVM_STR2(s)
#define HAXMVM_ERR fprintf(stderr, __FUNCTION__ "("  HAXMVM_STR(__LINE__)  ") HAXM err.\n");
#define HAXMVM_ERRF(fmt, ...) fprintf(stderr, __FUNCTION__ "("  HAXMVM_STR(__LINE__)  ") " fmt "\n", __VA_ARGS__);
LPVOID trap_int;
interrupt_gate idt[256];
WORD seg_cs;
WORD seg_ds;
BOOL init_vm86(BOOL vm86)
{
    ((void(*)())GetProcAddress(GetModuleHandleA("libwine"), "set_intel_vt_x_workaround"))();
    __asm
    {
        mov seg_cs, cs
        mov seg_ds, ds
    }
    hSystem = CreateFileW(L"\\\\.\\HAX", 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hSystem == INVALID_HANDLE_VALUE)
    {
        HAXMVM_ERRF("HAXM is not installed.\n");
        return FALSE;
    }
    struct hax_module_version ver;
    DWORD bytes;
    if (!DeviceIoControl(hSystem, HAX_IOCTL_VERSION, NULL, NULL, &ver, sizeof(ver), &bytes, NULL))
    {
        HAXMVM_ERRF("VERSION");
        return FALSE;
    }
    uint32_t vm_id;
    if (!DeviceIoControl(hSystem, HAX_IOCTL_CREATE_VM, NULL, NULL, &vm_id, sizeof(vm_id), &bytes, NULL))
    {
        HAXMVM_ERRF("CREATE_VM");
        return FALSE;
    }
    WCHAR buf[1000];
    swprintf_s(buf, RTL_NUMBER_OF(buf), L"\\\\.\\hax_vm%02d", vm_id);
    hVM = CreateFileW(buf, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hVM == INVALID_HANDLE_VALUE)
    {
        HAXMVM_ERRF("Could not create vm.");
        return FALSE;
    }
    uint32_t vcpu_id;
    struct hax_qemu_version verq;
    /* 3~ enable fast mmio */
    verq.cur_version = 1;
    verq.least_version = 0;
    if (!DeviceIoControl(hVM, HAX_VM_IOCTL_NOTIFY_QEMU_VERSION, &verq, sizeof(verq), NULL, 0, &bytes, NULL))
    {
    }
    vcpu_id = 1;
    if (!DeviceIoControl(hVM, HAX_VM_IOCTL_VCPU_CREATE, &vcpu_id, sizeof(vcpu_id), NULL, 0, &bytes, NULL))
    {
        HAXMVM_ERRF("could not create vcpu.");
        return FALSE;
    }
    swprintf_s(buf, RTL_NUMBER_OF(buf), L"\\\\.\\hax_vm%02d_vcpu%02d", vm_id, vcpu_id);
    hVCPU = CreateFileW(buf, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    struct hax_tunnel_info tunnel_info;
    if (!DeviceIoControl(hVCPU, HAX_VCPU_IOCTL_SETUP_TUNNEL, NULL, 0, &tunnel_info, sizeof(tunnel_info), &bytes, NULL))
    {
        HAXMVM_ERRF("SETUP_TUNNEL");
        return FALSE;
    }
    /* memory mapping */
    struct hax_alloc_ram_info alloc_ram = { 0 };
    struct hax_set_ram_info ram = { 0 };
    MEMORY_BASIC_INFORMATION mbi = { 0 };
    LPVOID mem = VirtualAlloc(NULL, 0x20000, MEM_COMMIT, PAGE_READWRITE);
    trap_int = VirtualAlloc(NULL, 0x10000, MEM_COMMIT, PAGE_READWRITE);
    alloc_ram.size = 0x10000 * 1;// 0xFFFF0000;// (uint64_t)mem2 + 4096 - 0x10000;
    alloc_ram.va = (uint64_t)mem;
    if (!DeviceIoControl(hVM, HAX_VM_IOCTL_ALLOC_RAM, &alloc_ram, sizeof(alloc_ram), NULL, 0, &bytes, NULL))
    {
        HAXMVM_ERRF("ALLOC_RAM");
        return FALSE;
    }
    while (!TRUE)
    {
        alloc_ram.size = mbi.RegionSize;
        VirtualQuery((PVOID)((SIZE_T)mbi.BaseAddress + mbi.RegionSize), &mbi, sizeof(mbi));
        if (!mbi.RegionSize)
            break;
        alloc_ram.va = (SIZE_T)mbi.BaseAddress;
        alloc_ram.size = (SIZE_T)mbi.RegionSize;
        if (!alloc_ram.va)
            continue;
        if (0&&mbi.State != MEM_COMMIT)
        {
            if ((SIZE_T)mbi.BaseAddress + mbi.RegionSize < (SIZE_T)mbi.BaseAddress)
            {
                break;
            }
            continue;
        }
        //MmProbeAndLockPages(xx, xx , IoReadAccess|IoWriteAccess) fails
        if (0 && mbi.State == MEM_COMMIT)
        {
            DWORD old;
            if (mbi.Protect & PAGE_READONLY)
            {
                VirtualProtect(mbi.BaseAddress, mbi.RegionSize, PAGE_READWRITE, &old);
            }
            if ((mbi.Protect & PAGE_EXECUTE_READ) || (mbi.Protect & PAGE_EXECUTE))
            {
                VirtualProtect(mbi.BaseAddress, mbi.RegionSize, PAGE_EXECUTE_READWRITE, &old);
            }
        }
        if (mbi.State == MEM_COMMIT)
        {
            DWORD old;
            if (mbi.Protect & PAGE_READONLY)
            {
                continue;
            }
            if ((mbi.Protect & PAGE_EXECUTE_READ) || (mbi.Protect & PAGE_EXECUTE))
            {
                continue;
            }
        }
        if (!DeviceIoControl(hVM, HAX_VM_IOCTL_ALLOC_RAM, &alloc_ram, sizeof(alloc_ram), NULL, 0, &bytes, NULL))
        {
            HAXMVM_ERR;
        }
        if ((SIZE_T)mbi.BaseAddress + mbi.RegionSize < (SIZE_T)mbi.BaseAddress)
        {
            break;
        }
        ram.pa_start = (SIZE_T)mbi.BaseAddress;
        ram.size = (SIZE_T)mbi.RegionSize;
        ram.va = (SIZE_T)mbi.BaseAddress;
        if (!DeviceIoControl(hVM, HAX_VM_IOCTL_SET_RAM, &ram, sizeof(ram), NULL, 0, &bytes, NULL))
        {
            HAXMVM_ERR;
            return FALSE;
        }
    }
    ram.pa_start = 0;
    ram.size = 0x10000;
    ram.va = (uint64_t)mem;
    if (!DeviceIoControl(hVM, HAX_VM_IOCTL_SET_RAM, &ram, sizeof(ram), NULL, 0, &bytes, NULL))
    {
        HAXMVM_ERRF("SET_RAM");
        return FALSE;
    }
    tunnel = (struct hax_tunnel*)tunnel_info.va;
    struct vcpu_state_t state;
    if (!DeviceIoControl(hVCPU, HAX_VCPU_GET_REGS, NULL, 0, &state, sizeof(state), &bytes, NULL))
    {
        HAXMVM_ERRF("GET_REGS");
        return FALSE;
    }
    /* setup initial states */
    /* FIXME: remove loader */
    char loader[] =
        "\x90\x90\x90\xB8\x00\x00\x8E\xD0\x66\xBC\x00\x10\x00\x00\x66\xB8"
        "\x92\x11\x00\x00\x66\xBB\x29\x11\x00\x00\xFA\x60\x0F\x01\x16\x80"
        "\x02\xFB\x61\x0F\x20\xC0\x66\x83\xC8\x01\x0F\x22\xC0\xEA\x3B\x00"
        "\x08\x00\x90\x90\x90\x90\x90\x90\x90\x90\x90\xB8\x10\x00\x00\x00"
        "\x8E\xD0\x8E\xD8\xBC\x00\x10\x00\x00\xBF\x00\x00\x00\x00\xA1\x00"
        "\x00\x00\x00\x8B\x06\xFA\x66\xB8\x1B\x00\x8E\xD8\x6A\x1F\x68\x00"
        "\x10\x00\x00\x9C\x6A\x27\x68\x74\x00\x00\x00\xF4\xCF\x90\x90\x90"
        "\xF4\x90\x90\xF4\xB8\x1F\x00\x00\x00\x8E\xD8\xC7\x06\x78\x56\x34"
        "\x12\x90\x90\x0F\xA2\x90\x0F\xA2\x90\x90\xF4\x90\x90\x90\x90\x90"
        "\x90\x90\x90\x9A\x82\x11\x00\x00\x23\x00\x66\xB8\x23\x00\x8E\xC0"
        "\x26\xA1\x00\x00\x00\x00\xF4\xCD\x21\xF4\xCD\x03\xF4\x90\x90\x90"
        "\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90"
        "\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90"
        "\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90"
        "\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90"
        "\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90"
        "\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90"
        "\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90"
        "\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90"
        "\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90"
        "\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90"
        "\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90"
        "\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90"
        "\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90"
        "\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90"
        "\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90"
        "\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90"
        "\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90"
        "\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90"
        "\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90"
        "\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90"
        "\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90"
        "\xD9\x28\x0F\xA2\x0F\x01\xC1\x90\x90\x90\x90\x90\x90\x90\x90\x90"
        "\x9B\x90\x0F\xA2\x0F\x01\xC1\x90\x90\x90\x90\x90\x90\x90\x90\x90"
        "\xDB\xE3\x90\x0F\xA2\x0F\x01\xC1\x90\x90\x90\x90\x90\x90\x90\x90"
        "\x9B\xD9\x38\x0F\xA2\x0F\x01\xC1\x90\x90\x90\x90\x90\x90\x90\x90"
        "\xD9\xFC\x0F\xA2\x0F\x01\xC1\x90\x90\x90\x90\x90\x90\x90\x90\x90"
        "\x9B\xDB\xE2\x0F\xA2\x0F\x01\xC1\x90\x90\x90\x90\x90\x90\x90\x90"
        "\x9B\xDD\x30\x0F\xA2\x0F\x01\xC1\x90\x90\x90\x90\x90\x90\x90\x90"
        "\xDD\x20\x0F\xA2\x0F\x01\xC1\x90\x90\x90\x90\x90\x90\x90\x90\x90"
        "\x30\x00\x86\x02\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\xFF\xFF"
        "\x00\x00\x00\x9A\xCF\x00\xFF\xFF\x00\x00\x00\x92\xCF\x00\xFF\xFF"
        "\x00\x00\x00\xF2\xCF\x00\xFF\xFF\x00\x00\x00\xFA\xCF\x00\xFF\xFF"
        "\x00\x00\x00\xF2\xCF\x00";
    SIZE_T ls = sizeof(loader) - 1;
    memcpy(mem, loader, ls);
    state._rip = (SIZE_T)0;
    state._ss.selector = 0;
    state._ss.base = 0;
    state._rsp = 0x1000;// (DWORD)mem;
    state._rip = 0;// (DWORD)mem;
    state._cs.selector = 0;
    state._cs.base = 0;
    state._cs.limit = 0x10000;
    state._ebp = 0x100f;
    int kani;
    state._rsi = 0x1000;// &kani;
    if (!DeviceIoControl(hVCPU, HAX_VCPU_SET_REGS, &state, sizeof(state), NULL, 0, &bytes, NULL))
    {
        HAXMVM_ERRF("SET_REGS");
        return FALSE;
    }
    DeviceIoControl(hVCPU, HAX_VCPU_IOCTL_RUN, NULL, 0, NULL, 0, &bytes, NULL);
    DeviceIoControl(hVCPU, HAX_VCPU_GET_REGS, NULL, 0, &state, sizeof(state), &bytes, NULL);
    state._eflags = 0x2;
    state._ldt.base = state._gdt.base;
    (DeviceIoControl(hVCPU, HAX_VCPU_SET_REGS, &state, sizeof(state), NULL, 0, &bytes, NULL));
    tunnel->request_interrupt_window = TRUE;
    (DeviceIoControl(hVCPU, HAX_VCPU_IOCTL_RUN, 0, 0, NULL, 0, &bytes, NULL));
    DeviceIoControl(hVCPU, HAX_VCPU_GET_REGS, NULL, 0, &state, sizeof(state), &bytes, NULL);
    //state._cr0 |= 1;/* PE */
    size_t kanai = sizeof(LDT_ENTRY);
    size_t kanaai = sizeof(wine_ldt);
    //state._gdt.base = (uint64)&wine_ldt[0];
    //state._gdt.limit = 65536;
    state._ldt.selector = 0x10;
    state._ldt.base = (uint64)&wine_ldt[0];
    state._ldt.limit = 65536;

    state._idt.limit = 0x8 * 256 - 1;
    state._idt.base = (SIZE_T)&idt[0];
    for (int i = 0; i < 256; i++)
    {
        idt[i].DPL = 3;
        idt[i].type = INT_GATE_INT32;
        idt[i].selector = seg_cs;
        idt[i].P = 1;
        idt[i].offset_low = (WORD)trap_int + i;
        idt[i].offset_high = (DWORD)trap_int >> 16;
    }
    tunnel->request_interrupt_window = TRUE;
    if (tunnel->_exit_reason != 0x0a)
    {
        HAXMVM_ERRF("tunnel->_exit_reason != 0x0a");
        return FALSE;
    }
    if (!DeviceIoControl(hVCPU, HAX_VCPU_SET_REGS, &state, sizeof(state), NULL, 0, &bytes, NULL))
    {
        HAXMVM_ERRF("SET_REGS");
        return FALSE;
    }
    return TRUE;
}

BOOL vm_exit()
{
    CloseHandle(hVCPU);
    CloseHandle(hVM);
    CloseHandle(hSystem);
    return TRUE;
}
void PUSH16(struct vcpu_state_t *state, WORD val)
{
    state->_esp -= 2;
    unsigned char *stack = (unsigned char*)(state->_ss.base + state->_esp);
    *(LPWORD)stack = val;
}
void PUSH32(struct vcpu_state_t *state, DWORD val)
{
    state->_esp -= 4;
    unsigned char *stack = (unsigned char*)(state->_ss.base + state->_esp);
    *(LPDWORD)stack = val;
}
WORD POP16(struct vcpu_state_t *state)
{
    LPWORD stack = (LPWORD)(state->_ss.base + state->_esp);
    state->_esp += 2;
    return *stack;
}
DWORD POP32(struct vcpu_state_t *state)
{
    LPDWORD stack = (LPDWORD)(state->_ss.base + state->_esp);
    state->_esp += 4;
    return *stack;
}
WORD PEEK16(struct vcpu_state_t *state, int i)
{
    LPWORD stack = (LPWORD)(state->_ss.base + state->_esp);
    return stack[i];
}
DWORD PEEK32(struct vcpu_state_t *state, int i)
{
    LPDWORD stack = (LPDWORD)(state->_ss.base + state->_esp);
    return stack[i];
}
void relay(LPVOID relay_func, BOOL reg, struct vcpu_state_t *state)
{
    unsigned char *stack1 = (unsigned char*)(state->_ss.base + state->_esp);
    unsigned char *stack = stack1;
    /*
    * (sp+24) word   first 16-bit arg
    * (sp+22) word   cs
    * (sp+20) word   ip
    * (sp+18) word   bp
    * (sp+14) long   32-bit entry point (reused for Win16 mutex recursion count)
    * (sp+12) word   ip of actual entry point (necessary for relay debugging)
    * (sp+8)  long   relay (argument conversion) function entry point
    * (sp+4)  long   cs of 16-bit entry point
    * (sp)    long   ip of 16-bit entry point
    */
    DWORD ip = *(DWORD*)stack;
    stack += sizeof(DWORD);
    DWORD cs = *(DWORD*)stack;
    stack += sizeof(DWORD);
    DWORD relay = *(DWORD*)stack;
    stack += sizeof(DWORD);
    WORD ip2 = *(WORD*)stack;
    stack += sizeof(WORD);
    DWORD entry = *(DWORD*)stack;
    //for debug
    void *entryf = (void*)entry;
    stack += sizeof(DWORD);
    WORD bp = *(WORD*)stack;
    stack += sizeof(WORD);
    WORD ip19 = *(WORD*)stack;
    stack += sizeof(WORD);
    WORD cs16 = *(WORD*)stack;
    stack += sizeof(WORD);
    WORD *args = (WORD*)stack;
    state->_eip = ip;
    load_seg(&state->_cs, (WORD)cs);
#include <pshpack1.h>
    /* 16-bit stack layout after __wine_call_from_16() */
    typedef struct _STACK16FRAME
    {
        struct STACK32FRAME *frame32;        /* 00 32-bit frame from last CallTo16() */
        DWORD         edx;            /* 04 saved registers */
        DWORD         ecx;            /* 08 */
        DWORD         ebp;            /* 0c */
        WORD          ds;             /* 10 */
        WORD          es;             /* 12 */
        WORD          fs;             /* 14 */
        WORD          gs;             /* 16 */
        DWORD         callfrom_ip;    /* 18 callfrom tail IP */
        DWORD         module_cs;      /* 1c module code segment */
        DWORD         relay;          /* 20 relay function address */
        WORD          entry_ip;       /* 22 entry point IP */
        DWORD         entry_point;    /* 26 API entry point to call, reused as mutex count */
        WORD          bp;             /* 2a 16-bit stack frame chain */
        WORD          ip;             /* 2c return address */
        WORD          cs;             /* 2e */
    } STACK16FRAME;
#include <poppack.h>
    CONTEXT context;
    DWORD osp = state->_esp;
    PUSH16(state, state->_gs.selector);
    PUSH16(state, state->_fs.selector);
    PUSH16(state, state->_es.selector);
    PUSH16(state, state->_ds.selector);
    PUSH32(state, state->_ebp);
    PUSH32(state, state->_ecx);
    PUSH32(state, state->_edx);
    PUSH32(state, osp);
    save_context_from_state(&context, state);
    STACK16FRAME *oa = (STACK16FRAME*)wine_ldt_get_ptr((WORD)context.SegSs, context.Esp);
    DWORD ooo = (WORD)context.Esp;
    int fret;
    DWORD bytes;
    /*if (!DeviceIoControl(hVCPU, HAX_VCPU_SET_REGS, state, sizeof(*state), NULL, 0, &bytes, NULL))
        HAXMVM_ERRF("SET_REGS");*/
    /*
    typedef void(*vm_debug_get_entry_point_t)(char *module, char *func, WORD *ordinal);
    static vm_debug_get_entry_point_t vm_debug_get_entry_point;
    if (!vm_debug_get_entry_point)
    {
        vm_debug_get_entry_point = (vm_debug_get_entry_point_t)GetProcAddress(LoadLibraryA(KRNL386), "vm_debug_get_entry_point");
    }
    char module[100], func[100];
    WORD ordinal = 0;
    vm_debug_get_entry_point(module, func, &ordinal);
    fprintf(stderr, "call built-in func %s.%d: %s ESP %04X\n", module, ordinal, func, context.Esp);
    */
    if ((DWORD)relay_func != relay)
    {
        fret = ((int(*)(void *entry_point, unsigned char *args16, CONTEXT *context))relay_func)((void*)entry, (unsigned char*)args, &context);
    }
    else
        fret = ((int(*)(void *, unsigned char *, CONTEXT *))relay)((void*)entry, (unsigned char*)args, &context);
    if (!reg)
    {
        state->_eax = fret;
    }
    oa = (STACK16FRAME*)wine_ldt_get_ptr((WORD)context.SegSs, context.Esp);
    if (reg)
        state->_eax = (DWORD)context.Eax;
    state->_ecx = reg ? (DWORD)context.Ecx : (DWORD)oa->ecx;
    if (reg)
        state->_edx = (DWORD)context.Edx;
    else
        state->_edx = (DWORD)oa->edx;
    state->_ebx = (DWORD)context.Ebx;
    state->_esp = (DWORD)context.Esp;
    state->_ebp = (DWORD)context.Ebp;
    state->_esi = (DWORD)context.Esi;
    state->_edi = (DWORD)context.Edi;
    state->_esp = osp + 18 + 2;
    state->_esp -= (ooo - context.Esp);
    state->_bp = bp;
    set_eflags(state, context.EFlags);
    load_seg(&state->_es, reg ? (WORD)context.SegEs : (WORD)oa->es);
    load_seg(&state->_ss, (WORD)context.SegSs);
    load_seg(&state->_ds, reg ? (WORD)context.SegDs : (WORD)oa->ds);
    load_seg(&state->_fs, (WORD)context.SegFs);
    load_seg(&state->_gs, (WORD)context.SegGs);
    state->_eip = context.Eip;
    load_seg(&state->_cs, (WORD)context.SegCs);
}

BOOL syscall_init = FALSE;
LPBYTE syscall_trap = FALSE;
#define dprintf(...)// printf(__VA_ARGS__)
void vm86main(CONTEXT *context, DWORD cbArgs, PEXCEPTION_HANDLER handler,
    void(*from16_reg)(void),
    LONG(*__wine_call_from_16)(void),
    int(*relay_call_from_16)(void *entry_point, unsigned char *args16, CONTEXT *context),
    void(*__wine_call_to_16_ret)(void),
    int dasm,
    pm_interrupt_handler pih
)
{
    if (!initflag)
    {
        haxmvm_panic("Could not initialize the hypervisor.\nHAXM may not be installed.\n");
    }
    if (tunnel->_exit_status == HAX_EXIT_STATECHANGE)
    {
        haxmvm_panic("hypervisor is panicked!!!");
    }
    /* call cs:relay_call_from_16 => HAX_EXIT_MMIO => switch to win32 */
    /* call cs:relay_call_from_16 => STI instr => Interrupt window VMExit(7) => switch to win32 (faster than above)*/
    if (!syscall_init)
    {
        SIZE_T page1 = (SIZE_T)from16_reg / 4096 * 4096;
        SIZE_T page2 = (SIZE_T)__wine_call_from_16 / 4096 * 4096;
        LPBYTE trap = syscall_trap = (LPBYTE)VirtualAlloc(NULL, 4096, MEM_COMMIT, PAGE_READWRITE);
        memset(trap, 0xFB, 4096); /* STI */
        struct hax_alloc_ram_info alloc_ram = { 0 };
        struct hax_set_ram_info ram = { 0 };
        alloc_ram.size = 4096;
        alloc_ram.va = trap;
        DWORD bytes;
        if (!DeviceIoControl(hVM, HAX_VM_IOCTL_ALLOC_RAM, &alloc_ram, sizeof(alloc_ram), NULL, 0, &bytes, NULL))
        {
            HAXMVM_ERRF("ALLOC_RAM");
        }
        if (page1 != page2)
        {
            ram.pa_start = (SIZE_T)page2;
            ram.size = (SIZE_T)4096;
            ram.va = (SIZE_T)trap;
            if (!DeviceIoControl(hVM, HAX_VM_IOCTL_SET_RAM, &ram, sizeof(ram), NULL, 0, &bytes, NULL))
            {
                HAXMVM_ERRF("SET_RAM\n");
            }
        }
        ram.pa_start = (SIZE_T)page1;
        ram.size = (SIZE_T)4096;
        ram.va = (SIZE_T)trap;
        if (!DeviceIoControl(hVM, HAX_VM_IOCTL_SET_RAM, &ram, sizeof(ram), NULL, 0, &bytes, NULL))
        {
            HAXMVM_ERRF("SET_RAM\n");
        }
        syscall_init = TRUE;
    }
    //is_single_step = TRUE;
    MEMORY_BASIC_INFORMATION mbi;
    DWORD bytes;
    DWORD ret_addr;
    {

        DWORD bytes;
        struct vcpu_state_t state;
        if (!DeviceIoControl(hVCPU, HAX_VCPU_GET_REGS, NULL, 0, &state, sizeof(state), &bytes, NULL))
            HAXMVM_ERRF("GET_REGS");
        load_seg(&state._gs, (WORD)context->SegGs);
        load_seg(&state._fs, (WORD)context->SegFs);
        load_seg(&state._es, (WORD)context->SegEs);
        load_seg(&state._ds, (WORD)context->SegDs);
        load_seg(&state._cs, (WORD)context->SegCs);
        load_seg(&state._ss, (WORD)context->SegSs);

        state._edi = context->Edi;
        state._esi = context->Esi;
        state._ebx = context->Ebx;
        state._edx = context->Edx;
        state._ecx = context->Ecx;
        state._eax = context->Eax;

        state._ebp = context->Ebp;
        state._eip = context->Eip;
        set_eflags(&state, context->EFlags);
        state._esp = context->Esp - cbArgs;

        if (!DeviceIoControl(hVCPU, HAX_VCPU_SET_REGS, &state, sizeof(state), NULL, 0, &bytes, NULL))
            HAXMVM_ERRF("SET_REGS");
        unsigned char *stack = (unsigned char*)state._ss.base + state._esp;
        ret_addr = *(LPDWORD)stack;
    }

    struct hax_alloc_ram_info alloc_ram = { 0 };
    struct hax_set_ram_info ram = { 0 };
    struct vcpu_state_t state2;
    DeviceIoControl(hVCPU, HAX_VCPU_GET_REGS, NULL, 0, &state2, sizeof(state2), &bytes, NULL);
    while (TRUE)
    {
        //DeviceIoControl(hVCPU, HAX_VCPU_GET_REGS, NULL, 0, &state2, sizeof(state2), &bytes, NULL);
        dprintf("%04X:%04X(base:%04llX) ESP:%08X F:%08X\n", state2._cs.selector, state2._eip, state2._cs.base, state2._esp, state2._eflags);
        if (state2._cs.selector == (ret_addr >> 16) && state2._eip == (ret_addr & 0xFFFF))
        {
            state2._eflags &= ~0x10000;
            if (!DeviceIoControl(hVCPU, HAX_VCPU_SET_REGS, &state2, sizeof(state2), NULL, 0, &bytes, NULL))
            {
                HAXMVM_ERRF("SET_REGS");
            }
            break;
        }
        if (is_single_step)
        {
            /* Debug exception */
            fprintf(stderr, "%04x:%04x EAX:%04x EDX:%04x EF:%04x %p\n", state2._cs.selector, state2._eip,
                state2._eax, state2._edx, state2._eflags, (LPBYTE)state2._cs.base + state2._eip);
        }
        if (!DeviceIoControl(hVCPU, HAX_VCPU_IOCTL_RUN, NULL, 0, NULL, 0, &bytes, NULL))
            return;
        DeviceIoControl(hVCPU, HAX_VCPU_GET_REGS, NULL, 0, &state2, sizeof(state2), &bytes, NULL);
        dprintf("%04X:%04X(base:%04llX) ESP:%08X F:%08X\n", state2._cs.selector, state2._eip, state2._cs.base, state2._esp, state2._eflags);
        if (state2._cs.selector == (ret_addr >> 16) && state2._eip == (ret_addr & 0xFFFF))
        {
            state2._eflags &= ~0x10000;
            if (!DeviceIoControl(hVCPU, HAX_VCPU_SET_REGS, &state2, sizeof(state2), NULL, 0, &bytes, NULL))
            {
                HAXMVM_ERRF("SET_REGS");
            }
            break;
        }
        if (tunnel->_exit_status == HAX_EXIT_INTERRUPT)
        {
            LPVOID ptr = (LPBYTE)state2._cs.base + state2._eip;
            LPVOID stack = (LPBYTE)state2._ss.base + state2._esp;
            LPBYTE bstack = (LPBYTE)stack;
            if (tunnel->_exit_reason == EXIT_INTERRUPT_WIN)
            {
                LPBYTE ptr2 = (LPBYTE)ptr - 2;
                BOOL is_reg = ptr2 == from16_reg;
                if (is_reg || ptr2 == __wine_call_from_16)
                {
                    state2._eflags &= ~0x10200;
                    tunnel->_exit_reason = 0;
                    relay(relay_call_from_16, is_reg, &state2);
                    state2._eflags &= ~0x10200;
                    if (!DeviceIoControl(hVCPU, HAX_VCPU_SET_REGS, &state2, sizeof(state2), NULL, 0, &bytes, NULL))
                    {
                        HAXMVM_ERRF("SET_REGS");
                    }
                    continue;
                }
                HAXMVM_ERRF("%04X:%04X(base:%04llX) ESP:%08X F:%08X", state2._cs.selector, state2._eip, state2._cs.base, state2._esp, state2._eflags);
                HAXMVM_ERRF("tunnel->_exit_reason == EXIT_INTERRUPT_WIN");
                haxmvm_panic("tunnel->_exit_reason == EXIT_INTERRUPT_WIN %04X:%04X(base:%04llX) ESP:%08X F:%08X", state2._cs.selector, state2._eip, state2._cs.base, state2._esp, state2._eflags);
            }
            state2._eflags &= ~0x10000;
            LPBYTE byte = (LPBYTE)ptr;
            if (!DeviceIoControl(hVCPU, HAX_VCPU_SET_REGS, &state2, sizeof(state2), NULL, 0, &bytes, NULL))
            {
                HAXMVM_ERRF("SET_REGS");
            }
            continue;
        }
        if (tunnel->_exit_status == HAX_EXIT_STATECHANGE)
        {
            HAXMVM_ERRF("%04X:%04X(base:%04llX) ESP:%08X", state2._cs.selector, state2._eip, state2._cs.base, state2._esp);
            HAXMVM_ERRF("hypervisor is panicked!!!");
            haxmvm_panic("hypervisor is panicked!!!");
        }
        cont:
        //MmProbeAndLockPages(xx, xx , IoReadAccess|IoWriteAccess) fails => HAX_EXIT_STATECHANGE
        if (state2._cs.selector == seg_cs && tunnel->_exit_status == HAX_EXIT_MMIO)
        {
            LPVOID ptr = (LPBYTE)state2._cs.base + state2._eip;
            state2._eflags &= ~0x10000;
            BOOL is_reg = ptr == from16_reg;
            if (is_reg || ptr == __wine_call_from_16)
            {
                relay(relay_call_from_16, is_reg, &state2);
                if (!DeviceIoControl(hVCPU, HAX_VCPU_SET_REGS, &state2, sizeof(state2), NULL, 0, &bytes, NULL))
                {
                    HAXMVM_ERRF("SET_REGS");
                }
                continue;
            }
            else if (ptr >= trap_int && (SIZE_T)ptr <= (SIZE_T)trap_int + 255)
            {
                //DWORD errorcode = POP32(&state2);
                DWORD eip = POP32(&state2);
                DWORD cs = POP32(&state2);
                DWORD eflags = POP32(&state2);
                load_seg(&state2._cs, (WORD)cs);
                state2._eip = eip;
                state2._eflags = eflags & ~0x10000;
                BYTE num = (SIZE_T)ptr - (SIZE_T)trap_int;
                if (is_single_step && num == 1)
                {
                    /* Debug exception */
                    if (!DeviceIoControl(hVCPU, HAX_VCPU_SET_REGS, &state2, sizeof(state2), NULL, 0, &bytes, NULL))
                    {
                        HAXMVM_ERRF("SET_REGS");
                    }
                    if (state2._cs.selector == seg_cs)
                    {
                        goto cont;
                    }
                    continue;
                }
                PUSH16(&state2, (WORD)eflags);
                PUSH16(&state2, (WORD)cs);
                PUSH16(&state2, (WORD)eip);
                CONTEXT ctx;
                save_context_from_state(&ctx, &state2);
                if (num != 0x10 &&  num < 32)
                {
                    /* fixme */
                    HAXMVM_ERRF("int %02Xh handler is not implemented yet.", num);
                    haxmvm_panic("int %02Xh handler is not implemented yet.", num);
                }
                if (!DeviceIoControl(hVCPU, HAX_VCPU_SET_REGS, &state2, sizeof(state2), NULL, 0, &bytes, NULL))
                {
                    HAXMVM_ERRF("SET_REGS");
                }
                dynamic__wine_call_int_handler(&ctx, num);
                load_context_to_state(&ctx, &state2);
                eip = POP16(&state2);
                cs = POP16(&state2);
                eflags = POP16(&state2);
                //load_seg(&state2._cs, (WORD)cs);
                //state2._eip = eip;
                //state2._eflags = eflags & ~0x10000;
                if (!DeviceIoControl(hVCPU, HAX_VCPU_SET_REGS, &state2, sizeof(state2), NULL, 0, &bytes, NULL))
                {
                    HAXMVM_ERRF("SET_REGS");
                }
                dprintf("int\n");
                continue;
            }
            else
            {
                HAXMVM_ERRF("??? %04x:%04x", state2._cs, state2._eip);
            }
        }

        if (FALSE && state2._cs.selector != seg_cs && tunnel->_exit_status == HAX_EXIT_MMIO)
        {
            SIZE_T addr = tunnel->mmio.gla / 4096 * 4096;
            alloc_ram.va = addr;
            alloc_ram.size = 4096;
            if (!DeviceIoControl(hVM, HAX_VM_IOCTL_ALLOC_RAM, &alloc_ram, sizeof(alloc_ram), NULL, 0, &bytes, NULL))
            {
                HAXMVM_ERRF("ALLOC_RAM");
            }
            ram.pa_start = (SIZE_T)addr;
            ram.size = (SIZE_T)4096;
            ram.va = (SIZE_T)addr;
            if (!DeviceIoControl(hVM, HAX_VM_IOCTL_SET_RAM, &ram, sizeof(ram), NULL, 0, &bytes, NULL))
            {
                HAXMVM_ERRF("SET_RAM\n");
            }
            state2._eflags &= ~0x10000;
            if (!DeviceIoControl(hVCPU, HAX_VCPU_SET_REGS, &state2, sizeof(state2), NULL, 0, &bytes, NULL))
            {
                HAXMVM_ERRF("SET_REGS");
            }
            continue;
        }
        if (TRUE && state2._cs.selector != seg_cs && tunnel->_exit_status == HAX_EXIT_MMIO)
        {
            // printf("HAXMMMIO %04X:%04X(base:%04llX) ESP:%08X F:%08X\n", state2._cs.selector, state2._eip, state2._cs.base, state2._esp, state2._eflags);
            VirtualQuery(tunnel->mmio.gla, &mbi, sizeof(mbi));
            while (mbi.RegionSize)
            {
                alloc_ram.va = (SIZE_T)mbi.BaseAddress;
                alloc_ram.size = (SIZE_T)mbi.RegionSize;
                if (!DeviceIoControl(hVM, HAX_VM_IOCTL_ALLOC_RAM, &alloc_ram, sizeof(alloc_ram), NULL, 0, &bytes, NULL))
                {
                    mbi.RegionSize -= 4096;
                    continue;
                }
                break;
            }
            dprintf("HAXM MMIO %llx %p - %p size=%p ab:%p\n", tunnel->mmio.gla, mbi.BaseAddress, (SIZE_T)mbi.BaseAddress + mbi.RegionSize, mbi.RegionSize, mbi.AllocationBase);

            if (tunnel->mmio.gla > alloc_ram.va + alloc_ram.size)
            {
                HAXMVM_ERRF("(tunnel->mmio.gla > alloc_ram.va + alloc_ram.size).");
            }
            if (!mbi.RegionSize)
            {
                HAXMVM_ERRF("!mbi.RegionSize");
            }
            ram.pa_start = (SIZE_T)mbi.BaseAddress;
            ram.size = (SIZE_T)mbi.RegionSize;
            ram.va = (SIZE_T)mbi.BaseAddress;
            if (!DeviceIoControl(hVM, HAX_VM_IOCTL_SET_RAM, &ram, sizeof(ram), NULL, 0, &bytes, NULL))
            {
                HAXMVM_ERRF("SET_RAM");
            }
            state2._eflags &= ~0x10000;
            if (!DeviceIoControl(hVCPU, HAX_VCPU_SET_REGS, &state2, sizeof(state2), NULL, 0, &bytes, NULL))
            {
                HAXMVM_ERRF("SET_REGS");
            }
            continue;
        }
        HAXMVM_ERRF("%04X:%04X(base:%04llX) ESP:%08X", state2._cs.selector, state2._eip, state2._cs.base, state2._esp);
        HAXMVM_ERRF("unknown status %d (reason: %d)!!!", tunnel->_exit_status, tunnel->_exit_reason);
        haxmvm_panic("hypervisor is panicked!!!");
    }
    save_context(context);
}

__declspec(dllexport) DWORD wine_call_to_16_vm86(DWORD target, DWORD cbArgs, PEXCEPTION_HANDLER handler,
    void(*from16_reg)(void),
    LONG(*__wine_call_from_16)(void),
    int(*relay_call_from_16)(void *entry_point, unsigned char *args16, CONTEXT *context),
    void(*__wine_call_to_16_ret)(void),
    int dasm,
    BOOL vm86,
    void *memory_base,
    pm_interrupt_handler pih)
{

    mem = vm86 ? (UINT8*)memory_base : NULL;
    if (!initflag)
        initflag = init_vm86(vm86);
    CONTEXT context;
    PVOID oldstack = dynamic_getWOW32Reserved();
    save_context(&context);
    //why??
    dynamic_setWOW32Reserved(oldstack);
    context.SegSs = ((size_t)dynamic_getWOW32Reserved() >> 16) & 0xFFFF;
    context.Esp = ((size_t)dynamic_getWOW32Reserved()) & 0xFFFF;
    context.SegCs = target >> 16;
    context.Eip = target & 0xFFFF;//i386_jmp_far(target >> 16, target & 0xFFFF);
    vm86main(&context, cbArgs, handler, from16_reg, __wine_call_from_16, relay_call_from_16, __wine_call_to_16_ret, dasm, pih);
    return context.Eax | context.Edx << 16;
}
__declspec(dllexport) void wine_call_to_16_regs_vm86(CONTEXT *context, DWORD cbArgs, PEXCEPTION_HANDLER handler,
    void(*from16_reg)(void),
    LONG(*__wine_call_from_16)(void),
    int(*relay_call_from_16)(void *entry_point, unsigned char *args16, CONTEXT *context),
    void(*__wine_call_to_16_ret)(void),
    int dasm,
    BOOL vm86,
    void *memory_base,
    pm_interrupt_handler pih
)
{
    mem = vm86 ? (UINT8*)memory_base : NULL;
    if (!initflag)
        initflag = init_vm86(vm86);
    vm86main(context, cbArgs, handler, from16_reg, __wine_call_from_16, relay_call_from_16, __wine_call_to_16_ret, dasm, pih);


}

SIZE_T base = 0;
SIZE_T x87func = 0x200 - 0x10;
void callx87(SIZE_T addr)
{
    DWORD bytes;
    struct vcpu_state_t state;
    DeviceIoControl(hVCPU, HAX_VCPU_GET_REGS, NULL, 0, &state, sizeof(state), &bytes, NULL);
    state._rip = addr;
    state._cs.selector = seg_cs;
    state._cs.base = 0;
    DeviceIoControl(hVCPU, HAX_VCPU_SET_REGS, &state, sizeof(state), NULL, 0, &bytes, NULL);
    if (!DeviceIoControl(hVCPU, HAX_VCPU_IOCTL_RUN, NULL, 0, NULL, 0, &bytes, NULL))
        return;
    //DeviceIoControl(hVCPU, HAX_VCPU_GET_REGS, NULL, 0, &state, sizeof(state), &bytes, NULL);

    if (tunnel->_exit_status == HAX_EXIT_STATECHANGE)
    {
        HAXMVM_ERRF("hypervisor is panicked!!!");
        haxmvm_panic("win87em: hypervisor is panicked!!!");
    }
}
/* x87 service functions */
void fldcw(WORD a)
{
    SIZE_T location = base + x87func + 1 * 0x10;
    callx87(location);
}
void wait()
{
    SIZE_T location = base + x87func + 2 * 0x10;
    callx87(location);
}
void fninit()
{
    SIZE_T location = base + x87func + 3 * 0x10;
    callx87(location);
}
void fstcw(WORD* a)
{
    SIZE_T location = base + x87func + 3 * 0x10;
    callx87(location);
}
void frndint()
{
    SIZE_T location = base + x87func + 3 * 0x10;
    callx87(location);
}
void fclex()
{
    SIZE_T location = base + x87func + 3 * 0x10;
    callx87(location);
}
void fsave(char* a)
{
    SIZE_T location = base + x87func + 3 * 0x10;
    callx87(location);
}
void frstor(const char* a)
{
    SIZE_T location = base + x87func + 3 * 0x10;
    callx87(location);
}
typedef void(*fldcw_t)(WORD);
typedef void(*wait_t)();
typedef void(*fninit_t)();
typedef void(*fstcw_t)(WORD*);
typedef void(*frndint_t)();
typedef void(*fclex_t)();
typedef void(*fsave_t)(char*);
typedef void(*frstor_t)(const char*);
typedef struct
{
    fldcw_t fldcw;
    wait_t wait;
    fninit_t fninit;
    fstcw_t fstcw;
    frndint_t frndint;
    fclex_t fclex;
    fsave_t fsave;
    frstor_t frstor;
} x87function;
__declspec(dllexport) void load_x87function(x87function *func)
{
    func->fclex = fclex;
    func->fldcw = fldcw;
    func->fninit = fninit;
    func->frndint = frndint;
    func->frstor = frstor;
    func->fsave = fsave;
    func->fstcw = fstcw;
    func->wait = wait;
}