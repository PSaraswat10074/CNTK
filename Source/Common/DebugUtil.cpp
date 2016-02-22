//
// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.md file in the project root for full license information.
//
// DebugUtil.cpp : Defines the debug util functions.
//
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS // "secure" CRT not available on all platforms  --add this at the top of all CPP files that give "function or variable may be unsafe" warnings
#endif
#include "stdafx.h"
#include "Basics.h"
#include "DebugUtil.h"
#include <algorithm>

namespace Microsoft { namespace MSR { namespace CNTK {

using namespace std;

void DebugUtil::PrintCallStack()
{

#ifdef _WIN32
    typedef USHORT(WINAPI * CaptureStackBackTraceType)(__in ULONG, __in ULONG, __out PVOID*, __out_opt PULONG);
    CaptureStackBackTraceType func = (CaptureStackBackTraceType)(GetProcAddress(LoadLibrary(L"kernel32.dll"), "RtlCaptureStackBackTrace"));

    if (func == NULL)
        return;

    void* callStack[MAX_CALLERS];
    unsigned short frames;
    SYMBOL_INFO* symbolInfo;
    HANDLE process;

    process = GetCurrentProcess();
    if (!SymInitialize(process, NULL, TRUE))
    {
        DWORD error = GetLastError();
        std::cerr << "Failed to print CALL STACK! SymInitialize error : " << msra::strfun::utf8(FormatWin32Error(error)) << std::endl;
        return;
    }

    frames = (func)(0, MAX_CALLERS, callStack, NULL);
    symbolInfo = (SYMBOL_INFO*) calloc(sizeof(SYMBOL_INFO) + 256 * sizeof(char), 1);
    symbolInfo->MaxNameLen = 255;
    symbolInfo->SizeOfStruct = sizeof(SYMBOL_INFO);
    frames = std::min(frames, MAX_CALL_STACK_DEPTH);

    std::cerr << std::endl
              << "[CALL STACK]" << std::endl;

    for (unsigned int i = 1; i < frames; i++)
    {
        if (i == 1)
        {
            std::cerr << "    >";
        }
        else
        {
            std::cerr << "    -";
        }

        if (SymFromAddr(process, (DWORD64)(callStack[i]), 0, symbolInfo))
        {
            std::cerr << symbolInfo->Name << std::endl;
        }
        else
        {
            DWORD error = GetLastError();
            std::cerr << callStack[i] << " (SymFromAddr error : " << msra::strfun::utf8(FormatWin32Error(error)) << ")" << std::endl;
        }
    }

    std::cerr << std::endl;

    free(symbolInfo);

    SymCleanup(process);
#else
    std::cerr << std::endl
              << "[CALL STACK]" << std::endl;

    unsigned int MAX_NUM_FRAMES = 1024;
    void* backtraceAddresses[MAX_NUM_FRAMES];
    unsigned int numFrames = backtrace(backtraceAddresses, MAX_NUM_FRAMES);
    char** symbolList = backtrace_symbols(backtraceAddresses, numFrames);

    for (unsigned int i = 0; i < numFrames; i++)
    {
        char* beginName = NULL;
        char* beginOffset = NULL;
        char* endOffset = NULL;

        // Find parentheses and +address offset surrounding the mangled name
        for (char* p = symbolList[i]; *p; ++p)
        {
            if (*p == '(')
                beginName = p;
            else if (*p == '+')
                beginOffset = p;
            else if ((*p == ')') && (beginOffset || beginName))
                endOffset = p;
        }

        if (beginName && endOffset && (beginName < endOffset))
        {
            *beginName++ = '\0';
            *endOffset++ = '\0';
            if (beginOffset)
                *beginOffset++ = '\0';

            // Mangled name is now in [beginName, beginOffset) and caller offset in [beginOffset, endOffset).
            int status = 0;
            unsigned int MAX_FUNCNAME_SIZE = 4096;
            size_t funcNameSize = MAX_FUNCNAME_SIZE;
            char funcName[MAX_FUNCNAME_SIZE];
            char* ret = abi::__cxa_demangle(beginName, funcName, &funcNameSize, &status);
            char* fName = beginName;
            if (status == 0)
                fName = ret;

            if (beginOffset)
            {
                fprintf(stderr, "  %-30s ( %-40s  + %-6s) %s\n", symbolList[i], fName, beginOffset, endOffset);
            }
            else
            {
                fprintf(stderr, "  %-30s ( %-40s    %-6s) %s\n", symbolList[i], fName, "", endOffset);
            }
        }
        else
        {
            // Couldn't parse the line. Print the whole line.
            fprintf(stderr, "  %-30s\n", symbolList[i]);
        }
    }

    free(symbolList);
#endif
}

std::string DebugUtil::GetCallStack()
{
    std::string crlf = "\n";
    std::string flatCallStack(crlf);

#ifdef _WIN32
    typedef USHORT(WINAPI * CaptureStackBackTraceType)(__in ULONG, __in ULONG, __out PVOID*, __out_opt PULONG);
    CaptureStackBackTraceType func = (CaptureStackBackTraceType)(GetProcAddress(LoadLibrary(L"kernel32.dll"), "RtlCaptureStackBackTrace"));

    if (func == NULL)
        return flatCallStack;

    void* callStack[MAX_CALLERS];
    unsigned short frames;
    SYMBOL_INFO* symbolInfo;
    HANDLE process;

    process = GetCurrentProcess();
    if (!SymInitialize(process, NULL, TRUE))
    {
        DWORD error = GetLastError();
        flatCallStack += "Failed to print CALL STACK! SymInitialize error : " + msra::strfun::utf8(FormatWin32Error(error));
        return flatCallStack;
    }

    frames = (func)(0, MAX_CALLERS, callStack, NULL);
    symbolInfo = (SYMBOL_INFO*)calloc(sizeof(SYMBOL_INFO) + 256 * sizeof(char), 1);
    symbolInfo->MaxNameLen = 255;
    symbolInfo->SizeOfStruct = sizeof(SYMBOL_INFO);
    frames = std::min(frames, MAX_CALL_STACK_DEPTH);

    for (unsigned int i = 1; i < frames; i++)
    {
        if (i == 1)
        {
            flatCallStack += "    >";
        }
        else
        {
            flatCallStack += "    -";
        }

        if (SymFromAddr(process, (DWORD64)(callStack[i]), 0, symbolInfo))
        {
            flatCallStack += symbolInfo->Name + crlf;
        }
        else
        {
            DWORD error = GetLastError();
            char buf[17];
            sprintf_s(buf, "%p", callStack[i]);
            flatCallStack += buf;
            flatCallStack += " (SymFromAddr error : " + msra::strfun::utf8(FormatWin32Error(error)) + ")" + crlf;
        }
    }

    flatCallStack += crlf;

    free(symbolInfo);

    SymCleanup(process);
#else
    flatCallStack += "[CALL STACK]" + crlf;

    unsigned int MAX_NUM_FRAMES = 1024;
    void* backtraceAddresses[MAX_NUM_FRAMES];
    unsigned int numFrames = backtrace(backtraceAddresses, MAX_NUM_FRAMES);
    char** symbolList = backtrace_symbols(backtraceAddresses, numFrames);

    for (unsigned int i = 0; i < numFrames; i++)
    {
        char* beginName = NULL;
        char* beginOffset = NULL;
        char* endOffset = NULL;

        // Find parentheses and +address offset surrounding the mangled name
        for (char* p = symbolList[i]; *p; ++p)
        {
            if (*p == '(')
                beginName = p;
            else if (*p == '+')
                beginOffset = p;
            else if ((*p == ')') && (beginOffset || beginName))
                endOffset = p;
        }
        const int buf_size = 1024;
        char buffer[buf_size];

        if (beginName && endOffset && (beginName < endOffset))
        {
            *beginName++ = '\0';
            *endOffset++ = '\0';
            if (beginOffset)
                *beginOffset++ = '\0';

            // Mangled name is now in [beginName, beginOffset) and caller offset in [beginOffset, endOffset).
            int status = 0;
            unsigned int MAX_FUNCNAME_SIZE = 4096;
            size_t funcNameSize = MAX_FUNCNAME_SIZE;
            char funcName[MAX_FUNCNAME_SIZE];
            char* ret = abi::__cxa_demangle(beginName, funcName, &funcNameSize, &status);
            char* fName = beginName;
            if (status == 0)
                fName = ret;

            if (beginOffset)
            {
                flatCallStack += 
                snprintf(buffer, buf_size, "  %-30s ( %-40s  + %-6s) %s\n", symbolList[i], fName, beginOffset, endOffset);
            }
            else
            {
                snprintf(buffer, buf_size, "  %-30s ( %-40s    %-6s) %s\n", symbolList[i], fName, "", endOffset);
            }
        }
        else
        {
            // Couldn't parse the line. Print the whole line.
            snprintf(buffer, buf_size, "  %-30s\n", symbolList[i]);
        }

        flatCallStack += buffer;
    }

    free(symbolList);
#endif
    return flatCallStack;
}
} } }
