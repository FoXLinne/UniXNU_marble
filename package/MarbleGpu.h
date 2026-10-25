// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once

// Stage numbers are shared by the log report and the on-screen diagnostic.
enum MarbleGpuStage : unsigned {
    MarbleGpuCx = 10,
    MarbleGpuProbe = 11,
    MarbleGpuGmu = 20,
    MarbleGpuGmuMemory = 21,
    MarbleGpuGmuFirmware = 22,
    MarbleGpuGmuBoot = 23,
    MarbleGpuHfi = 24,
    MarbleGpuVotes = 25,
    MarbleGpuGx = 26,
    MarbleGpuContextAllocate = 31,
    MarbleGpuContextPrepare = 32,
    MarbleGpuContextProgram = 33,
    MarbleGpuContextAttach = 34,
    MarbleGpuContextVm = 35,
    MarbleGpuContextMap = 36,
    MarbleGpuContextReady = 37,
    MarbleGpuContext = 40,
    MarbleGpuCpFirmware = 41,
    MarbleGpuCpRegisters = 42,
    MarbleGpuCpRing = 43,
    MarbleGpuCpInit = 44,
    MarbleGpuCpScratch = 45,
    MarbleGpuZap = 46,
    MarbleGpuSecureExit = 47,
    MarbleGpuPublish = 50,
    MarbleGpuFamily = 51,
    MarbleGpuMetaclass = 52,
    MarbleGpuAccelerator = 53,
    MarbleGpuRegister = 54,
    MarbleGpuReady = 60
};

extern "C" void native_marble_gpu_checkpoint(unsigned stage, int status = 0);
extern "C" void native_marble_gpu_failed(int status);
extern "C" void native_marble_gpu_report(char *text, unsigned capacity);
extern "C" bool native_marble_gmu_ready(void);
