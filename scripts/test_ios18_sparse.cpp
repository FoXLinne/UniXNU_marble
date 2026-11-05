#include "SparseDisk.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <fstream>
#include <iterator>
#include <vector>
struct Ops {
 std::vector<uint8_t> buffer;
 uint8_t *allocate() { return static_cast<uint8_t *>(calloc(1,4096)); }
 uint64_t read(uint64_t at,void *p,uint64_t n) { memcpy(p,buffer.data()+at,n);return n; }
 uint64_t copy(uint64_t at,const void *p,uint64_t n) { memcpy(buffer.data()+at,p,n);return n; }
};
int main(int argc,char **argv) {
 assert(argc==2);std::ifstream source(argv[1],std::ios::binary);
 std::vector<uint8_t> packed((std::istreambuf_iterator<char>(source)),std::istreambuf_iterator<char>());
 uint64_t logical=0;assert(ramPackedLayout(packed.data(),packed.size(),&logical)&&logical==0xc0000000ULL);
 RamSparseDisk data={static_cast<uint8_t **>(calloc(logical/4096,sizeof(uint8_t*))),logical,0};Ops ops;
 assert(ramSparseImport(data,packed.data(),packed.size(),ops));uint64_t imported=data.resident;
 for(uint64_t offset: {0x80000000ULL+4090,0xc0000000ULL-512}) {
  ops.buffer.assign(32,0x5a);bool oom=false;
  assert(ramSparseIO(data,true,offset,32,ops,oom)==32&&!oom);
  ops.buffer.assign(32,0);assert(ramSparseIO(data,false,offset,32,ops,oom)==32&&!oom);
  for(auto value:ops.buffer)assert(value==0x5a);
 }
 printf("PASS: actual Data seed %llu encoded bytes, %llu logical bytes, %llu imported resident bytes; cross-page I/O above 2 GiB and final-sector I/O\n",(unsigned long long)packed.size(),(unsigned long long)logical,(unsigned long long)imported);
 for(uint64_t i=0;i<logical/4096;i++)free(data.pages[i]);free(data.pages);
}
