// Host test of boot/storage/NativeSwitchBlock.cpp, the boot chain's read-only block driver for the SD card's two
// APFS containers: the real driver, included into this translation unit over native_block_stubs.h (BUILD.bazel points
// its IOKit headers there) and a backend that serves a pattern. Each scenario runs in a process of its own (the driver
// keeps its published state in statics): bounds and refused mutations, backend errors and short copies, a completion
// that reenters the driver, two threads' reads serialized, rollback of every failed step of publication, and the volume
// path lookup by UUID (ancestry, ambiguity, inactive media).
#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include "bazel/testing/fresh_process.h"
#include "bazel/testing/require.h"
#include "../../NativeSwitchBlock.cpp"

struct iosw_block_backend { iosw_block_allocator allocator; };
static unsigned creates=0,destroys=0,reads=0;
static int createStatus=0,readStatus=0,infoStatus=0;
static bool invalidSize=false;
static std::atomic<unsigned> activeReads{0},maximumReads{0};
extern "C" int iosw_block_create(const iosw_block_allocator *allocator,iosw_block_backend **out) {
 STUB_REQUIRE(heldLocks==1); ++creates; *out=nullptr;
 if(createStatus)return createStatus;
 *out=static_cast<iosw_block_backend *>(allocator->allocate(allocator->context,sizeof(**out)));
 if(!*out)return IOSW_BLOCK_NOMEM;
 (*out)->allocator=*allocator;return 0;
}
extern "C" void iosw_block_destroy(iosw_block_backend *backend) {
 STUB_REQUIRE(heldLocks==1); ++destroys;
 auto a=backend->allocator; a.release(a.context,backend,sizeof(*backend));
}
extern "C" int iosw_block_get_info(iosw_block_backend *,unsigned index,iosw_block_info *out) {
 STUB_REQUIRE(heldLocks==1 && index<2); if(infoStatus)return infoStatus;
 *out={};out->size_bytes=invalidSize?1:(index?3303014400ULL:6077580800ULL);
 strlcpy(out->label,index?"SystemOS":"OS",sizeof(out->label));return 0;
}
static uint8_t pattern(unsigned disk,uint64_t byte) { return static_cast<uint8_t>(disk*73+byte/512+byte%512); }
extern "C" int iosw_block_read(iosw_block_backend *,unsigned index,uint64_t offset,void *dst,uint32_t bytes) {
 STUB_REQUIRE(heldLocks==1 && index<2 && bytes<=65536 && !(offset%512) && !(bytes%512));
 ++reads; auto active=++activeReads; maximumReads.store(std::max(maximumReads.load(),active));
 std::this_thread::sleep_for(std::chrono::microseconds(50));
 for(unsigned i=0;i<bytes;++i)static_cast<uint8_t *>(dst)[i]=pattern(index,offset+i);
 --activeReads;return readStatus;
}
static NativeSwitchBlockDevice *child(unsigned index) {
 REQUIRE(IOService::registered.size()==2); return dynamic_cast<NativeSwitchBlockDevice *>(IOService::registered[index]);
}
static void cleanup() {
 for(auto *media:IOService::candidates) { if(media->getProvider())media->detach(media->getProvider());media->release(); }
 IOService::candidates.clear();
 for(auto *service:IOService::registered) {
  auto *provider=service->getProvider();service->stop(provider);service->detach(provider);service->release();
 }
 IOService::registered.clear();
 if(publishedOwner) { auto *provider=publishedOwner->getProvider();publishedOwner->stop(provider);publishedOwner->detach(provider);publishedOwner->release();publishedOwner=nullptr; }
 REQUIRE(!mallocLive && OSObject::live==1 && !heldLocks && !activeReads);
 publicationState=0;publicationStatus=IOSW_BLOCK_NOT_READY;
 allocationCalls=failAllocationCall=attachCalls=failAttachCall=startCalls=failStartCall=0;
 propertyCalls=failPropertyCall=superclassStops=mallocCalls=failMallocCall=0;
 creates=destroys=reads=0;createStatus=readStatus=infoStatus=0;invalidSize=false;maximumReads=0;
 switch_block_devices_published=0;switch_block_read_requests=0;switch_block_read_bytes=0;switch_block_read_failures=0;
}
struct Result {unsigned calls=0;IOReturn status=123;UInt64 actual=999;};
static void complete(void *ctx,void *,IOReturn status,UInt64 bytes) {
 STUB_REQUIRE(!heldLocks);auto &r=*static_cast<Result *>(ctx);++r.calls;r.status=status;r.actual=bytes;
}
static IOStorageCompletion completion(Result &r) { return {&r,complete,nullptr}; }
static bool matches(IOMemoryDescriptor &data,unsigned disk,uint64_t offset) {
 for(unsigned i=0;i<data.bytes.size();++i)if(data.bytes[i]!=pattern(disk,offset+i))return false;
 return true;
}
static void verify(IOMemoryDescriptor &data,unsigned disk,uint64_t offset) { REQUIRE(matches(data,disk,offset)); }
static void publish() { REQUIRE(switch_block_publish()==0 && creates==1 && IOService::registered.size()==2); }
static void basic() {
 publish();REQUIRE(switch_block_publish()==0 && creates==1);
 for(unsigned i=0;i<2;++i) {
  UInt64 n=0;bool b=false;auto *d=child(i);
  REQUIRE(d->reportBlockSize(&n)==0 && n==512);
  REQUIRE(d->reportMaxValidBlock(&n)==0 && n==(i?6451199:11870274));
  REQUIRE(d->reportWriteProtection(&b)==0 && b);
  REQUIRE(d->reportMediaState(&b,nullptr)==0 && b);
  REQUIRE(dynamic_cast<OSNumber *>(d->getProperty(kIOMaximumBlockCountReadKey))->unsigned64BitValue()==128);
  REQUIRE(dynamic_cast<OSNumber *>(d->getProperty(kIOMaximumByteCountReadKey))->unsigned64BitValue()==65536);
  for(auto lba:std::vector<uint64_t>{0,8388609,n}) {
   if(lba>n)continue;IOMemoryDescriptor data(512);Result r;auto c=completion(r);
   REQUIRE(d->doAsyncReadWrite(&data,lba,1,nullptr,&c)==0 && r.calls==1 && !r.status && r.actual==512);
   verify(data,i,lba*512);
  }
 }
 cleanup();
}
static void rejected() {
 publish();auto *d=child(0);IOMemoryDescriptor data(65536);Result r;auto c=completion(r);
 REQUIRE(d->doAsyncReadWrite(nullptr,0,1,nullptr,&c)==kIOReturnBadArgument);
 REQUIRE(d->doAsyncReadWrite(&data,0,1,nullptr,nullptr)==kIOReturnBadArgument);
 auto noAction=c;noAction.action=nullptr;REQUIRE(d->doAsyncReadWrite(&data,0,1,nullptr,&noAction)==kIOReturnBadArgument);
 for(auto pair:std::vector<std::pair<uint64_t,uint64_t>>{{0,0},{0,129},{UINT64_MAX,1},{1,UINT64_MAX},{11870274,2},{11870275,1}})
  REQUIRE(d->doAsyncReadWrite(&data,pair.first,pair.second,nullptr,&c)==kIOReturnBadArgument);
 IOMemoryDescriptor shortData(511);REQUIRE(d->doAsyncReadWrite(&shortData,0,1,nullptr,&c)==kIOReturnBadArgument);
 for(unsigned direction:{kIODirectionOut,kIODirectionInOut,0u}) {data.direction=direction;REQUIRE(d->doAsyncReadWrite(&data,0,1,nullptr,&c)==kIOReturnNotWritable);}
 REQUIRE(d->doFormatMedia(512)==kIOReturnNotWritable && d->doUnmap(nullptr,0,0)==kIOReturnNotWritable);
 REQUIRE(d->doEjectMedia()==kIOReturnUnsupported && d->setWriteCacheState(true)==kIOReturnUnsupported);
 REQUIRE(!reads && !r.calls);cleanup();
}
static void errors() {
 publish();IOMemoryDescriptor data(1024);Result r;auto c=completion(r);readStatus=IOSW_BLOCK_TIMEOUT;
 REQUIRE(child(0)->doAsyncReadWrite(&data,0,2,nullptr,&c)==0 && r.calls==1 && r.status==kIOReturnTimeout && !r.actual);
 REQUIRE(std::all_of(data.bytes.begin(),data.bytes.end(),[](auto b){return b==0xa5;}));
 REQUIRE(child(1)->doAsyncReadWrite(&data,0,2,nullptr,&c)==kIOReturnNotReady && r.calls==1 && reads==1);
 bool present=true;REQUIRE(child(1)->reportMediaState(&present,nullptr)==0 && !present);cleanup();
 publish();data.copyLimit=400;r={};c=completion(r);
 REQUIRE(child(0)->doAsyncReadWrite(&data,0,2,nullptr,&c)==0 && r.calls==1 && r.status==kIOReturnIOError && r.actual==400);
 data.copyLimit=UINT64_MAX;r={};c=completion(r);
 REQUIRE(child(1)->doAsyncReadWrite(&data,0,2,nullptr,&c)==0 && r.calls==1 && !r.status);
 child(0)->stop(publishedOwner);r={};c=completion(r);
 REQUIRE(child(1)->doAsyncReadWrite(&data,0,2,nullptr,&c)==kIOReturnNotReady && !r.calls);cleanup();
}
struct Reentry {Result outer,inner;IOMemoryDescriptor data{512};};
static void reenter(void *ctx,void *,IOReturn status,UInt64 bytes) {
 STUB_REQUIRE(!heldLocks);auto &r=*static_cast<Reentry *>(ctx);++r.outer.calls;r.outer.status=status;r.outer.actual=bytes;
 auto c=completion(r.inner);STUB_REQUIRE(child(1)->doAsyncReadWrite(&r.data,3,1,nullptr,&c)==0);
}
static void concurrent() {
 publish();Reentry r;IOStorageCompletion c{&r,reenter,nullptr};IOMemoryDescriptor data(512);
 REQUIRE(child(0)->doAsyncReadWrite(&data,0,1,nullptr,&c)==0 && r.outer.calls==1 && r.inner.calls==1);
 verify(r.data,1,1536);
 auto worker=[](unsigned disk){for(unsigned i=0;i<30;++i){IOMemoryDescriptor bytes(65536);Result r;auto c=completion(r);
  EXPECT_TRUE(child(disk)->doAsyncReadWrite(&bytes,i*128,128,nullptr,&c)==0 && r.calls==1 && !r.status && r.actual==65536);
  EXPECT_TRUE(matches(bytes,disk,i*65536));}};
 std::thread a(worker,0),b(worker,1);a.join();b.join();REQUIRE(maximumReads==1 && reads==62);cleanup();
}
static void failures() {
 for(unsigned call=1;call<=3;++call) {failAllocationCall=call;REQUIRE(switch_block_publish()!=0 && IOService::registered.empty());REQUIRE(switch_block_publish()!=0);REQUIRE(creates==destroys);cleanup();}
 for(unsigned call=1;call<=3;++call) {failAttachCall=call;REQUIRE(switch_block_publish()!=0 && IOService::registered.empty());REQUIRE(creates==destroys);cleanup();}
 for(unsigned call=1;call<=3;++call) {failStartCall=call;REQUIRE(switch_block_publish()!=0 && IOService::registered.empty());REQUIRE(creates==destroys);cleanup();}
 for(unsigned call=1;call<=4;++call) {failPropertyCall=call;REQUIRE(switch_block_publish()!=0 && IOService::registered.empty());REQUIRE(creates==destroys);cleanup();}
 for(unsigned call=1;call<=2;++call) {failMallocCall=call;REQUIRE(switch_block_publish()!=0 && IOService::registered.empty());REQUIRE(creates==(call==2?1:0));cleanup();}
 invalidSize=true;REQUIRE(switch_block_publish()==IOSW_BLOCK_CORRUPT && IOService::registered.empty() && creates==destroys);cleanup();
 createStatus=IOSW_BLOCK_IO;REQUIRE(switch_block_publish()==IOSW_BLOCK_IO && IOService::registered.empty());cleanup();
}
static void textProperty(IOService *service,const char *key,const char *text) {auto *s=OSString::withCString(text);service->setProperty(key,s);s->release();}
static IOService *media(unsigned disk,const char *name,IOService *parent=nullptr) {
 auto *m=new IOService;textProperty(m,"UUID",volumeUUIDs[disk]);textProperty(m,kIOBSDNameKey,name);
 m->setProperty(kIOBSDMajorKey,uint64_t{1},32);m->setProperty(kIOBSDMinorKey,uint64_t{4},32);
 REQUIRE(m->attach(parent?parent:child(disk)));IOService::candidates.push_back(m);return m;
}
static void registry() {
 publish();char path[64];REQUIRE(switch_block_volume_path(0,path,sizeof(path))==IOSW_BLOCK_NOT_READY && !path[0]);
 media(0,"disk7s1");media(1,"disk12s2");REQUIRE(switch_block_volume_path(0,path,sizeof(path))==0 && !std::strcmp(path,"/dev/disk7s1"));
 REQUIRE(switch_block_volume_path(1,path,sizeof(path))==0 && !std::strcmp(path,"/dev/disk12s2"));
 REQUIRE(switch_block_volume_path(2,path,sizeof(path))==IOSW_BLOCK_INVALID && !path[0]);
 REQUIRE(switch_block_volume_path(0,path,11)==IOSW_BLOCK_NOT_READY && !path[0]);
 media(0,"disk99s1");REQUIRE(switch_block_volume_path(0,path,sizeof(path))==IOSW_BLOCK_CORRUPT && !path[0]);cleanup();
 publish();auto *m=media(0,"disk1s1",child(1));REQUIRE(switch_block_volume_path(0,path,sizeof(path))==IOSW_BLOCK_NOT_READY);cleanup();
 publish();m=media(0,"../../disk1");REQUIRE(switch_block_volume_path(0,path,sizeof(path))==IOSW_BLOCK_NOT_READY);cleanup();
 publish();m=media(0,"disk1s1");m->inactive=true;REQUIRE(switch_block_volume_path(0,path,sizeof(path))==IOSW_BLOCK_NOT_READY);cleanup();
 publish();m=media(0,"disk1s1");infoStatus=IOSW_BLOCK_NOT_READY;REQUIRE(switch_block_volume_path(0,path,sizeof(path))==IOSW_BLOCK_NOT_READY && !path[0]);cleanup();
}
// One process per scenario: the driver's publication and the stand-ins' registry are boot-wide. The resource service
// the driver publishes under comes and goes with the scenario, and nothing it allocated outlives it.
static void scenario(void (*body)()) {
 switch_ios::test::ExpectPassesInFreshProcess([body] {
  IOService::resource=new IOService;
  body();
  IOService::resource->release();IOService::resource=nullptr;
  EXPECT_EQ(OSObject::live.load(),0u) << "objects leaked";
 });
}

TEST(NativeSwitchBlock, BasicReads) { scenario(basic); }
TEST(NativeSwitchBlock, RejectedRequestsAndMutations) { scenario(rejected); }
TEST(NativeSwitchBlock, BackendErrorsAndShortCopies) { scenario(errors); }
TEST(NativeSwitchBlock, ReentryAndTwoThreadSerialization) { scenario(concurrent); }
TEST(NativeSwitchBlock, PublicationRollback) { scenario(failures); }
TEST(NativeSwitchBlock, VolumePathByUuid) { scenario(registry); }
