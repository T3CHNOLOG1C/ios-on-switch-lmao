#include <fstream>
#include <iterator>
#include <thread>
#include "../NativeRamBlock.cpp"
extern "C" {unsigned long gPhysBase=0x80000000ULL,gPhysSize=0x10000000ULL;}
static std::vector<uint8_t> original;
static boot_args args;
static NativeRamBlock *driver=nullptr;
static void property(IOService *object,const char *key,const char *text){auto *s=OSString::withCString(text);object->setProperty(key,s);s->release();}
static void range(const char *key,uint64_t physical=0x80020000ULL,uint64_t bytes=kNativeDataSeedEncodedBytes,unsigned length=16){
 uint64_t pair[]={physical,bytes};auto *d=OSData::withBytes(pair,length);IOService::memoryMap->setProperty(key,d);d->release();
}
static void setup(bool present=true){
 mappedSeed=original;args.topOfKernelData=0x80020000ULL+((original.size()+4095)&~4095ULL);PE_state.bootArgs=&args;
 IOService::resource=new IOService;IOService::memoryMap=new IOService;
 if(present)range("NativeDataSeed");
 range("RAMDisk",UINT64_MAX,UINT64_MAX);range("PackedRAMDisk",1,1);
 driver=new NativeRamBlock;assert(driver->attach(IOService::resource));
}
static void cleanup(){
 for(auto *m:IOService::candidates){m->detach(m->getProvider());m->release();}IOService::candidates.clear();
 driver->stop(IOService::resource);
 for(auto *d:IOService::registered)d->release();IOService::registered.clear();
 driver->detach(IOService::resource);driver->release();driver=nullptr;
 IOService::memoryMap->release();IOService::memoryMap=nullptr;
 IOService::resource->release();IOService::resource=nullptr;
 assert(!mallocLive&&allocations.empty()&&!lockLive&&!heldLocks&&!OSObject::live);
 allocationCalls=failAllocationCall=attachCalls=failAttachCall=startCalls=failStartCall=0;
 propertyCalls=failPropertyCall=superclassStops=mallocCalls=failMallocCall=0;
 lockCalls=failLockCall=0;IOService::nativeSeedLookups=IOService::legacySeedLookups=0;
 native_ram_block_stage=0;
 native_ram_block_error=0;
 native_ram_block_ready=0;
 native_ram_block_sparse=0;
 native_ram_block_bytes=0;
 native_ram_block_reads=0;
 native_ram_block_writes=0;
 native_ram_block_failures=0;
 native_ram_block_resident_bytes=0;
}
static void start(){assert(driver->start(IOService::resource));assert(native_ram_block_ready&&native_ram_block_stage==4&&!native_ram_block_error);assert(IOService::legacySeedLookups==0&&IOService::nativeSeedLookups==1);}
struct Result {unsigned calls=0;IOReturn status=99;uint64_t bytes=99;};
static void complete(void *ctx,void *,IOReturn status,UInt64 bytes){assert(!heldLocks);auto &r=*static_cast<Result *>(ctx);++r.calls;r.status=status;r.bytes=bytes;}
static IOStorageCompletion completion(Result &r){return {&r,complete,nullptr};}
static Result request(IOMemoryDescriptor &bytes,uint64_t block,uint64_t count){Result r;auto c=completion(r);assert(driver->doAsyncReadWrite(&bytes,block,count,nullptr,&c)==0&&r.calls==1);return r;}
static uint8_t expectedByte(uint64_t at){
 uint64_t count=ramU64(original.data()+24),cursor=32+count*16;
 for(uint64_t i=0;i<count;++i){uint64_t start=ramU64(original.data()+32+i*16),n=ramU64(original.data()+40+i*16);
  if(at>=start&&at-start<n)return original[cursor+at-start];cursor+=n;}
 return 0;
}
static void basic(){
 setup();start();assert(native_ram_block_bytes==kNativeDataSeedLogicalBytes&&native_ram_block_resident_bytes==kNativeDataSeedNonzeroPages*4096);
 assert(!memcmp(native_ram_block_seed_sha256,kNativeDataSeedLogicalSHA256,32));
 for(uint64_t offset:std::vector<uint64_t>{0,4096,16384,65536,1ULL<<20,1ULL<<28,kNativeDataSeedLogicalBytes-65536}){
  IOMemoryDescriptor b(65536);auto r=request(b,offset/512,128);assert(!r.status&&r.bytes==65536);
  for(unsigned i=0;i<b.bytes.size();++i)assert(b.bytes[i]==expectedByte(offset+i));
 }
 uint64_t n=0;bool b=false;assert(driver->reportBlockSize(&n)==0&&n==512);
 assert(driver->reportMaxValidBlock(&n)==0&&n==kNativeDataSeedLogicalBytes/512-1);
 assert(driver->reportWriteProtection(&b)==0&&!b);assert(driver->getWriteCacheState(&b)==0&&!b);
 assert(driver->reportMediaState(&b,nullptr)==0&&b);assert(driver->doSynchronizeCache()==0);
 for(const char *key:{kIOMaximumByteCountReadKey,kIOMaximumByteCountWriteKey})assert(dynamic_cast<OSNumber *>(driver->getProperty(key))->unsigned64BitValue()==65536);
 cleanup();
}
static void badStartup(){
 setup(false);assert(!driver->start(IOService::resource)&&native_ram_block_error==3&&IOService::legacySeedLookups==0);cleanup();
 for(unsigned i=0;i<8;++i){setup();
  if(i==0)range("NativeDataSeed",0x80020001);
  if(i==1)range("NativeDataSeed",0x80020000,kNativeDataSeedEncodedBytes-1);
  if(i==2)range("NativeDataSeed",0x80020000,kNativeDataSeedEncodedBytes,8);
  if(i==3)range("NativeDataSeed",0x7ffff000);
  if(i==4)range("NativeDataSeed",0x90000000);
  if(i==5)args.topOfKernelData-=4096;
  if(i==6)PE_state.bootArgs=nullptr;
  if(i==7)mappedSeed[90]^=1;
  assert(!driver->start(IOService::resource)&&!native_ram_block_ready&&IOService::registered.empty());
  assert(!driver->start(IOService::resource)); // same boot never retries
  cleanup();
 }
 // Failure at each allocation point, including first/middle/final imported page.
 for(unsigned call:std::vector<unsigned>{1,2,3,20,static_cast<unsigned>(kNativeDataSeedNonzeroPages+2)}){
  setup();failMallocCall=call;assert(!driver->start(IOService::resource)&&native_ram_block_error==6&&IOService::registered.empty()&&!mallocLive&&!lockLive);cleanup();
 }
 setup();failLockCall=1;assert(!driver->start(IOService::resource)&&native_ram_block_error==6);cleanup();
 setup();failStartCall=1;assert(!driver->start(IOService::resource)&&native_ram_block_error==1);cleanup();
 for(unsigned call=1;call<=4;++call){setup();failPropertyCall=call;assert(!driver->start(IOService::resource)&&native_ram_block_error==7&&!mallocLive&&!lockLive);cleanup();}
 setup();start();auto *second=new NativeRamBlock;assert(!second->start(IOService::resource));second->release();assert(native_ram_block_ready);cleanup();
}
static void bounds(){
 setup();start();IOMemoryDescriptor b(65536);Result r;auto c=completion(r);
 assert(driver->doAsyncReadWrite(nullptr,0,1,nullptr,&c)==kIOReturnBadArgument);
 assert(driver->doAsyncReadWrite(&b,0,1,nullptr,nullptr)==kIOReturnBadArgument);
 auto bad=c;bad.action=nullptr;assert(driver->doAsyncReadWrite(&b,0,1,nullptr,&bad)==kIOReturnBadArgument);
 for(auto pair:std::vector<std::pair<uint64_t,uint64_t>>{{0,0},{0,129},{UINT64_MAX,1},{1,UINT64_MAX},{kNativeDataSeedLogicalBytes/512,1},{kNativeDataSeedLogicalBytes/512-1,2}})
  assert(driver->doAsyncReadWrite(&b,pair.first,pair.second,nullptr,&c)==kIOReturnBadArgument);
 IOMemoryDescriptor shortB(511);assert(driver->doAsyncReadWrite(&shortB,0,1,nullptr,&c)==kIOReturnBadArgument);
 for(unsigned direction:{kIODirectionInOut,0u}){b.direction=direction;assert(driver->doAsyncReadWrite(&b,0,1,nullptr,&c)==kIOReturnBadArgument);}
 assert(driver->doEjectMedia()==kIOReturnUnsupported&&driver->doFormatMedia(1)==kIOReturnUnsupported&&driver->doUnmap(nullptr,0,0)==kIOReturnUnsupported&&driver->setWriteCacheState(true)==kIOReturnUnsupported);
 assert(driver->reportBlockSize(nullptr)==kIOReturnBadArgument&&driver->reportMaxValidBlock(nullptr)==kIOReturnBadArgument&&driver->reportMediaState(nullptr,nullptr)==kIOReturnBadArgument);
 assert(!r.calls&&!native_ram_block_reads&&!native_ram_block_writes);
 cleanup();
}
static void writes(){
 setup();start();const uint64_t offset=(1ULL<<29)+512;
 IOMemoryDescriptor w(65536,kIODirectionOut);std::fill(w.bytes.begin(),w.bytes.end(),0x66);
 auto before=native_ram_block_resident_bytes;w.copyLimit=12345;auto r=request(w,offset/512,128);
 assert(r.status==kIOReturnIOError&&!r.bytes&&native_ram_block_resident_bytes==before);
 IOMemoryDescriptor b(65536);r=request(b,offset/512,128);assert(!r.status&&std::all_of(b.bytes.begin(),b.bytes.end(),[](auto x){return x==0;}));
 w.copyLimit=UINT64_MAX;failMallocCall=mallocCalls+9;r=request(w,offset/512,128);
 assert(r.status==kIOReturnNoMemory&&!r.bytes&&native_ram_block_resident_bytes==before);
 r=request(b,offset/512,128);assert(!r.status&&std::all_of(b.bytes.begin(),b.bytes.end(),[](auto x){return x==0;}));
 failMallocCall=0;r=request(w,offset/512,128);assert(!r.status&&r.bytes==65536&&native_ram_block_resident_bytes==before+17*4096);
 assert(driver->doSynchronizeCache()==0);r=request(b,offset/512,128);assert(!r.status&&b.bytes==w.bytes);
 std::fill(w.bytes.begin(),w.bytes.end(),0x77);w.copyLimit=65535;r=request(w,offset/512,128);
 assert(r.status==kIOReturnIOError&&!r.bytes);r=request(b,offset/512,128);assert(!r.status&&b.bytes[65535]==0x66&&b.bytes[0]==0x66);
 b.copyLimit=5000;r=request(b,offset/512,128);assert(r.status==kIOReturnIOError&&r.bytes==5000);
 driver->stop(IOService::resource);Result stopped;auto c=completion(stopped);b.copyLimit=UINT64_MAX;
 assert(driver->doAsyncReadWrite(&b,0,1,nullptr,&c)==kIOReturnNotReady&&!stopped.calls);
 assert(driver->doSynchronizeCache()==kIOReturnNotReady);bool present=true;assert(driver->reportMediaState(&present,nullptr)==0&&!present);
 cleanup();
}
static void cap(){
 setup();start();IOMemoryDescriptor w(65536,kIODirectionOut);std::fill(w.bytes.begin(),w.bytes.end(),0xc3);
 uint64_t at=1ULL<<28;
 while(native_ram_block_resident_bytes<kResidentLimit){
  uint64_t bytes=std::min<uint64_t>(65536,kResidentLimit-native_ram_block_resident_bytes);
  auto r=request(w,at/512,bytes/512);assert(!r.status&&r.bytes==bytes);at+=bytes;
 }
 auto live=mallocLive;auto r=request(w,at/512,8);assert(r.status==kIOReturnNoMemory&&!r.bytes&&mallocLive==live&&native_ram_block_resident_bytes==kResidentLimit);
 r=request(w,(1ULL<<28)/512,128);assert(!r.status&&r.bytes==65536&&mallocLive==live);
 IOMemoryDescriptor b(4096);r=request(b,at/512,8);assert(!r.status&&std::all_of(b.bytes.begin(),b.bytes.end(),[](auto x){return x==0;}));cleanup();
}
struct Reentry {Result first,second;IOMemoryDescriptor bytes{512};};
static void reenter(void *ctx,void *,IOReturn status,UInt64 bytes){assert(!heldLocks);auto &r=*static_cast<Reentry *>(ctx);++r.first.calls;r.first.status=status;r.first.bytes=bytes;auto c=completion(r.second);assert(driver->doAsyncReadWrite(&r.bytes,0,1,nullptr,&c)==0);}
static void concurrent(){
 setup();start();Reentry r;IOStorageCompletion c{&r,reenter,nullptr};IOMemoryDescriptor b(512);
 assert(driver->doAsyncReadWrite(&b,0,1,nullptr,&c)==0&&r.first.calls==1&&r.second.calls==1);
 auto work=[](unsigned worker){for(unsigned i=0;i<100;++i){IOMemoryDescriptor w(65536,kIODirectionOut),b(65536);
  std::fill(w.bytes.begin(),w.bytes.end(),static_cast<uint8_t>(worker*80+i));
  uint64_t block=((1ULL<<29)+worker*(1ULL<<20))/512;
  auto result=request(w,block,128);assert(!result.status);result=request(b,block,128);assert(!result.status&&b.bytes==w.bytes);
 }};
 std::thread a(work,0),bthread(work,1);a.join();bthread.join();cleanup();
}
static IOService *media(const char *name,IOService *parent=nullptr,const char *uuid=kNativeDataSeedUUID){
 auto *m=new IOService;property(m,"UUID",uuid);property(m,kIOBSDNameKey,name);
 m->setProperty(kIOBSDMajorKey,uint64_t{1},32);m->setProperty(kIOBSDMinorKey,uint64_t{42},32);
 assert(m->attach(parent?parent:driver));IOService::candidates.push_back(m);return m;
}
static void registry(){
 setup();start();char path[64];dev_t device=123;uint64_t base=9,bytes=9;uint8_t digest[32];
 assert(!native_ram_block_volume_seed(kNativeDataSeedUUID,&device,path,sizeof(path),&base,&bytes,digest)&&device==NODEV&&!path[0]&&!base&&!bytes);
 media("disk42s2");assert(native_ram_block_volume_seed(kNativeDataSeedUUID,&device,path,sizeof(path),&base,&bytes,digest)&&!strcmp(path,"/dev/disk42s2")&&device==makedev(1,42)&&!base&&bytes==kNativeDataSeedLogicalBytes&&!memcmp(digest,kNativeDataSeedLogicalSHA256,32));
 assert(!native_ram_block_volume_seed("04769D26-2CA0-4825-B443-E2E5F88300F2",&device,path,sizeof(path),&base,&bytes,digest)&&device==NODEV&&!path[0]&&!base&&!bytes);
 assert(!native_ram_block_volume_seed(kNativeDataSeedUUID,&device,path,8,&base,&bytes,digest));
 media("disk99s1");assert(!native_ram_block_volume_seed(kNativeDataSeedUUID,&device,path,sizeof(path),&base,&bytes,digest));cleanup();
 for(unsigned which=0;which<5;++which){setup();start();auto *m=media(which==0?"../../disk1":"disk9s1",which==1?IOService::resource:nullptr);
  if(which==2)m->inactive=true;if(which==3)m->setProperty(kIOBSDMajorKey,uint64_t{256},32);if(which==4)driver->stop(IOService::resource);
  assert(!native_ram_block_volume_seed(kNativeDataSeedUUID,&device,path,sizeof(path),&base,&bytes,digest)&&device==NODEV&&!path[0]);cleanup();
 }
}
int main(int argc,char **argv){
 assert(argc==2);std::ifstream stream(argv[1],std::ios::binary);original={std::istreambuf_iterator<char>(stream),std::istreambuf_iterator<char>()};assert(original.size()==kNativeDataSeedEncodedBytes);
 basic();badStartup();bounds();writes();cap();concurrent();registry();
 puts("PASS: actual RAM driver seed authentication/property isolation, bounds, 128 MiB cap, atomic writes/OOM/short copies, serialization/completion reentry, cleanup and typed UUID discovery");
}
