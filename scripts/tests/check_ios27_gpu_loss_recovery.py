#!/usr/bin/env python3
"""Exercise renderer recovery while its caller blocks and SIGTERM is ignored."""
from pathlib import Path
import os
import subprocess
import time
repo=Path(__file__).resolve().parents[2]
out=repo.parents[1]/'outputs/UniXNU_marble/ios27_24B119/host-tests/gpu-loss-recovery';out.mkdir(parents=True,exist_ok=True)
s=(repo/'ios27/sources/AdrenoMetal/NativeBridgeMetalFrontend.mm').read_text();start=s.index('static void endRenderServerAfterGpuLoss()');end=s.index('\n}',start)+2
head='''#include <atomic>\n#include <unistd.h>\n#include <stdlib.h>\n#include <signal.h>\n#include <dispatch/dispatch.h>\n#define NBLOG(...) ((void)0)\nstatic bool isRenderServer(){return getenv("MARBLE_RECOVERY_TEST_SERVER")!=nullptr;}\n'''
head += "#include <string.h>\nstatic const char *detail(){return \"retained Vulkan loss\";}\nstatic const char *(*gBackendError)()=detail;\nstatic void report(const char *value){write(1,value,strlen(value));}\nstatic void (*native_bridge_metal_loss_hook)(const char *)=report;\n"
tail='''\nint main(){signal(SIGTERM,SIG_IGN);endRenderServerAfterGpuLoss();endRenderServerAfterGpuLoss();usleep(3000000);return 0;}\n'''
(out/'test.mm').write_text(head+s[start:end]+tail)
subprocess.run(['clang++','-std=c++17','-fblocks',str(out/'test.mm'),'-o',str(out/'test')],check=True)
start=time.monotonic();r=subprocess.run([str(out/'test')],env=dict(os.environ,MARBLE_RECOVERY_TEST_SERVER='1'),timeout=2,capture_output=True,text=True);elapsed=time.monotonic()-start
assert r.stdout=='retained Vulkan loss' and r.returncode==1 and .35<elapsed<2,(r.returncode,elapsed)
r=subprocess.run([str(out/'test')],env={k:v for k,v in os.environ.items() if k!='MARBLE_RECOVERY_TEST_SERVER'},timeout=4);assert r.returncode==0
print('PASS: retained loss reported; blocked renderer exits after recovery deadline despite ignored SIGTERM; other process stays running')
