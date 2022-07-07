/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl

#include "mongo/platform/basic.h"

#include <bitset>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <iostream>
#include <psapi.h>

#include "mongo/logv2/log.h"
#include "mongo/util/processinfo.h"

namespace mongo {

namespace {

using Slpi = SYSTEM_LOGICAL_PROCESSOR_INFORMATION;
using SlpiBuf = std::aligned_storage_t<sizeof(Slpi)>;

struct LpiRecords {
    const Slpi* begin() const {
        return reinterpret_cast<const Slpi*>(slpiRecords.get());
    }

    const Slpi* end() const {
        return begin() + count;
    }

    std::unique_ptr<SlpiBuf[]> slpiRecords;
    size_t count;
};

// Both the body of this getLogicalProcessorInformationRecords and the callers of
// getLogicalProcessorInformationRecords are largely modeled off of the example code at
// https://docs.microsoft.com/en-us/windows/win32/api/sysinfoapi/nf-sysinfoapi-getlogicalprocessorinformation
LpiRecords getLogicalProcessorInformationRecords() {

    DWORD returnLength = 0;
    LpiRecords lpiRecords{};

    DWORD returnCode = 0;
    do {
        returnCode = GetLogicalProcessorInformation(
            reinterpret_cast<Slpi*>(lpiRecords.slpiRecords.get()), &returnLength);
        if (returnCode == FALSE) {
            if (GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
                lpiRecords.slpiRecords = std::unique_ptr<SlpiBuf[]>(
                    new SlpiBuf[((returnLength - 1) / sizeof(Slpi)) + 1]);
            } else {
                DWORD gle = GetLastError();
                LOGV2_WARNING(23811,
                              "GetLogicalProcessorInformation failed",
                              "error"_attr = errnoWithDescription(gle));
                return LpiRecords{};
            }
        }
    } while (returnCode == FALSE);


    lpiRecords.count = returnLength / sizeof(Slpi);
    return lpiRecords;
}

int getPhysicalCores() {
    int processorCoreCount = 0;
    for (auto&& lpi : getLogicalProcessorInformationRecords()) {
        if (lpi.Relationship == RelationProcessorCore)
            processorCoreCount++;
    }
    return processorCoreCount;
}

}  // namespace
int _wconvertmtos(SIZE_T s) {
    return (int)(s / (1024 * 1024));
}

ProcessInfo::ProcessInfo(ProcessId pid) {}

ProcessInfo::~ProcessInfo() {}

// get the number of CPUs available to the current process
boost::optional<unsigned long> ProcessInfo::getNumCoresForProcess() {
    DWORD_PTR process_mask, system_mask;

    if (!GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask))
        return boost::none;

    std::bitset<sizeof(process_mask) * 8> mask(process_mask);
    auto num = mask.count();
    if (num == 0)
        return boost::none;

    // If we are running in a Windows Container using process isolation this process is
    // associated with a job object we can query for the cpu limit
    // https://docs.microsoft.com/en-us/virtualization/windowscontainers/manage-containers/resource-controls
    // https://docs.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-jobobject_cpu_rate_control_information
    JOBOBJECT_CPU_RATE_CONTROL_INFORMATION cpuInfo;
    ZeroMemory(&cpuInfo, sizeof(cpuInfo));
    if (QueryInformationJobObject(
            NULL, JobObjectCpuRateControlInformation, &cpuInfo, sizeof(cpuInfo), NULL) &&
        (cpuInfo.ControlFlags & JOB_OBJECT_CPU_RATE_CONTROL_ENABLE) &&
        (cpuInfo.ControlFlags & JOB_OBJECT_CPU_RATE_CONTROL_HARD_CAP)) {
        // CpuRate is a percentage times 100
        num = std::ceil(num * (static_cast<double>(cpuInfo.CpuRate) / 10000));
    }

    return num;
}

bool ProcessInfo::supported() {
    return true;
}

int ProcessInfo::getVirtualMemorySize() {
    MEMORYSTATUSEX mse;
    mse.dwLength = sizeof(mse);
    BOOL status = GlobalMemoryStatusEx(&mse);
    if (!status) {
        DWORD gle = GetLastError();
        LOGV2_ERROR(23812, "GlobalMemoryStatusEx failed", "error"_attr = errnoWithDescription(gle));
        fassert(28621, status);
    }

    DWORDLONG x = (mse.ullTotalVirtual - mse.ullAvailVirtual) / (1024 * 1024);
    invariant(x <= 0x7fffffff);
    return (int)x;
}

int ProcessInfo::getResidentSize() {
    PROCESS_MEMORY_COUNTERS pmc;
    BOOL status = GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc));
    if (!status) {
        DWORD gle = GetLastError();
        LOGV2_ERROR(23813, "GetProcessMemoryInfo failed", "error"_attr = errnoWithDescription(gle));
        fassert(28622, status);
    }

    return _wconvertmtos(pmc.WorkingSetSize);
}

void ProcessInfo::getExtraInfo(BSONObjBuilder& info) {
    MEMORYSTATUSEX mse;
    mse.dwLength = sizeof(mse);
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        info.append("page_faults", static_cast<int>(pmc.PageFaultCount));
        info.append("usagePageFileMB", static_cast<int>(pmc.PagefileUsage / 1024 / 1024));
    }
    if (GlobalMemoryStatusEx(&mse)) {
        info.append("totalPageFileMB", static_cast<int>(mse.ullTotalPageFile / 1024 / 1024));
        info.append("availPageFileMB", static_cast<int>(mse.ullAvailPageFile / 1024 / 1024));
        info.append("ramMB", static_cast<int>(mse.ullTotalPhys / 1024 / 1024));
    }

#ifndef _WIN64
    BOOL wow64Process;
    BOOL retWow64 = IsWow64Process(GetCurrentProcess(), &wow64Process);
    info.append("wow64Process", static_cast<bool>(retWow64 && wow64Process));
#endif
}

bool getFileVersion(const char* filePath, DWORD& fileVersionMS, DWORD& fileVersionLS) {
    DWORD verSize = GetFileVersionInfoSizeA(filePath, NULL);
    if (verSize == 0) {
        DWORD gle = GetLastError();
        LOGV2_WARNING(23807,
                      "GetFileVersionInfoSizeA failed",
                      "path"_attr = filePath,
                      "error"_attr = errnoWithDescription(gle));
        return false;
    }

    std::unique_ptr<char[]> verData(new char[verSize]);
    if (GetFileVersionInfoA(filePath, NULL, verSize, verData.get()) == 0) {
        DWORD gle = GetLastError();
        LOGV2_WARNING(23808,
                      "GetFileVersionInfoSizeA failed",
                      "path"_attr = filePath,
                      "error"_attr = errnoWithDescription(gle));
        return false;
    }

    UINT size;
    VS_FIXEDFILEINFO* verInfo;
    if (VerQueryValueA(verData.get(), "\\", (LPVOID*)&verInfo, &size) == 0) {
        DWORD gle = GetLastError();
        LOGV2_WARNING(23809,
                      "VerQueryValueA failed",
                      "path"_attr = filePath,
                      "error"_attr = errnoWithDescription(gle));
        return false;
    }

    if (size != sizeof(VS_FIXEDFILEINFO)) {
        LOGV2_WARNING(23810,
                      "VerQueryValueA returned structure with unexpected size",
                      "path"_attr = filePath);
        return false;
    }

    fileVersionMS = verInfo->dwFileVersionMS;
    fileVersionLS = verInfo->dwFileVersionLS;
    return true;
}

void ProcessInfo::SystemInfo::collectSystemInfo() {
    BSONObjBuilder bExtra;
    std::stringstream verstr;
    OSVERSIONINFOEX osvi;   // os version
    MEMORYSTATUSEX mse;     // memory stats
    SYSTEM_INFO ntsysinfo;  // system stats

    // get basic processor properties
    GetNativeSystemInfo(&ntsysinfo);
    addrSize = (ntsysinfo.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64 ? 64 : 32);
    numCores = ntsysinfo.dwNumberOfProcessors;
    numPhysicalCores = getPhysicalCores();
    pageSize = static_cast<unsigned long long>(ntsysinfo.dwPageSize);
    bExtra.append("pageSize", static_cast<long long>(pageSize));
    bExtra.append("physicalCores", static_cast<int>(numPhysicalCores));

    // get memory info
    mse.dwLength = sizeof(mse);
    if (GlobalMemoryStatusEx(&mse)) {
        memSize = mse.ullTotalPhys;

        // If we are running in a Windows Container using process isolation this process is
        // associated with a job object we can query for the memory limit
        // https://docs.microsoft.com/en-us/virtualization/windowscontainers/manage-containers/resource-controls
        // https://docs.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-jobobject_extended_limit_information
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jobInfo;
        ZeroMemory(&jobInfo, sizeof(jobInfo));
        if (QueryInformationJobObject(
                NULL, JobObjectExtendedLimitInformation, &jobInfo, sizeof(jobInfo), NULL) &&
            (jobInfo.BasicLimitInformation.LimitFlags & JOB_OBJECT_LIMIT_JOB_MEMORY) &&
            jobInfo.JobMemoryLimit != 0) {
            memLimit = jobInfo.JobMemoryLimit;
        } else {
            memLimit = memSize;
        }
    }

    // get OS version info
    ZeroMemory(&osvi, sizeof(osvi));
    osvi.dwOSVersionInfoSize = sizeof(osvi);
#pragma warning(push)
// GetVersionEx is deprecated
#pragma warning(disable : 4996)
    if (GetVersionEx((OSVERSIONINFO*)&osvi)) {
#pragma warning(pop)

        verstr << osvi.dwMajorVersion << "." << osvi.dwMinorVersion;
        if (osvi.wServicePackMajor)
            verstr << " SP" << osvi.wServicePackMajor;
        verstr << " (build " << osvi.dwBuildNumber << ")";

        osName = "Microsoft ";
        switch (osvi.dwMajorVersion) {
            case 10:
                if (osvi.wProductType == VER_NT_WORKSTATION)
                    osName += "Windows 10";
                else {
                    // The only way to tell apart Windows Server versions is via build number
                    if (osvi.dwBuildNumber >= 17763) {
                        osName += "Windows Server 2019";
                    } else {
                        osName += "Windows Server 2016";
                    }
                }
                break;
            case 6:
                switch (osvi.dwMinorVersion) {
                    case 3:
                        if (osvi.wProductType == VER_NT_WORKSTATION)
                            osName += "Windows 8.1";
                        else
                            osName += "Windows Server 2012 R2";
                        break;
                    case 2:
                        if (osvi.wProductType == VER_NT_WORKSTATION)
                            osName += "Windows 8";
                        else
                            osName += "Windows Server 2012";
                        break;
                    case 1:
                        if (osvi.wProductType == VER_NT_WORKSTATION)
                            osName += "Windows 7";
                        else
                            osName += "Windows Server 2008 R2";
                        break;
                    case 0:
                        if (osvi.wProductType == VER_NT_WORKSTATION)
                            osName += "Windows Vista";
                        else
                            osName += "Windows Server 2008";
                        break;
                    default:
                        osName += "Windows NT version ";
                        osName += verstr.str();
                        break;
                }
                break;
            default:
                osName += "Windows";
                break;
        }
    } else {
        // unable to get any version data
        osName += "Windows NT";
    }

    if (ntsysinfo.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64) {
        cpuArch = "x86_64";
    } else if (ntsysinfo.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_INTEL) {
        cpuArch = "x86";
    } else if (ntsysinfo.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_IA64) {
        cpuArch = "ia64";
    } else {
        cpuArch = "unknown";
    }

    osType = "Windows";
    osVersion = verstr.str();
    hasNuma = checkNumaEnabled();
    _extraStats = bExtra.obj();
}


bool ProcessInfo::checkNumaEnabled() {
    DWORD numaNodeCount = 0;
    for (auto&& lpi : getLogicalProcessorInformationRecords()) {
        if (lpi.Relationship == RelationNumaNode)
            // Non-NUMA systems report a single record of this type.
            ++numaNodeCount;
    }

    // For non-NUMA machines, the count is 1
    return numaNodeCount > 1;
}

}  // namespace mongo
