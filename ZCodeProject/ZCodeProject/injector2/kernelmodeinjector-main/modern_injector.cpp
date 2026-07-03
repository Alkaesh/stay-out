// =============================================================================
//  modern_injector.cpp - Kernel-mode manual-map DLL injector.
//
//  Rewritten to use only EXPORTED ntoskrnl routines so it links under the WDK.
//
//  Injection method (kernel-side):
//    1. Read the target .dll from disk (raw PE bytes, NO XOR).
//    2. KeStackAttachProcess(target) -> ZwAllocateVirtualMemory(SizeOfImage).
//    3. Map headers + sections, zero-fill the rest of each section.
//    4. Apply x64 base relocations (IMAGE_REL_BASED_DIR64 / HIGHLOW / ABSOLUTE).
//    5. Set per-section page protection via ZwProtectVirtualMemory.
//    6. Resolve kernel32!LoadLibraryA + kernel32!GetProcAddress by walking the
//       target's PEB loaded-module list (ZwQueryInformationProcess).
//    7. Write a position-independent "loader stub" into the target. The stub
//       walks the DLL's import directory, fills the IAT using those two
//       kernel32 functions, then calls DllMain(base, DLL_PROCESS_ATTACH, NULL)
//       and finally kernel32!ExitThread(0).
//    8. RtlCreateUserThread in the target to run the stub.
//
//  IOCTL contract (unchanged, matches loader.exe):
//    IOCTL_INJECT_DLL, METHOD_BUFFERED:
//      struct { ULONG TargetPid; WCHAR DllPath[260]; }
//
//  DEBUGGING NOTES (assumptions that need verification on first real load):
//    - PEB field offsets assume x64 (PEB->Ldr at +0x18, Ldr->InMemoryOrderModuleList
//      at +0x20). Correct for x64 Win10/11. NOT portable to x86.
//    - RtlCreateUserThread is exported but UNDOCUMENTED; its signature is
//      stable in practice but not guaranteed. Signature used here matches the
//      widely-documented form (see Stormshield writeup).
//    - We do NOT register the DLL in PEB Ldr lists (true manual map). CRT that
//      enumerates loaded modules will not see it; stayout_dll resolves Python
//      via GetProcAddress scans, so this is fine for it.
// =============================================================================

#include <ntifs.h>
#include <ntddk.h>        // PROCESS_BASIC_INFORMATION, ProcessBasicInformation enum
#include <ntimage.h>
#include <ntstrsafe.h>
#include <stddef.h>      // offsetof (kernel build pulls CRT headers from km/crt)

// ZwQueryInformationProcess and RtlCreateUserThread ARE exported by ntoskrnl
// (verified via dumpbin) but are not declared in the WDK kernel headers
// (ZwQueryInformationProcess lives in winternl.h for user mode). Declare them
// here so we can call/export-resolve them.
extern "C" {
NTSYSAPI NTSTATUS NTAPI ZwQueryInformationProcess(
    _In_      HANDLE ProcessHandle,
    _In_      PROCESSINFOCLASS ProcessInformationClass,
    _Out_     PVOID ProcessInformation,
    _In_      ULONG ProcessInformationLength,
    _Out_opt_ PULONG ReturnLength);

NTSYSAPI NTSTATUS NTAPI RtlCreateUserThread(
    _In_     HANDLE ProcessHandle,
    _In_opt_ PSECURITY_DESCRIPTOR SecurityDescriptor,
    _In_     BOOLEAN CreateSuspended,
    _In_     ULONG StackZeroBits,
    _In_opt_ PULONG StackReserved,
    _In_opt_ PULONG StackCommit,
    _In_     PVOID StartAddress,
    _In_opt_ PVOID StartParameter,
    _Out_    PHANDLE ThreadHandle,
    _Out_    PCLIENT_ID ClientId);

// ZwProtectVirtualMemory is exported by ntoskrnl but not declared in the
// kernel headers shipped with the WDK (it lives in winbase.h for user mode).
NTSYSAPI NTSTATUS NTAPI ZwProtectVirtualMemory(
    _In_    HANDLE ProcessHandle,
    _Inout_ PVOID* BaseAddress,
    _Inout_ PSIZE_T RegionSize,
    _In_    ULONG NewProtect,
    _Out_   PULONG OldProtect);
}

// -----------------------------------------------------------------------------
//  Config / tags / IOCTL
// -----------------------------------------------------------------------------
#define DEVICE_NAME    L"\\Device\\ModernInjector"
#define SYMLINK_NAME   L"\\DosDevices\\ModernInjector"
#define IOCTL_INJECT_DLL CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define POOL_TAG        'XLLD'   // 'DLLX' reversed
#define STUB_POOL_TAG   'BUTS'   // 'STUB'

struct InjectionRequest {
    ULONG  TargetPid;
    WCHAR  DllPath[260];
};

// kernel32 functions the loader stub needs. Resolved from the target's PEB.
struct Kernel32Api {
    PVOID LoadLibraryA;     // HMODULE (LPCSTR)
    PVOID GetProcAddress;   // FARPROC (HMODULE, LPCSTR)
    PVOID ExitThread;       // void (DWORD)  ¡ª exit the remote thread cleanly
};

// Data block copied next to the stub; the stub reads its parameters from here.
// FIELD OFFSETS ARE HARDCODED IN THE SHELLCODE BELOW ¡ª change both together.
struct StubData {
    PVOID   LoadLibraryA;       // +0x00
    PVOID   GetProcAddress;     // +0x08
    PVOID   ExitThread;         // +0x10
    PVOID   ImageBase;          // +0x18  where the DLL was mapped
    ULONG   SizeOfImage;        // +0x20
    ULONG   ImportDirRva;       // +0x24  IMAGE_DIRECTORY_ENTRY_IMPORT
    ULONG   ImportDirSize;      // +0x28
    ULONG   EntryPointRva;      // +0x2C  AddressOfEntryPoint
};
static_assert(offsetof(StubData, LoadLibraryA)  == 0x00, "shellcode offset");
static_assert(offsetof(StubData, GetProcAddress)== 0x08, "shellcode offset");
static_assert(offsetof(StubData, ExitThread)    == 0x10, "shellcode offset");
static_assert(offsetof(StubData, ImageBase)     == 0x18, "shellcode offset");
static_assert(offsetof(StubData, ImportDirRva)  == 0x24, "shellcode offset");
static_assert(offsetof(StubData, EntryPointRva) == 0x2C, "shellcode offset");

// =============================================================================
//  Loader stub (position-independent shellcode).
//
//  MSVC does NOT support inline __asm for x64 targets, and hand-encoding rel8
//  jumps in a byte array is too error-prone to maintain untested. So the stub
//  bytes live in loader_stub.asm (assembled by MASM, which resolves all branch
//  displacements). The driver copies those bytes into a RWX page in the target
//  and runs them as the start address of a remote thread (RtlCreateUserThread),
//  with &StubData passed as the thread's StartParameter (-> rcx under Win64).
//
//  What the stub does (see StubData offsets above):
//    rbp = rcx = StubData*
//    1. rsi = ImageBase + ImportDirRva            ; IMAGE_IMPORT_DESCRIPTOR*
//    2. loop imports:
//         if desc->Name == 0 -> done
//         LoadLibraryA(ImageBase + desc->Name) -> r13 (module handle)
//         r14 = ImageBase + (OriginalFirstThunk ?: FirstThunk)   ; INT
//         r15 = ImageBase + FirstThunk                            ; IAT
//         loop thunks:
//           t = *r14 ; if 0 -> next import
//           if t & 0x8000000000000000:  by ordinal  -> GetProcAddress(mod, t)
//           else: GetProcAddress(mod, ImageBase + t + 2)         ; name
//           *r15 = result
//           r14 += 8 ; r15 += 8
//         rsi += 20
//    3. DllMain(ImageBase, DLL_PROCESS_ATTACH=1, NULL)  ; rax = entrypoint
//    4. ExitThread(0)
//
//  Layout of StubData offsets (see struct above):
//    +0x00 LoadLibraryA   +0x08 GetProcAddress  +0x10 ExitThread
//    +0x18 ImageBase      +0x2C EntryPointRva   +0x24 ImportDirRva
// =============================================================================
extern "C" {
    extern UCHAR LoaderStubStart[];   // beginning of the stub procedure
    extern UCHAR LoaderStubEnd[];     // one-past-end marker (see .asm)
}

// =============================================================================
//  RAII-ish helpers.
// =============================================================================
struct HandleCloser {
    HANDLE handle;
    explicit HandleCloser(HANDLE h) : handle(h) {}
    ~HandleCloser() { if (handle) ZwClose(handle); }
};

struct PoolFreer {
    PVOID  ptr;
    explicit PoolFreer(PVOID p) : ptr(p) {}
    ~PoolFreer() { if (ptr) ExFreePoolWithTag(ptr, POOL_TAG); }
    PVOID release() { PVOID p = ptr; ptr = nullptr; return p; }
};

// =============================================================================
//  ReadFileIntoPool - open dllPath, read whole file into a PagedPool buffer.
//  Returns STATUS_SUCCESS and *OutBuffer / *OutSize on success.
// =============================================================================
static NTSTATUS ReadFileIntoPool(PCUNICODE_STRING path, PUCHAR* OutBuffer, PSIZE_T OutSize) {
    *OutBuffer = nullptr;
    *OutSize = 0;

    OBJECT_ATTRIBUTES objAttr;
    InitializeObjectAttributes(&objAttr, const_cast<PUNICODE_STRING>(path),
        OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, nullptr, nullptr);

    IO_STATUS_BLOCK io{};
    HANDLE hFile = nullptr;
    NTSTATUS status = ZwCreateFile(&hFile, GENERIC_READ | SYNCHRONIZE, &objAttr, &io,
        nullptr, FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ, FILE_OPEN,
        FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE, nullptr, 0);
    if (!NT_SUCCESS(status)) return status;
    HandleCloser fc(hFile);

    FILE_STANDARD_INFORMATION info{};
    status = ZwQueryInformationFile(hFile, &io, &info, sizeof(info), FileStandardInformation);
    if (!NT_SUCCESS(status)) return status;
    if (info.EndOfFile.QuadPart <= 0 || info.EndOfFile.QuadPart > (16 * 1024 * 1024)) {
        return STATUS_INVALID_IMAGE_FORMAT;   // sanity: DLL < 16 MB
    }

    SIZE_T size = static_cast<SIZE_T>(info.EndOfFile.QuadPart);
    PUCHAR buf = static_cast<PUCHAR>(ExAllocatePool2(POOL_FLAG_PAGED, size, POOL_TAG));
    if (!buf) return STATUS_INSUFFICIENT_RESOURCES;

    status = ZwReadFile(hFile, nullptr, nullptr, nullptr, &io, buf,
        static_cast<ULONG>(size), nullptr, nullptr);
    if (!NT_SUCCESS(status)) {
        ExFreePoolWithTag(buf, POOL_TAG);
        return status;
    }

    *OutBuffer = buf;
    *OutSize = size;
    return STATUS_SUCCESS;
}

// =============================================================================
//  ApplyRelocations - fix absolute addresses for the load delta (x64).
//  imageBase = where actually mapped, preferred = OptionalHeader.ImageBase.
// =============================================================================
static NTSTATUS ApplyRelocations(PUCHAR imageBase, PIMAGE_NT_HEADERS nt, ULONG_PTR preferred) {
    ULONG relocSize = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].Size;
    ULONG relocRva  = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].VirtualAddress;
    if (!relocSize || !relocRva) return STATUS_SUCCESS;

    const LONG_PTR delta = reinterpret_cast<LONG_PTR>(imageBase) - static_cast<LONG_PTR>(preferred);
    if (delta == 0) return STATUS_SUCCESS;

    auto* reloc = reinterpret_cast<PIMAGE_BASE_RELOCATION>(imageBase + relocRva);
    PUCHAR relocEnd = reinterpret_cast<PUCHAR>(reloc) + relocSize;

    while (reinterpret_cast<PUCHAR>(reloc) < relocEnd && reloc->VirtualAddress) {
        PUCHAR blockBase = imageBase + reloc->VirtualAddress;
        ULONG  count = (reloc->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(USHORT);
        PUSHORT entries = reinterpret_cast<PUSHORT>(reloc + 1);

        for (ULONG i = 0; i < count; i++) {
            USHORT type = entries[i] >> 12;
            USHORT off  = entries[i] & 0x0FFF;
            PVOID  target = blockBase + off;

            switch (type) {
                case IMAGE_REL_BASED_DIR64: {
                    *reinterpret_cast<PULONG_PTR>(target) += delta;
                    break;
                }
                case IMAGE_REL_BASED_HIGHLOW: {
                    *reinterpret_cast<PULONG>(target) += static_cast<ULONG>(delta);
                    break;
                }
                case IMAGE_REL_BASED_ABSOLUTE:
                    break;   // padding, skip
                default:
                    // Unsupported reloc type; ignore (rare for normal DLLs).
                    break;
            }
        }
        reloc = reinterpret_cast<PIMAGE_BASE_RELOCATION>(
            reinterpret_cast<PUCHAR>(reloc) + reloc->SizeOfBlock);
    }
    return STATUS_SUCCESS;
}

// =============================================================================
//  ResolveKernel32Api - find LoadLibraryA / GetProcAddress / ExitThread in the
//  TARGET process by walking its PEB loaded-module list.
//
//  MUST be called while attached to the target (KeStackAttachProcess) because we
//  read target user-mode memory directly.
//
//  x64 PEB layout (Win10/11):  PEB->Ldr is at +0x18 (PEB_LDR_DATA*).
//  PEB_LDR_DATA->InMemoryOrderModuleList is a LIST_ENTRY at +0x20; each entry is
//  an LDR_DATA_TABLE_ENTRY whose InMemoryOrderLinks is the list node. The module
//  base is at a fixed offset within that struct (we read the export directory
//  and match by export name).
//
//  To avoid hardcoding brittle struct offsets, we look for the module whose name
//  contains "kernel32.dll" (case-insensitive, by Basename) ¡ª read via the
//  FullDllName/Buffer pointer chain. The field offsets used below are the stable
//  x64 ones; they are documented inline for debugging.
// =============================================================================
static NTSTATUS ResolveKernel32Api(PEPROCESS target, Kernel32Api* out) {
    out->LoadLibraryA = nullptr;
    out->GetProcAddress = nullptr;
    out->ExitThread = nullptr;

    // Get target PEB via ZwQueryInformationProcess (documented PROCESSINFOCLASS).
    PROCESS_BASIC_INFORMATION pbi{};
    ULONG retLen = 0;
    NTSTATUS status = ZwQueryInformationProcess(
        NtCurrentProcess(), ProcessBasicInformation, &pbi, sizeof(pbi), &retLen);
    if (!NT_SUCCESS(status) || !pbi.PebBaseAddress) return STATUS_INVALID_PARAMETER;

    // x64: PEB->Ldr (PEB_LDR_DATA*) at +0x18.
    const ULONG_PTR peb = reinterpret_cast<ULONG_PTR>(pbi.PebBaseAddress);
    PVOID ldr = reinterpret_cast<PVOID>(
        *reinterpret_cast<PULONG_PTR>(peb + 0x18));
    if (!ldr) return STATUS_INVALID_PARAMETER;

    // PEB_LDR_DATA->InMemoryOrderModuleList (LIST_ENTRY) at +0x20.
    LIST_ENTRY* head = reinterpret_cast<LIST_ENTRY*>(
        reinterpret_cast<PUCHAR>(ldr) + 0x20);
    LIST_ENTRY* cur = head->Flink;
    if (!cur) return STATUS_INVALID_PARAMETER;

    PVOID k32Base = nullptr;

    // LDR_DATA_TABLE_ENTRY (x64) layout, offsets used:
    //   +0x20  InMemoryOrderLinks  (the list node we walk)
    //   +0x30  DllBase
    //   +0x38  EntryPoint
    //   +0x40  SizeOfImage
    //   +0x48  FullDllName (UNICODE_STRING: +0 len, +2 maxlen, +8 buffer ptr)
    //   +0x58  BaseDllName
    for (ULONG guard = 0; cur != head && guard < 4096; cur = cur->Flink, guard++) {
        PUCHAR entry = reinterpret_cast<PUCHAR>(cur);
        PVOID  dllBase = *reinterpret_cast<PVOID*>(entry + 0x30);
        if (!dllBase) continue;

        // BaseDllName buffer ptr (UNICODE_STRING at +0x58, Buffer at +0x58+8).
        UNICODE_STRING baseName;
        baseName.Length        = *reinterpret_cast<PUSHORT>(entry + 0x58);
        baseName.MaximumLength = *reinterpret_cast<PUSHORT>(entry + 0x58 + 2);
        baseName.Buffer        = *reinterpret_cast<PWCH*>(entry + 0x58 + 8);
        if (!baseName.Buffer || baseName.Length < 12) continue;

        // Case-insensitive endsWith "kernel32.dll" (12 chars).
        if (baseName.Length < 24) continue;   // need at least "kernel32.dll" (24 bytes wide)
        PWCH tail = baseName.Buffer + (baseName.Length / sizeof(WCHAR)) - 12;
        if (_wcsnicmp(tail, L"kernel32.dll", 12) != 0) continue;

        k32Base = dllBase;
        break;
    }
    if (!k32Base) return STATUS_DLL_NOT_FOUND;

    // Helper: resolve one export by name from a mapped module (read from target
    // memory directly since we're attached). Pure function of the PE on disk.
    auto resolveByName = [](PVOID moduleBase, const char* funcName) -> PVOID {
        auto* dos = reinterpret_cast<PIMAGE_DOS_HEADER>(moduleBase);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
        auto* nt = reinterpret_cast<PIMAGE_NT_HEADERS>(
            reinterpret_cast<PUCHAR>(moduleBase) + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;

        ULONG edRva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
        ULONG edSize = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].Size;
        if (!edRva || !edSize) return nullptr;

        auto* exp = reinterpret_cast<PIMAGE_EXPORT_DIRECTORY>(
            reinterpret_cast<PUCHAR>(moduleBase) + edRva);
        PULONG  addrTable = reinterpret_cast<PULONG>(
            reinterpret_cast<PUCHAR>(moduleBase) + exp->AddressOfFunctions);
        PUSHORT nameOrd   = reinterpret_cast<PUSHORT>(
            reinterpret_cast<PUCHAR>(moduleBase) + exp->AddressOfNameOrdinals);
        PULONG  nameTable = reinterpret_cast<PULONG>(
            reinterpret_cast<PUCHAR>(moduleBase) + exp->AddressOfNames);

        for (ULONG i = 0; i < exp->NumberOfNames; i++) {
            const char* name = reinterpret_cast<const char*>(
                reinterpret_cast<PUCHAR>(moduleBase) + nameTable[i]);
            if (strcmp(name, funcName) != 0) continue;

            ULONG funcRva = addrTable[nameOrd[i]];
            if (!funcRva) return nullptr;

            // Detect forwarders (RVA falls inside the export dir itself).
            if (funcRva >= edRva && funcRva < edRva + edSize) return nullptr; // forwarded, skip

            return reinterpret_cast<PVOID>(
                reinterpret_cast<PUCHAR>(moduleBase) + funcRva);
        }
        return nullptr;
    };

    out->LoadLibraryA  = resolveByName(k32Base, "LoadLibraryA");
    out->GetProcAddress = resolveByName(k32Base, "GetProcAddress");
    out->ExitThread    = resolveByName(k32Base, "ExitThread");

    if (!out->LoadLibraryA || !out->GetProcAddress || !out->ExitThread) {
        DbgPrint("[inj] kernel32 resolve partial: LL=%p GPA=%p ET=%p\n",
            out->LoadLibraryA, out->GetProcAddress, out->ExitThread);
        return STATUS_PROCEDURE_NOT_FOUND;
    }
    DbgPrint("[inj] kernel32: LL=%p GPA=%p ET=%p\n",
        out->LoadLibraryA, out->GetProcAddress, out->ExitThread);
    return STATUS_SUCCESS;
}

// =============================================================================
//  WriteLoaderStub - allocate a RWX page in the target, copy StubData + stub.
//  Returns the VA where the stub begins (thread start address).
// =============================================================================
static NTSTATUS WriteLoaderStub(Kernel32Api* api, PVOID imageBase, PIMAGE_NT_HEADERS nt,
                                PVOID* outStubVa) {
    StubData data{};
    data.LoadLibraryA   = api->LoadLibraryA;
    data.GetProcAddress = api->GetProcAddress;
    data.ExitThread     = api->ExitThread;
    data.ImageBase      = imageBase;
    data.SizeOfImage    = nt->OptionalHeader.SizeOfImage;
    data.ImportDirRva   = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    data.ImportDirSize  = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].Size;
    data.EntryPointRva  = nt->OptionalHeader.AddressOfEntryPoint;

    const SIZE_T stubBytes = reinterpret_cast<SIZE_T>(LoaderStubEnd) -
                             reinterpret_cast<SIZE_T>(LoaderStubStart);
    const SIZE_T totalSize = sizeof(StubData) + stubBytes + 16;

    PVOID stubPage = nullptr;
    SIZE_T allocSize = totalSize;
    NTSTATUS status = ZwAllocateVirtualMemory(NtCurrentProcess(), &stubPage, 0,
        &allocSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!NT_SUCCESS(status)) return status;

    // Layout: [ StubData ][ stub code ]
    RtlCopyMemory(stubPage, &data, sizeof(StubData));
    PUCHAR codePtr = reinterpret_cast<PUCHAR>(stubPage) + sizeof(StubData);
    RtlCopyMemory(codePtr, reinterpret_cast<PVOID>(LoaderStubStart), stubBytes);

    *outStubVa = codePtr;
    DbgPrint("[inj] stub page %p, code %p, stub bytes %llu\n",
        stubPage, codePtr, stubBytes);
    return STATUS_SUCCESS;
}

// =============================================================================
//  InjectDll - core manual-map routine. Runs ATTACHED to the target.
// =============================================================================
static NTSTATUS InjectDll(PEPROCESS target, PUCHAR dllData, SIZE_T dllSize) {
    UNREFERENCED_PARAMETER(dllSize);
    NTSTATUS status;

    auto* dos = reinterpret_cast<PIMAGE_DOS_HEADER>(dllData);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        DbgPrint("[inj] bad DOS sig\n");
        return STATUS_INVALID_IMAGE_FORMAT;
    }
    auto* nt = reinterpret_cast<PIMAGE_NT_HEADERS>(dllData + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        DbgPrint("[inj] bad NT sig\n");
        return STATUS_INVALID_IMAGE_FORMAT;
    }
    if (nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) {
        DbgPrint("[inj] not x64 (machine=0x%X)\n", nt->FileHeader.Machine);
        return STATUS_INVALID_IMAGE_FORMAT;
    }

    const SIZE_T sizeOfImage = nt->OptionalHeader.SizeOfImage;
    const ULONG_PTR preferred = nt->OptionalHeader.ImageBase;
    auto* sections = IMAGE_FIRST_SECTION(nt);

    KAPC_STATE apc;
    KeStackAttachProcess(target, &apc);

    // 1. Allocate SizeOfImage in the target (RW initially; sections fixed later).
    PVOID imageBase = nullptr;
    SIZE_T regionSize = sizeOfImage;
    status = ZwAllocateVirtualMemory(NtCurrentProcess(), &imageBase, 0, &regionSize,
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!NT_SUCCESS(status)) {
        DbgPrint("[inj] alloc failed 0x%X\n", status);
        KeUnstackDetachProcess(&apc);
        return status;
    }

    // 2. Map headers.
    RtlCopyMemory(imageBase, dllData, nt->OptionalHeader.SizeOfHeaders);

    // 3. Map sections (copy raw -> virtual; zero-fill remainder).
    for (USHORT i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        PUCHAR dst = reinterpret_cast<PUCHAR>(imageBase) + sections[i].VirtualAddress;
        PUCHAR src = dllData + sections[i].PointerToRawData;
        SIZE_T raw = sections[i].SizeOfRawData;
        SIZE_T virt = sections[i].Misc.VirtualSize;
        if (raw) RtlCopyMemory(dst, src, raw);
        if (virt > raw) {
            RtlZeroMemory(dst + raw, virt - raw);
        }
    }

    // 4. Relocations (working on the mapped copy in place).
    {
        PIMAGE_NT_HEADERS mappedNt = reinterpret_cast<PIMAGE_NT_HEADERS>(
            reinterpret_cast<PUCHAR>(imageBase) + dos->e_lfanew);
        ApplyRelocations(reinterpret_cast<PUCHAR>(imageBase), mappedNt, preferred);
    }

    // 5. Section page protections.
    PIMAGE_NT_HEADERS mappedNt = reinterpret_cast<PIMAGE_NT_HEADERS>(
        reinterpret_cast<PUCHAR>(imageBase) + dos->e_lfanew);
    PIMAGE_SECTION_HEADER mappedSections = IMAGE_FIRST_SECTION(mappedNt);
    for (USHORT i = 0; i < mappedNt->FileHeader.NumberOfSections; i++) {
        ULONG prot = PAGE_NOACCESS;
        ULONG ch = mappedSections[i].Characteristics;
        bool exec  = (ch & IMAGE_SCN_MEM_EXECUTE) != 0;
        bool read  = (ch & IMAGE_SCN_MEM_READ)    != 0;
        bool write = (ch & IMAGE_SCN_MEM_WRITE)   != 0;

        if (exec && write)       prot = PAGE_EXECUTE_READWRITE;
        else if (exec && read)   prot = PAGE_EXECUTE_READ;
        else if (exec)           prot = PAGE_EXECUTE;
        else if (read && write)  prot = PAGE_READWRITE;
        else if (read)           prot = PAGE_READONLY;
        else if (write)          prot = PAGE_WRITECOPY;
        else                     continue;   // leave PAGE_NOACCESS

        PVOID  base = reinterpret_cast<PUCHAR>(imageBase) + mappedSections[i].VirtualAddress;
        SIZE_T size = mappedSections[i].Misc.VirtualSize
                      ? mappedSections[i].Misc.VirtualSize
                      : mappedSections[i].SizeOfRawData;
        ULONG oldProt = 0;
        ZwProtectVirtualMemory(NtCurrentProcess(), &base, &size, prot, &oldProt);
    }

    // 6. Resolve kernel32 API in the target.
    Kernel32Api api{};
    status = ResolveKernel32Api(target, &api);
    if (!NT_SUCCESS(status)) {
        DbgPrint("[inj] ResolveKernel32Api failed 0x%X\n", status);
        SIZE_T freeSz = 0;
        ZwFreeVirtualMemory(NtCurrentProcess(), &imageBase, &freeSz, MEM_RELEASE);
        KeUnstackDetachProcess(&apc);
        return status;
    }

    // 7. Write loader stub (returns stub code VA).
    PVOID stubVa = nullptr;
    status = WriteLoaderStub(&api, imageBase, mappedNt, &stubVa);
    if (!NT_SUCCESS(status)) {
        SIZE_T freeSz = 0;
        ZwFreeVirtualMemory(NtCurrentProcess(), &imageBase, &freeSz, MEM_RELEASE);
        KeUnstackDetachProcess(&apc);
        return status;
    }
    PVOID stubDataVa = reinterpret_cast<PUCHAR>(stubVa) - sizeof(StubData);

    // 8. Create the remote thread running the stub. RtlCreateUserThread is
    //    exported by ntoskrnl (verified) and declared above. Signature (x64):
    //      NTSTATUS RtlCreateUserThread(
    //          HANDLE Process, PSECURITY_DESCRIPTOR, BOOLEAN CreateSuspended,
    //          ULONG StackZeroBits, PULONG StackReserved, PULONG StackCommit,
    //          PVOID StartAddress, PVOID StartParameter,
    //          PHANDLE ThreadHandle, PCLIENT_ID ClientId);
    HANDLE  hThread = nullptr;
    CLIENT_ID cid{};
    status = RtlCreateUserThread(
        NtCurrentProcess(),     // we're attached to target
        nullptr,                // security descriptor
        FALSE,                  // not suspended
        0,                      // StackZeroBits
        nullptr, nullptr,       // stack sizes (default)
        stubVa,                 // start address (the loader stub)
        stubDataVa,             // StartParameter -> rcx = StubData*
        &hThread,               // out thread handle
        &cid);                  // out client id
    if (!NT_SUCCESS(status)) {
        DbgPrint("[inj] RtlCreateUserThread failed 0x%X\n", status);
        SIZE_T freeSz = 0;
        ZwFreeVirtualMemory(NtCurrentProcess(), &imageBase, &freeSz, MEM_RELEASE);
        KeUnstackDetachProcess(&apc);
        return status;
    }

    DbgPrint("[inj] remote thread created: tid=%lu, base=%p, stub=%p\n",
        HandleToULong(cid.UniqueThread), imageBase, stubVa);

    // 9. Wait briefly for DllMain to return (so we can report success/fail).
    //    Capped at 5s; stayout_dll spawns its own worker thread from DllMain
    //    and returns immediately, so this returns fast.
    if (hThread) {
        LARGE_INTEGER timeout;
        timeout.QuadPart = -5 * 10000000LL;   // 5s relative (100ns units, negative)
        ZwWaitForSingleObject(hThread, FALSE, &timeout);
        ZwClose(hThread);
    }

    KeUnstackDetachProcess(&apc);
    DbgPrint("[inj] injection complete: mapped at %p\n", imageBase);
    return STATUS_SUCCESS;
}

// =============================================================================
//  IOCTL dispatch.
// =============================================================================
static NTSTATUS HandleInject(InjectionRequest* req) {
    NTSTATUS status;

    PEPROCESS target = nullptr;
    // PsLookupProcessByProcessId treats the HANDLE as a PID when the low 2 bits
    // are clear; cast through ULONG_PTR to avoid the ULONG->HANDLE truncation warning.
    status = PsLookupProcessByProcessId(reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(req->TargetPid)), &target);
    if (!NT_SUCCESS(status)) {
        DbgPrint("[inj] target %u not found: 0x%X\n", req->TargetPid, status);
        return status;
    }

    UNICODE_STRING path;
    RtlInitUnicodeString(&path, req->DllPath);

    PUCHAR dllData = nullptr;
    SIZE_T dllSize = 0;
    status = ReadFileIntoPool(&path, &dllData, &dllSize);
    if (!NT_SUCCESS(status)) {
        DbgPrint("[inj] read dll failed 0x%X\n", status);
        ObDereferenceObject(target);
        return status;
    }
    PoolFreer pf(dllData);

    // InjectDll attaches to the target itself.
    status = InjectDll(target, dllData, dllSize);

    ObDereferenceObject(target);
    return status;
}

// =============================================================================
//  IRP dispatch.
// =============================================================================
static NTSTATUS DeviceControl(PDEVICE_OBJECT, PIRP irp) {
    auto* stack = IoGetCurrentIrpStackLocation(irp);
    NTSTATUS status = STATUS_SUCCESS;
    ULONG info = 0;

    const ULONG code = stack->Parameters.DeviceIoControl.IoControlCode;
    const ULONG inLen = stack->Parameters.DeviceIoControl.InputBufferLength;

    if (code == IOCTL_INJECT_DLL) {
        if (inLen < sizeof(InjectionRequest)) {
            status = STATUS_BUFFER_TOO_SMALL;
        } else {
            auto* req = reinterpret_cast<InjectionRequest*>(irp->AssociatedIrp.SystemBuffer);
            DbgPrint("[inj] IOCTL: pid=%lu path=%ws\n", req->TargetPid, req->DllPath);
            status = HandleInject(req);
        }
    } else {
        status = STATUS_INVALID_DEVICE_REQUEST;
    }

    irp->IoStatus.Status = status;
    irp->IoStatus.Information = info;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return status;
}

// =============================================================================
//  DriverEntry.
// =============================================================================
extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT driverObject, PUNICODE_STRING registryPath) {
    UNREFERENCED_PARAMETER(registryPath);

    UNICODE_STRING deviceName;
    RtlInitUnicodeString(&deviceName, DEVICE_NAME);
    PDEVICE_OBJECT deviceObject = nullptr;

    NTSTATUS status = IoCreateDevice(driverObject, 0, &deviceName,
        FILE_DEVICE_UNKNOWN, 0, FALSE, &deviceObject);
    if (!NT_SUCCESS(status)) {
        DbgPrint("[inj] IoCreateDevice failed 0x%X\n", status);
        return status;
    }

    UNICODE_STRING symLink;
    RtlInitUnicodeString(&symLink, SYMLINK_NAME);
    status = IoCreateSymbolicLink(&symLink, &deviceName);
    if (!NT_SUCCESS(status)) {
        IoDeleteDevice(deviceObject);
        return status;
    }

    driverObject->MajorFunction[IRP_MJ_CREATE] =
    driverObject->MajorFunction[IRP_MJ_CLOSE] =
        [](PDEVICE_OBJECT, PIRP irp) -> NTSTATUS {
            irp->IoStatus.Status = STATUS_SUCCESS;
            irp->IoStatus.Information = 0;
            IoCompleteRequest(irp, IO_NO_INCREMENT);
            return STATUS_SUCCESS;
        };
    driverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = DeviceControl;
    driverObject->DriverUnload = [](PDRIVER_OBJECT drv) {
        UNICODE_STRING sym;
        RtlInitUnicodeString(&sym, SYMLINK_NAME);
        IoDeleteSymbolicLink(&sym);
        if (drv->DeviceObject) IoDeleteDevice(drv->DeviceObject);
        DbgPrint("[inj] ModernInjector unloaded\n");
    };

    DbgPrint("[inj] ModernInjector loaded\n");
    return STATUS_SUCCESS;
}
