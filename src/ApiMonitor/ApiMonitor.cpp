﻿
#include <cassert>
#include <iostream>
#include <thread>
#include <vector>
#include <windows.h>
#include "def.h"
#include "NamedPipe.h"
#include "pipemessage.h"

using namespace std;

typedef struct reloc_line
{
    WORD m_addr : 12;
    WORD m_type : 4;
} reloc_line;

void LoadVReloc(ULONG_PTR hBase, bool bForce, ULONG_PTR delta)
{
    PIMAGE_NT_HEADERS imNH = (PIMAGE_NT_HEADERS)(hBase + ((PIMAGE_DOS_HEADER)hBase)->e_lfanew);
    if (imNH->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].VirtualAddress == 0)
        return; // 没有重定位数据
    if (hBase == imNH->OptionalHeader.ImageBase && bForce == FALSE)
        return; // 装入了默认地址
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
    memcpy(memData.data(), imageData, secHeaderBegin); // 复制 pe 头
    PVOID newBase = VirtualAllocEx(hProcess, 0, imageSize, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    ULONG_PTR delta = (ULONG_PTR)newBase - (ULONG_PTR)ntHeader->OptionalHeader.ImageBase;
    if (delta != 0) // 需要重定位
        LoadVReloc((ULONG_PTR)memData.data(), TRUE, delta);
    SIZE_T W = 0;
    WriteProcessMemory(hProcess, newBase, memData.data(), imageSize, &W);
    PVOID oep = (PVOID)(entry - (ULONG_PTR)hDll2 + (ULONG_PTR)newBase);

    
    ///////////////////////////////////////////////////////////////////////////
    // 拦截 LdrLoadDll

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
    assert(found && "launch space not found");
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
    // 拦截 LdrxCallInitRoutine(调用 _DllMainCRTStartup)
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

class PipeReply
{
public:
    CRITICAL_SECTION mPipeMsgCS;
    PipeDefine::msg::SetBreakCondition mSetBreakCondition;
    bool mConditionReady{ false };

    // debug
    long long outputdbgstr{ 0 };

    PipeReply()
    {
        InitializeCriticalSection(&mPipeMsgCS);
    }

    ~PipeReply()
    {
        DeleteCriticalSection(&mPipeMsgCS);
    }

    PipeDefine::msg::SetBreakCondition* Lock()
    {
        EnterCriticalSection(&mPipeMsgCS);
        return &mSetBreakCondition;
    }

    void UnLock()
    {
        LeaveCriticalSection(&mPipeMsgCS);
    }
};
PipeReply g_Reply;


void Reply(const uint8_t *readData, uint32_t readDataSize, uint8_t *writeData, uint32_t *writeDataSize, const uint32_t maxWriteBuffer, void* userData)
{
    printf("data arrive. size=%d\n", readDataSize);
    if (readDataSize < sizeof(PipeDefine::PipeMsg) + sizeof(size_t))
    {
        // 过短消息
        printf("too short.");
        return;
    }

    PipeDefine::Message* msg = (PipeDefine::Message*)readData;
    while ((const uint8_t *)msg - readData < readDataSize)
    {
        switch (msg->type)
        {
        case PipeDefine::Pipe_C_Req_Inited: {
            PipeDefine::msg::Init m;
            std::vector<char, Allocator::allocator<char>> str(msg->Content, msg->Content + msg->ContentSize);
            m.Unserial(str);
            m.dummy += 1;
            str = m.Serial();
            PipeDefine::Message* msg2 = (PipeDefine::Message*)writeData;
            msg2->type = PipeDefine::Pipe_S_Ack_Inited;
            msg2->tid = msg->tid;
            msg2->ContentSize = str.size();
            memcpy_s(msg2->Content, maxWriteBuffer, str.data(), str.size());
            *writeDataSize = msg2->HeaderLength + msg2->ContentSize;
            break;
        }
        case PipeDefine::Pipe_C_Req_ModuleApiList: {
            PipeDefine::msg::ModuleApis m;
            std::vector<char, Allocator::allocator<char>> str(msg->Content, msg->Content + msg->ContentSize);
            m.Unserial(str);
            for (size_t i = 0; i < m.apis.size(); ++i)
            {
                if (!_stricmp(m.module_name.c_str(), "kernel32.dll") && m.apis[i].name == "OutputDebugStringA")
                    g_Reply.outputdbgstr = m.apis[i].va;
            }

            PipeDefine::msg::ApiFilter filter;
            filter.module_name = m.module_name;
            for (size_t i = 0; i < m.apis.size(); ++i)
            {
                PipeDefine::msg::ApiFilter::Api filter_api;
                filter_api.api_name = m.apis[i].name;
                filter_api.filter = true;
                filter.apis.push_back(filter_api);
            }
            str = filter.Serial();
            PipeDefine::Message* msg2 = (PipeDefine::Message*)writeData;
            msg2->type = PipeDefine::Pipe_S_Ack_FilterApi;
            msg2->tid = msg->tid;
            msg2->ContentSize = str.size();
            memcpy_s(msg2->Content, maxWriteBuffer, str.data(), str.size());
            *writeDataSize = msg2->HeaderLength + msg2->ContentSize;
            break;
        }
        case PipeDefine::Pipe_C_Req_ApiInvoked: {
            PipeDefine::msg::ApiInvoked m;
            std::vector<char, Allocator::allocator<char>> str(msg->Content, msg->Content + msg->ContentSize);
            m.Unserial(str);
            //printf("Api Invoked: %s, %s, tid: %d, call from: 0x%llx, time: %d\n", m.module_name.c_str(), m.api_name.c_str(), msg->tid, m.call_from, m.times);
            if (g_Reply.mConditionReady)
            {
                g_Reply.mConditionReady = false;
                auto msg = g_Reply.Lock();
                str = msg->Serial();
                PipeDefine::Message* msg2 = (PipeDefine::Message*)writeData;
                msg2->type = PipeDefine::Pipe_S_Req_SetBreakCondition;
                msg2->tid = -1;
                msg2->ContentSize = str.size();
                memcpy_s(msg2->Content, maxWriteBuffer, str.data(), str.size());
                *writeDataSize = msg2->HeaderLength + msg2->ContentSize;
                g_Reply.UnLock();
                printf("condition sent!\n");
            }
            break;
        }
        default:
            printf("unknown message type.\n");
            throw "unknown message type.";
            break;
        }

        msg = (PipeDefine::Message*)((intptr_t)msg + PipeDefine::Message::HeaderLength + msg->ContentSize);
    }
}

int main(int argc, char** argv)
{
    WCHAR app[MAX_PATH] = { 0 };
    WCHAR cmd[MAX_PATH] = { 0 };
    MultiByteToWideChar(CP_ACP, 0, argv[1], -1, app, MAX_PATH - 1);
    wcscpy_s(cmd, app);
    STARTUPINFO si = { 0 };
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = { 0 };
    BOOL success = CreateProcess(app, cmd, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, NULL, &si, &pi);

    LPVOID paramBase = VirtualAllocEx(pi.hProcess, (LPVOID)PARAM::PARAM_ADDR, PARAM::PARAM_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    PVOID oep = BuildRemoteData(pi.hProcess, TEXT("C:\\Projects\\ApiMonitor\\bin\\Win32\\Release\\PayLoad.dll"));
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
        ps.StartServer(PipeDefine::PIPE_NAME, Reply, nullptr);
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
    //MessageBoxA(0, "break when \"OutputDebugStringA\" called.", 0, 0);

    //PipeDefine::msg::SetBreakCondition* cond = g_Reply.Lock();
    //cond->func_addr = g_Reply.outputdbgstr;
    //cond->break_next_time = true;
    //g_Reply.UnLock();
    //g_Reply.mConditionReady = true;

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    while (1)
        Sleep(1);
    ps.StopServer();
    th.join();
    return 0;
}

