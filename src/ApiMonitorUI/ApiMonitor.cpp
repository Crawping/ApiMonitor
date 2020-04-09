
#include <cassert>
#include <iostream>
#include <thread>
#include <vector>
#include <windows.h>
#include "def.h"
#include "NamedPipe.h"
#include "pipemessage.h"
#include "ApiMonitor.h"

using namespace std;


namespace Detail
{


typedef struct reloc_line
{
    WORD m_addr : 12;
    WORD m_type : 4;
} reloc_line;

void LoadVReloc(ULONG_PTR hBase, bool bForce, ULONG_PTR delta)
{
    PIMAGE_NT_HEADERS imNH = (PIMAGE_NT_HEADERS)(hBase + ((PIMAGE_DOS_HEADER)hBase)->e_lfanew);
    if (imNH->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].VirtualAddress == 0)
        return; // û���ض�λ����
    if (hBase == imNH->OptionalHeader.ImageBase && bForce == FALSE)
        return; // װ����Ĭ�ϵ�ַ
    if (delta == 0)
        delta = hBase - imNH->OptionalHeader.ImageBase;
    ULONG_PTR lpreloc = hBase + imNH->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].VirtualAddress;
    PIMAGE_BASE_RELOCATION pimBR = (PIMAGE_BASE_RELOCATION)lpreloc;
    while (pimBR->VirtualAddress != 0)
    {
        reloc_line* reline = (reloc_line*)((char*)pimBR + sizeof(IMAGE_BASE_RELOCATION));
        int preNum = (pimBR->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(reloc_line);
        for (int i = 0; i < preNum; ++i)
        {
            switch (reline->m_type)
            {
            case IMAGE_REL_BASED_HIGHLOW:
                *(PDWORD)(hBase + pimBR->VirtualAddress + reline->m_addr) += delta;
                break;
            case IMAGE_REL_BASED_DIR64:
                *(ULONG_PTR*)(hBase + pimBR->VirtualAddress + reline->m_addr) += delta;
                break;
            }
            ++reline;
        }
        pimBR = (PIMAGE_BASE_RELOCATION)reline;
    }
}


PVOID BuildRemoteData(HANDLE hProcess, const TCHAR* dllPath)
{
    HMODULE hDll2 = LoadLibraryEx(dllPath, NULL, 0);
    ULONG_PTR entry = (ULONG_PTR)GetProcAddress(hDll2, "Entry");
    HANDLE hDll = CreateFile(dllPath, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    if (hDll == INVALID_HANDLE_VALUE)
        return NULL;
    std::vector<char> file(GetFileSize(hDll, 0));
    SIZE_T R;
    ReadFile(hDll, file.data(), file.size(), &R, 0);
    CloseHandle(hDll);

    char* imageData = (char*)file.data();
    PIMAGE_NT_HEADERS ntHeader = (PIMAGE_NT_HEADERS)(imageData + ((PIMAGE_DOS_HEADER)imageData)->e_lfanew);
    DWORD imageSize = ntHeader->OptionalHeader.SizeOfImage;
    std::vector<char> memData(imageSize);
    PIMAGE_SECTION_HEADER secHeader = (PIMAGE_SECTION_HEADER)((ULONG_PTR)ntHeader + sizeof(IMAGE_NT_HEADERS));
    DWORD secHeaderBegin = secHeader->VirtualAddress;
    for (DWORD i = 0; i < ntHeader->FileHeader.NumberOfSections; ++i)
    {
        if (secHeader->PointerToRawData != 0)
            secHeaderBegin = min(secHeader->PointerToRawData, secHeaderBegin);
        memcpy(&memData[secHeader->VirtualAddress], imageData + secHeader->PointerToRawData, secHeader->SizeOfRawData);
        ++secHeader;
    }
    memcpy(memData.data(), imageData, secHeaderBegin); // ���� pe ͷ
    PVOID newBase = VirtualAllocEx(hProcess, 0, imageSize, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    ULONG_PTR delta = (ULONG_PTR)newBase - (ULONG_PTR)ntHeader->OptionalHeader.ImageBase;
    if (delta != 0) // ��Ҫ�ض�λ
        LoadVReloc((ULONG_PTR)memData.data(), TRUE, delta);
    SIZE_T W = 0;
    WriteProcessMemory(hProcess, newBase, memData.data(), imageSize, &W);
    PVOID oep = (PVOID)(entry - (ULONG_PTR)hDll2 + (ULONG_PTR)newBase);


    HMODULE ntDllBase = GetModuleHandleA("ntdll.dll");
    auto pLdrLoadDll = (FN_LdrLoadDll)GetProcAddress(ntDllBase, "LdrLoadDll");
    vector<unsigned char> remoteMemory(0x200);
    ReadProcessMemory(hProcess, (LPVOID)((ULONG_PTR)pLdrLoadDll - 0x100), remoteMemory.data(), remoteMemory.size(), &R);
    bool found = false;
    size_t position = 0;
    for (size_t i = 0x100; i > 0 && !found; --i)
    {
        if (remoteMemory[i] == 0xcc)
        {
            int k = 0;
            for (; k < 7; ++k)
                if (remoteMemory[i - k] != 0xcc)
                    break;
            if (k == 7)
            {
                found = true;
                position = i - 6;
            }
        }
    }
    assert(found);
    if (found)
    {
        char jmp[2];
        jmp[0] = '\xeb';
        jmp[1] = position - (0x100 + 0x2);
        WriteProcessMemory(hProcess, (LPVOID)pLdrLoadDll, jmp, sizeof(jmp), &R);

        auto hook = GetProcAddress(hDll2, "HookLdrLoadDllPad");
        char jmp2[6];
        jmp2[0] = '\x68';
        *(PDWORD)&jmp2[1] = (DWORD)((ULONG_PTR)hook - (ULONG_PTR)hDll2 + (ULONG_PTR)newBase);
        jmp2[5] = '\xc3';
        WriteProcessMemory(hProcess, (LPVOID)((ULONG_PTR)pLdrLoadDll - 0x100 + position), jmp2, sizeof(jmp2), &R);
    }


    ///////////////////////////////////////////////////////////////////////////
    // ���ص��� _DllMainCRTStartup ��
    {
        PIMAGE_NT_HEADERS ntDllNtHeader = (PIMAGE_NT_HEADERS)((ULONG_PTR)ntDllBase + ((PIMAGE_DOS_HEADER)ntDllBase)->e_lfanew);
        PIMAGE_SECTION_HEADER ntDllSecHeader = (PIMAGE_SECTION_HEADER)((ULONG_PTR)ntDllNtHeader + sizeof(IMAGE_NT_HEADERS));
        DWORD ntDllSecHeaderBegin = ntDllSecHeader->VirtualAddress;
        //
        // ff7514   push    dword ptr[ebp + 14h]
        // ff7510   push    dword ptr[ebp + 10h]
        // ff750c   push    dword ptr[ebp + 0Ch]
        // ff5508   call    dword ptr[ebp + 8]
        //
        char pattern[] = "\xff\x75\x14\xff\x75\x10\xff\x75\x0c\xff\x55\x08";
        char pattern2[] = "\xcc\xcc\xcc\xcc\xcc\xcc\xcc\xcc\xcc\xcc\xcc";
        void* addrFound = nullptr;
        void* pad       = nullptr;
        for (DWORD i = 0; i < ntDllNtHeader->FileHeader.NumberOfSections; ++i)
        {
            if (strncmp((char*)ntDllSecHeader->Name, ".text", 8))
            {
                ++ntDllSecHeader;
                continue;
            }

            for (int k = 0; k < ntDllSecHeader->Misc.VirtualSize; ++k)
            {
                if (!memcmp((char*)((ULONG_PTR)ntDllBase + ntDllSecHeader->VirtualAddress + k), pattern, sizeof(pattern) - 1))
                {
                    addrFound = (char*)((ULONG_PTR)ntDllBase + ntDllSecHeader->VirtualAddress + k);
                    break;
                }
            }
            break;
        }
        assert(addrFound && "_DllMainCRTStartup caller not found");
        for (char* i = (char*)addrFound; i > (char*)addrFound - 128; --i)
        {
            if (!memcmp(i, pattern2, sizeof(pattern2) - 1))
            {
                pad = i;
                break;
            }
        }
        assert(pad && "launch address not found 2");
        char jmp1[3] = { '\xeb', '\x90', '\x90' };
        ULONG_PTR callInstrAddr = (ULONG_PTR)addrFound + 9;
        ULONG_PTR retAddr = callInstrAddr + 2;
        jmp1[1] = (char)(0x100 - (retAddr - (ULONG_PTR)pad));
        WriteProcessMemory(hProcess, (char*)callInstrAddr, jmp1, sizeof(jmp1), &R);
        auto hook = GetProcAddress(hDll2, "DllMainCRTStartupPad");
        char jmp2[11] = { 0 };
        jmp2[0] = '\x68';
        *(PDWORD)&jmp2[1] = (DWORD)retAddr;
        jmp2[5] = '\x68';
        *(PDWORD)&jmp2[6] = (DWORD)((ULONG_PTR)hook - (ULONG_PTR)hDll2 + (ULONG_PTR)newBase);
        jmp2[10] = '\xc3';
        WriteProcessMemory(hProcess, pad, jmp2, sizeof(jmp2), &R);
    }

    FreeLibrary(hDll2);
    return oep;
}


}



void Monitor::SetPipeHandler(PipeController * controller)
{
    mControllerRef = controller;
}

int Monitor::LoadFile(const std::wstring& filePath)
{
    WCHAR cmd[MAX_PATH] = { 0 };
    wcscpy_s(cmd, filePath.c_str());
    STARTUPINFO si = { 0 };
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = { 0 };
    BOOL success = CreateProcess(filePath.c_str(), cmd, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, NULL, &si, &pi);

    LPVOID paramBase = VirtualAllocEx(pi.hProcess, (LPVOID)PARAM::PARAM_ADDR, PARAM::PARAM_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    PVOID oep = Detail::BuildRemoteData(pi.hProcess, TEXT("C:\\Projects\\ApiMonitor\\bin\\Win32\\Release\\PayLoad.dll"));
    SIZE_T R = 0;
    PARAM param;
    memset(&param, 0, sizeof(PARAM));
    param.ntdllBase = (LPVOID)GetModuleHandleA("ntdll.dll");
    param.f_LdrLoadDll = (FN_LdrLoadDll)((ULONG_PTR)GetProcAddress((HMODULE)param.ntdllBase, "LdrLoadDll") + 2);
    param.dwProcessId = pi.dwProcessId;
    param.dwThreadId = pi.dwThreadId;
    param.ctx.ContextFlags = CONTEXT_ALL;
    GetThreadContext(pi.hThread, &param.ctx);

    WriteProcessMemory(pi.hProcess, paramBase, &param, sizeof(param), &R);
    CONTEXT copy = param.ctx;
    copy.Eax = (DWORD)oep;
    SetThreadContext(pi.hThread, &copy);

    NamedPipeServer ps;
    std::thread th = std::thread([&]() {
        ps.StartServer(PipeDefine::PIPE_NAME, mControllerRef->mMsgHandler, mControllerRef->mUserData);
    });

    while (!ps.IsRunning())
        Sleep(1);
    ResumeThread(pi.hThread);

    Sleep(3000);
    //MessageBoxA(0, "will suspend.", 0, 0);
    //auto sus = (FN_NtSuspendProcess)GetProcAddress((HMODULE)GetModuleHandleA("ntdll.dll"), "NtSuspendProcess");
    //sus(pi.hProcess);
    //MessageBoxA(0, "will resume.", 0, 0);
    //auto res = (FN_NtSuspendProcess)GetProcAddress((HMODULE)GetModuleHandleA("ntdll.dll"), "NtResumeProcess");
    //res(pi.hProcess);
    MessageBoxA(0, "break when \"OutputDebugStringA\" called.", 0, 0);

    PipeDefine::msg::SetBreakCondition* cond = mControllerRef->Lock();
    cond->func_addr = mControllerRef->outputdbgstr;
    cond->break_next_time = true;
    mControllerRef->UnLock();
    mControllerRef->mConditionReady = true;

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    while (!mStopMonitor)
        Sleep(1);
    ps.StopServer();
    th.join();
    return 0;
}


