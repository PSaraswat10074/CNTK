//
// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.md file in the project root for full license information.
//
// This file requires the NVML library. Unfortunately, this library does not install an environment variable for locating it.
// On Windows, the SDK gets installed to "c:\Program Files\NVIDIA Corporation\GDK\gdk_win7_amd64_release\nvml" (/include, /lib).
// On Linux, you need to install the deployment kit from https://developer.nvidia.com/gpu-deployment-kit and
// set NVML_INCLUDE = /the path you installed deployment kit/usr/include/nvidia/gdk

// From the SDK documentation:
// "The NVML library can be found at: %ProgramW6432%\"NVIDIA Corporation"\NVSMI\ on Windows, but will not be added to the path. To dynamically link to NVML, add this path to the PATH environmental variable. To dynamically load NVML, call LoadLibrary with this path."
// "On Linux the NVML library will be found on the standard library path. For 64-bit Linux, both the 32-bit and 64-bit NVML libraries will be installed."
//

#define _CRT_SECURE_NO_WARNINGS // "secure" CRT not available on all platforms  --add this at the top of all CPP files that give "function or variable may be unsafe" warnings
#include "Basics.h"
#include "Platform.h"
#include "BestGpu.h"
#include "Config.h" // for ConfigParameters
#include "ScriptableObjects.h"
#include "DebugUtil.h"
#ifndef CPUONLY
#pragma comment(lib, "cudart.lib")
#include <cuda_runtime.h>
#include <nvml.h>                // note: expected at "c:\Program Files\NVIDIA Corporation\GDK\gdk_win7_amd64_release\nvml\include" (Windows) and /the path you installed deployment kit/usr/include/nvidia/gdk (Linux)
#pragma comment(lib, "nvml.lib") // note: expected at "c:\Program Files\NVIDIA Corporation\GDK\gdk_win7_amd64_release\nvml\lib" (Windows) and /the path you installed deployment kit/usr/include/nvidia/gdk (Linux)
#include <vector>
#else
int bestGPUDummy = 42; // put something into this CPP, as to avoid a linker warning
#endif
#include "CommonMatrix.h" // for CPUDEVICE

#ifndef CPUONLY // #define this to disable GPUs

// CUDA-C includes
#include <cuda.h>
#ifdef __WINDOWS__
#define NOMINMAX
#include "Windows.h"
#include <Delayimp.h>
#include <Shlobj.h>
#define PATH_DELIMITER '\\'
#elif defined(__UNIX__)
#define PATH_DELIMITER '/'
#endif // __WINDOWS__
#include <stdio.h>
#include <string.h>
#include <algorithm>

#include <memory>
#include "CrossProcessMutex.h"

// ---------------------------------------------------------------------------
// BestGpu class
// ---------------------------------------------------------------------------

namespace Microsoft { namespace MSR { namespace CNTK {

struct ProcessorData
{
    int cores;
    nvmlMemory_t memory;
    nvmlUtilization_t utilization;
    cudaDeviceProp deviceProp;
    size_t cudaFreeMem;
    size_t cudaTotalMem;
    bool cntkFound;
    int deviceId; // the deviceId (cuda side) for this processor
};

enum BestGpuFlags
{
    bestGpuNormal = 0,
    bestGpuAvoidSharing = 1,     // don't share with other known machine learning Apps (cl.exe/dbn.exe)
    bestGpuFavorMemory = 2,      // favor memory
    bestGpuFavorUtilization = 4, // favor low utilization
    bestGpuFavorSpeed = 8,       // favor fastest processor
    bestGpuExclusiveLock = 16,   // obtain mutex for selected GPU
    bestGpuRequery = 256,        // rerun the last query, updating statistics
};

class BestGpu
{
    std::map<int, std::unique_ptr<CrossProcessMutex>> m_GPUMutex;

private:
    bool m_initialized;       // initialized
    bool m_nvmlData;          // nvml Data is valid
    bool m_cudaData;          // cuda Data is valid
    int m_deviceCount;        // how many devices are available?
    int m_queryCount;         // how many times have we queried the usage counters?
    BestGpuFlags m_lastFlags; // flag state at last query
    int m_lastCount;          // count of devices (with filtering of allowed Devices)
    std::vector<ProcessorData*> m_procData;
    int m_allowedDevices; // bitfield of allowed devices
    void GetCudaProperties();
    void GetNvmlData();
    void QueryNvmlData();

public:
    BestGpu()
        : m_initialized(false), m_nvmlData(false), m_cudaData(false), m_deviceCount(0), m_queryCount(0), m_lastFlags(bestGpuNormal), m_lastCount(0), m_allowedDevices(-1)
    {
        Init();
    }
    ~BestGpu();
    void Init();
    void SetAllowedDevices(const std::vector<int>& devices); // only allow certain GPUs
    bool DeviceAllowed(int device);
    void DisallowDevice(int device)
    {
        m_allowedDevices &= ~(1 << device);
    }
    void AllowAll();                                                                          // reset to allow all GPUs (no allowed list)
    bool UseMultiple();                                                                       // using multiple GPUs?
    int GetDevice(BestGpuFlags flags = bestGpuNormal);                                        // get a single device
    static const int AllDevices = -1;                                                         // can be used to specify all GPUs in GetDevices() call
    static const int RequeryDevices = -2;                                                     // Requery refreshing statistics and picking the same number as last query
    std::vector<int> GetDevices(int number = AllDevices, BestGpuFlags flags = bestGpuNormal); // get multiple devices
private:
    bool LockDevice(int deviceId, bool trial = true);
};

// DeviceFromConfig - Parse 'deviceId' config parameter to determine what type of behavior is desired
//Symbol - Meaning
// 'auto' - automatically pick a single GPU based on ?BestGpu? score
// 'cpu'  - use the CPU
// 0      - or some other single number, use a single GPU with CUDA ID same as the number
// This can only be called with the same parameters each time, and 'auto' is determined upon first call.
static DEVICEID_TYPE SelectDevice(DEVICEID_TYPE deviceId, bool bLockGPU)
{
    // This can only be called with the same parameter.
    static DEVICEID_TYPE selectedDeviceId = DEVICEID_NOTYETDETERMINED;
    if (selectedDeviceId == DEVICEID_NOTYETDETERMINED)
        selectedDeviceId = deviceId;
    else if (selectedDeviceId != deviceId)
        InvalidArgument("SelectDevice: Attempted to change device selection from %d to %d (%d means 'auto').", (int)selectedDeviceId, (int)deviceId, (int)DEVICEID_AUTO);

    if (deviceId == DEVICEID_AUTO)
    {
        static DEVICEID_TYPE bestDeviceId = DEVICEID_NOTYETDETERMINED;
        // set bestDeviceId once if not set yet
        if (bestDeviceId == DEVICEID_NOTYETDETERMINED)
        {
            // GPU device to be auto-selected, so init our class
            static BestGpu* g_bestGpu = nullptr;
            if (g_bestGpu == nullptr)
                g_bestGpu = new BestGpu();
            bestDeviceId = (DEVICEID_TYPE)g_bestGpu->GetDevice(BestGpuFlags(bLockGPU ? (bestGpuAvoidSharing | bestGpuExclusiveLock) : bestGpuAvoidSharing));
            // TODO: Do we need to hold this pointer at all? We will only query it once. Or is it used to hold lock to a GPU?
        }
        // already chosen
        deviceId = bestDeviceId;
    }

    return deviceId;
}
//#ifdef MATH_EXPORTS
//__declspec(dllexport)
//#endif
DEVICEID_TYPE DeviceFromConfig(const ScriptableObjects::IConfigRecord& config)
{
    bool bLockGPU = config(L"lockGPU", true);
    // we need to deal with the old CNTK config semantics where 'deviceId' can be either a string or an int
    auto valpp = config.Find(L"deviceId");
    if (!valpp)
        return SelectDevice(DEVICEID_AUTO, bLockGPU); // not given at all: default
    auto valp = *valpp;                               // (the type is not determined at this point)
    if (valp.Is<ScriptableObjects::String>())
    {
        wstring val = valp;
        if (val == L"cpu")
            return SelectDevice(CPUDEVICE, false);
        else if (val == L"auto")
            return SelectDevice(DEVICEID_AUTO, bLockGPU);
        else
            InvalidArgument("Invalid value '%ls' for deviceId parameter. Allowed are 'auto' and 'cpu' (case-sensitive).", val.c_str());
    }
    else
        return SelectDevice(valp, bLockGPU);
}
// legacy version for old CNTK config
//#ifdef MATH_EXPORTS
//__declspec(dllexport)
//#endif
DEVICEID_TYPE DeviceFromConfig(const ConfigParameters& config)
{
    ConfigValue val = config("deviceId", "auto");
    bool bLockGPU = config(L"lockGPU", true);

    if      (EqualCI(val, "cpu"))  return SelectDevice(CPUDEVICE,     false);
    else if (EqualCI(val, "auto")) return SelectDevice(DEVICEID_AUTO, bLockGPU);
    else                           return SelectDevice((int) val,     bLockGPU);
}

// !!!!This is from helper_cuda.h which comes with CUDA samples!!!! Consider if it is beneficial to just include all helper_cuda.h
// TODO: This is duplicated in GPUMatrix.cu
// Beginning of GPU Architecture definitions
inline int _ConvertSMVer2Cores(int major, int minor)
{
    // Defines for GPU Architecture types (using the SM version to determine the # of cores per SM
    typedef struct
    {
        int SM; // 0xMm (hexidecimal notation), M = SM Major version, and m = SM minor version
        int Cores;
    } sSMtoCores;

    sSMtoCores nGpuArchCoresPerSM[] =
        {
            {0x10, 8},   // Tesla Generation (SM 1.0) G80 class
            {0x11, 8},   // Tesla Generation (SM 1.1) G8x class
            {0x12, 8},   // Tesla Generation (SM 1.2) G9x class
            {0x13, 8},   // Tesla Generation (SM 1.3) GT200 class
            {0x20, 32},  // Fermi Generation (SM 2.0) GF100 class
            {0x21, 48},  // Fermi Generation (SM 2.1) GF10x class
            {0x30, 192}, // Kepler Generation (SM 3.0) GK10x class
            {0x35, 192}, // Kepler Generation (SM 3.5) GK11x class
            {-1, -1}};

    int index = 0;

    while (nGpuArchCoresPerSM[index].SM != -1)
    {
        if (nGpuArchCoresPerSM[index].SM == ((major << 4) + minor))
        {
            return nGpuArchCoresPerSM[index].Cores;
        }

        index++;
    }

    return nGpuArchCoresPerSM[7].Cores;
}

void BestGpu::GetCudaProperties()
{
    if (m_cudaData)
        return;

    int dev = 0;

    for (ProcessorData* pd : m_procData)
    {
        cudaSetDevice(dev);
        pd->deviceId = dev;
        cudaGetDeviceProperties(&pd->deviceProp, dev);
        size_t free;
        size_t total;
        cudaMemGetInfo(&free, &total);
        pd->cores = _ConvertSMVer2Cores(pd->deviceProp.major, pd->deviceProp.minor) * pd->deviceProp.multiProcessorCount;
        pd->cudaFreeMem = free;
        pd->cudaTotalMem = total;
        dev++;
        cudaDeviceReset();
    }
    m_cudaData = m_procData.size() > 0;
}

void BestGpu::Init()
{
    if (m_initialized)
        return;

    // get the count of objects
    cudaError_t err = cudaGetDeviceCount(&m_deviceCount);
    if (err != cudaSuccess)
        m_deviceCount = 0; // if this fails, we have no GPUs

    ProcessorData pdEmpty = {0};
    for (int i = 0; i < m_deviceCount; i++)
    {
        ProcessorData* data = new ProcessorData();
        *data = pdEmpty;
        m_procData.push_back(data);
    }

    if (m_deviceCount > 0)
    {
        GetCudaProperties();
        GetNvmlData();
    }
    m_initialized = true;
}

BestGpu::~BestGpu()
{
    for (ProcessorData* data : m_procData)
    {
        delete data;
    }
    m_procData.clear();

    if (m_nvmlData)
    {
        nvmlShutdown();
    }
}

// GetNvmlData - Get data from the Nvidia Management Library
void BestGpu::GetNvmlData()
{
    // if we already did this, or we couldn't initialize the CUDA data, skip it
    if (m_nvmlData || !m_cudaData)
        return;

    // First initialize NVML library
    nvmlReturn_t result = nvmlInit();
    if (NVML_SUCCESS != result)
    {
        return;
    }

    QueryNvmlData();
}

// GetDevice - Determine the best device ID to use
// bestFlags - flags that modify how the score is calculated
int BestGpu::GetDevice(BestGpuFlags bestFlags)
{
    std::vector<int> best = GetDevices(1, bestFlags);
    return best[0];
}

// SetAllowedDevices - set the allowed devices array up
// devices - vector of allowed devices
void BestGpu::SetAllowedDevices(const std::vector<int>& devices)
{
    m_allowedDevices = 0;
    for (int device : devices)
    {
        m_allowedDevices |= (1 << device);
    }
}

// DeviceAllowed - is a particular device allowed?
// returns: true if the device is allowed, otherwise false
bool BestGpu::DeviceAllowed(int device)
{
    return !!(m_allowedDevices & (1 << device));
}

// AllowAll - Reset the allowed filter to allow all GPUs
void BestGpu::AllowAll()
{
    m_allowedDevices = -1; // set all bits
}

// UseMultiple - Are we using multiple GPUs?
// returns: true if more than one GPU was returned in last call
bool BestGpu::UseMultiple()
{
    return m_lastCount > 1;
}

// GetDevices - Determine the best device IDs to use
// number - how many devices do we want?
// bestFlags - flags that modify how the score is calculated
std::vector<int> BestGpu::GetDevices(int number, BestGpuFlags p_bestFlags)
{
    BestGpuFlags bestFlags = p_bestFlags;

    // if they want all devices give them eveything we have
    if (number == AllDevices)
        number = std::max(m_deviceCount, 1);
    else if (number == RequeryDevices)
    {
        number = m_lastCount;
    }

    // create the initial array, initialized to all CPU
    std::vector<int> best(m_deviceCount, -1);
    std::vector<double> scores(m_deviceCount, -1.0);

    // if no GPUs were found, we should use the CPU
    if (m_procData.size() == 0)
    {
        best.clear();
        best.push_back(-1); // default to CPU
        return best;
    }

    // get latest data
    QueryNvmlData();

    double utilGpuW = 0.15;
    double utilMemW = 0.1;
    double speedW = 0.2;
    double freeMemW = 0.2;
    double mlAppRunningW = 0.2;

    // if it's a requery, just use the same flags as last time
    if (bestFlags & bestGpuRequery)
        bestFlags = m_lastFlags;

    // adjust weights if necessary
    if (bestFlags & bestGpuAvoidSharing)
    {
        mlAppRunningW *= 3;
    }
    if (bestFlags & bestGpuFavorMemory) // favor memory
    {
        freeMemW *= 2;
    }
    if (bestFlags & bestGpuFavorUtilization) // favor low utilization
    {
        utilGpuW *= 2;
        utilMemW *= 2;
    }
    if (bestFlags & bestGpuFavorSpeed) // favor fastest processor
    {
        speedW *= 2;
    }

    for (ProcessorData* pd : m_procData)
    {
        double score = 0.0;

        if (!DeviceAllowed(pd->deviceId))
            continue;

        // GPU utilization score
        score = (1.0 - pd->utilization.gpu / 75.0f) * utilGpuW;
        score += (1.0 - pd->utilization.memory / 60.0f) * utilMemW;
        score += pd->cores / 1000.0f * speedW;
        double mem = pd->memory.total > 0 ? pd->memory.free / (double) pd->memory.total : 1000000; // I saw this to be 0 when remoted in
        // if it's not a tcc driver, then it's WDDM driver and values will be off because windows allocates all the memory from the nvml point of view
        if (!pd->deviceProp.tccDriver || pd->memory.total == 0)
            mem = pd->cudaFreeMem / (double) pd->cudaTotalMem;
        score += mem * freeMemW;
        score += (pd->cntkFound ? 0 : 1) * mlAppRunningW;
        for (int i = 0; i < best.size(); i++)
        {
            // look for a better score
            if (score > scores[i])
            {
                // make room for this score in the correct location (insertion sort)
                for (int j = (int) best.size() - 1; j > i; --j)
                {
                    scores[j] = scores[j - 1];
                    best[j] = best[j - 1];
                }
                scores[i] = score;
                best[i] = pd->deviceId;
                break;
            }
        }
    }

    // now get rid of any extra empty slots and disallowed devices
    for (int j = (int) best.size() - 1; j > 0; --j)
    {
        // if this device is not allowed, or never was set remove it
        if (best[j] == -1)
            best.pop_back();
        else
            break;
    }

    // global lock for this process
    CrossProcessMutex deviceAllocationLock("DBN.exe GPGPU querying lock");

    if (!deviceAllocationLock.Acquire((bestFlags & bestGpuExclusiveLock) != 0)) // failure  --this should not really happen
        RuntimeError("DeviceFromConfig: Unexpected failure acquiring device allocation lock.");

    {
        // even if user do not want to lock the GPU, we still need to check whether a particular GPU is locked or not,
        // to respect other users' exclusive lock.

        vector<int> bestAndAvaialbe;
        for (auto i : best)
        {
            if (LockDevice(i, true))
            {
                // available
                bestAndAvaialbe.push_back(i);
            }
        }
        best = bestAndAvaialbe;
        if (best.size() > number)
        {
            best.resize(number);
        }
    }

    // save off the last values for future requeries
    m_lastFlags = bestFlags;
    m_lastCount = (int) best.size();

    // if we eliminated all GPUs, use CPU
    if (best.size() == 0)
    {
        best.push_back(-1);
    }

    for (int z = 0; z < best.size() && z < number; z++)
    {
        LockDevice(best[z], false);
    }

    return best; // return the array of the best GPUs
}

// QueryNvmlData - Query data from the Nvidia Management Library, and accumulate counters,
// In case failure, this function simply backs out without filling in the data structure and without setting m_nvmlData.
void BestGpu::QueryNvmlData()
{
    if (!m_cudaData)
        return;

    for (int i = 0; i < m_deviceCount; i++)
    {
        nvmlDevice_t device;
        nvmlPciInfo_t pci;
        nvmlMemory_t memory;
        nvmlUtilization_t utilization;

        // Query for device handle to perform operations on a device
        nvmlReturn_t result = nvmlDeviceGetHandleByIndex(i, &device);
        if (NVML_SUCCESS != result)
            return; // failed: just back out

        // pci.busId is very useful to know which device physically you're talking to
        // Using PCI identifier you can also match nvmlDevice handle to CUDA device.
        result = nvmlDeviceGetPciInfo(device, &pci);
        if (NVML_SUCCESS != result)
            return;

        ProcessorData* curPd = NULL;
        for (ProcessorData* pd : m_procData)
        {
            if (pd->deviceProp.pciBusID == (int) pci.bus)
            {
                curPd = pd;
                break;
            }
        }

        if (curPd == NULL)
            continue;

        // Get the memory usage, will only work for TCC drivers
        result = nvmlDeviceGetMemoryInfo(device, &memory);
        if (NVML_SUCCESS != result)
            return;
        curPd->memory = memory;

        // Get the memory usage, will only work for TCC drivers
        result = nvmlDeviceGetUtilizationRates(device, &utilization);
        if (NVML_SUCCESS != result)
            return;
        if (m_queryCount)
        {
            // average, slightly overweighting the most recent query
            curPd->utilization.gpu    = (curPd->utilization.gpu    * m_queryCount + utilization.gpu    * 2) / (m_queryCount + 2);
            curPd->utilization.memory = (curPd->utilization.memory * m_queryCount + utilization.memory * 2) / (m_queryCount + 2);
        }
        else
        {
            curPd->utilization = utilization;
        }
        m_queryCount++;

        unsigned int size = 0;
        result = nvmlDeviceGetComputeRunningProcesses(device, &size, NULL);
        if (size > 0)
        {
            std::vector<nvmlProcessInfo_t> processInfo(size);
            processInfo.resize(size);
            for (nvmlProcessInfo_t info : processInfo)
                info.usedGpuMemory = 0;
            result = nvmlDeviceGetComputeRunningProcesses(device, &size, &processInfo[0]);
            if (NVML_SUCCESS != result)
                return;
            bool cntkFound = false;
            for (nvmlProcessInfo_t info : processInfo)
            {
                std::string name;
                name.resize(256);
                unsigned len = (unsigned) name.length();
                nvmlSystemGetProcessName(info.pid, (char*) name.data(), len);
                name.resize(strlen(name.c_str()));
                size_t pos = name.find_last_of(PATH_DELIMITER);
                if (pos != std::string::npos)
                    name = name.substr(pos + 1);
                if (GetCurrentProcessId() == info.pid || name.length() == 0)
                    continue;
#ifdef _WIN32
                cntkFound = cntkFound || EqualCI(name, "cntk.exe"); // recognize ourselves
                cntkFound = cntkFound || EqualCI(name, "cn.exe") || EqualCI(name, "dbn.exe"); // also recognize some MS-proprietary legacy tools
#else
                cntkFound = cntkFound || name == "cntk"; // (Linux is case sensitive)
#endif
            }
            // set values to save
            curPd->cntkFound = cntkFound;
        }
    }
    m_nvmlData = true;
    return;
}

bool BestGpu::LockDevice(int deviceId, bool trial)
{
    if (deviceId < 0) // don't lock CPU, always return true
    {
        return true;
    }
    // ported from dbn.exe, not perfect but it works in practice
    char buffer[80];
    sprintf(buffer, "DBN.exe GPGPU exclusive lock for device %d", deviceId);
    std::unique_ptr<CrossProcessMutex> mutex(new CrossProcessMutex(buffer));
    if (!mutex->Acquire(/*wait=*/false)) // GPU not available
    {
        fprintf(stderr, "LockDevice: Failed to lock GPU %d for exclusive use.\n", deviceId);
        return false;
    }
    else
    {
        fprintf(stderr, "LockDevice: Locked GPU %d %s.\n", deviceId, trial ? "to test availability" : "for exclusive use");
        if (!trial)
            m_GPUMutex[deviceId] = std::move(mutex);
        else
            fprintf(stderr, "LockDevice: Unlocked GPU %d after testing.\n", deviceId);
    }
    return true;
}

#ifdef _WIN32

#if 0
// ---------------------------------------------------------------------------
// some interfacing with the Windows DLL system for finding nvml.dll if not in PATH
// Not needed since the build process copies it.
// ---------------------------------------------------------------------------

// The "notify hook" gets called for every call to the
// Delay load helper.  This allows a user to hook every call and
// skip the Delay load helper entirely.
//
// dliNotify == { dliStartProcessing | dliNotePreLoadLibrary  | dliNotePreGetProc | dliNoteEndProcessing } on this call.

extern "C" INT_PTR WINAPI DelayLoadNotify(
    unsigned        dliNotify,
    PDelayLoadInfo  pdli
    )
{
    // load nvml.dll from an alternate path
    if (dliNotify == dliNotePreLoadLibrary && !strcmp(pdli->szDll, "nvml.dll"))
    {
        WCHAR *path;
        WCHAR nvmlPath[MAX_PATH] = { 0 };
        HRESULT hr = SHGetKnownFolderPath(FOLDERID_ProgramFiles, 0, NULL, &path);
        lstrcpy(nvmlPath, path);
        CoTaskMemFree(path);
        if (SUCCEEDED(hr))
        {
            HMODULE module = NULL;
            WCHAR* dllName = L"\\NVIDIA Corporation\\NVSMI\\nvml.dll";
            lstrcat(nvmlPath, dllName);
            module = LoadLibraryEx(nvmlPath, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
            return (INT_PTR)module;
        }
    }
    // check for failed GetProc, old version of the driver
    if (dliNotify == dliFailGetProc && !strcmp(pdli->szDll, "nvml.dll"))
    {
        char name[256];
        size_t len = strlen(pdli->dlp.szProcName);
        strcpy_s(name, pdli->dlp.szProcName);
        // if the version 2 APIs are not supported, truncate "_v2"
        if (len>3 && name[len-1] == '2')
            name[len-3] = 0;
        FARPROC pfnRet = ::GetProcAddress(pdli->hmodCur, name);
        return (INT_PTR)pfnRet;
    }

    return NULL;
}

ExternC
PfnDliHook __pfnDliNotifyHook2 = (PfnDliHook)DelayLoadNotify;
// This is the failure hook, dliNotify = {dliFailLoadLib|dliFailGetProc}
ExternC
PfnDliHook   __pfnDliFailureHook2 = (PfnDliHook)DelayLoadNotify;
#endif // _WIN32
#endif

}}}

#endif // CPUONLY
