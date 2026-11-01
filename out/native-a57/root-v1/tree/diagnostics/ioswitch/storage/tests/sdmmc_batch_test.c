/* Independent SDHCI register model for the bounded PIO batch primitive. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "sdmmc_read.h"

enum fault {
    NONE, BAD_POWER, PREEXISTING, CMD_ERROR, CMD_TIMEOUT, CARD_ERROR,
    DATA_ERROR, DATA_TIMEOUT, NO_BUFFER, EARLY_DONE, DONE_WITH_READY,
    DONE_READY_NO_BUFFER, DONE_READY_ERROR,
    END_ERROR, END_TIMEOUT, AUTO_ERROR, AUTO_STATUS_ONLY, STOP_CARD_ERROR,
    BUSY_TIMEOUT, BUSY_THEN_READY, TOTAL_TIMEOUT, STOPPED_TIMER, LATE_ERROR
};
struct model {
    uint32_t reg[128], reads[256];
    uint64_t now, frequency;
    uint32_t requested, blocks, words, phase, armed, commands, writes, fault_block;
    uint32_t busy_reads, tick_reads;
    enum fault fault;
};
static uint8_t byte_at(uint32_t lba, uint32_t at) {
    return (uint8_t)(lba*13u + at*7u + at/512u);
}
static uint8_t read8(void *ctx,uint32_t off) {
    struct model *m=ctx;m->reads[off]++;
    if(off==0x28)return 2;
    if(off==0x29)return m->fault==BAD_POWER?0:11;
    if(off==0x2f)return 0;
    assert(0);return 0;
}
static uint16_t read16(void *ctx,uint32_t off) {
    struct model *m=ctx;m->reads[off]++;
    if(off==0xfe)return 0x0303;
    if(off==0x2c)return 7;
    if(off==0x3c)return m->fault==AUTO_STATUS_ONLY?4:0;
    assert(off==4||off==6||off==12);return (uint16_t)m->reg[off/2];
}
static uint32_t read32(void *ctx,uint32_t off) {
    struct model *m=ctx;m->reads[off]++;
    if(off==0x24) {
        uint32_t p=0x1fb0000;
        if(m->phase==2) {
            p|=0x203;
            if(!((m->fault==NO_BUFFER||m->fault==DONE_READY_NO_BUFFER)&&
                 m->blocks==m->fault_block))p|=0x800;
        }
        if(m->phase==4 && m->requested>1) {
            m->busy_reads++;
            if(m->fault==BUSY_TIMEOUT || (m->fault==BUSY_THEN_READY&&m->busy_reads<=4))
                p=(p|2u)&~0x100000u;
        }
        return p;
    }
    if(off==0x30) {
        if(m->phase==0)return m->fault==PREEXISTING?0x8000:0;
        if(m->phase==1) {
            if(m->fault==CMD_TIMEOUT||m->fault==STOPPED_TIMER)return 0;
            return m->fault==CMD_ERROR?0x20001:1;
        }
        if(m->phase==2) {
            if(m->blocks==m->fault_block) {
                if(m->fault==DATA_ERROR)return 0x208020;
                if(m->fault==DATA_TIMEOUT)return 0;
                if(m->fault==EARLY_DONE)return 2;
                if(m->fault==DONE_WITH_READY||m->fault==DONE_READY_NO_BUFFER)return 0x22;
                if(m->fault==DONE_READY_ERROR)return 0x208022;
            }
            return m->armed?0:0x20;
        }
        if(m->phase==3) {
            if(m->fault==END_ERROR)return 0x208002;
            if(m->fault==AUTO_ERROR)return 0x1008002;
            if(m->fault==END_TIMEOUT)return 0;
            return 2;
        }
        if(m->phase==4)return m->fault==LATE_ERROR?0x208000:0;
        assert(0);
    }
    if(off==0x10)return m->fault==CARD_ERROR?0x400000:0x900;
    if(off==0x1c)return m->fault==STOP_CARD_ERROR?0x400000:0x900;
    if(off==0x20) {
        assert(m->phase==2 && m->words<m->requested*128);
        /* In a batch, clearing readiness must precede reading. The next
         * block's event is latched immediately by the final FIFO read. */
        if(m->requested>1)assert(m->armed);
        uint32_t value=0, at=m->words*4;
        for(unsigned i=0;i<4;i++)value|=(uint32_t)byte_at(m->reg[8/2],at+i)<<(8*i);
        m->words++;
        if(!(m->words&127u)) {
            m->blocks++;
            if(m->fault==TOTAL_TIMEOUT)m->now+=m->frequency/16;
            if(m->requested>1) {
                m->armed=0;
                if(m->blocks==m->requested)m->phase=3;
            }
        }
        return value;
    }
    assert(off==8||off==0x34||off==0x38);return m->reg[off/2];
}
static void write16(void *ctx,uint32_t off,uint16_t value) {
    struct model *m=ctx;m->writes++;
    if(off==0xe) {
        assert(m->phase==0 && (value==0x113a||value==0x123a));
        m->commands++;m->requested=m->reg[6/2];
        assert(m->requested>=1&&m->requested<=128);
        assert(m->reg[4/2]==512);
        assert(value==(m->requested==1?0x113a:0x123a));
        assert(m->reg[12/2]==(m->requested==1?0x12:0x36));
        assert((m->reg[0x34/2]&0x03ff8023)==0x03ff8023);
        assert(m->reg[0x38/2]==0);
        m->phase=1;return;
    }
    assert(off==4||off==6||off==12);m->reg[off/2]=value;
}
static void write32(void *ctx,uint32_t off,uint32_t value) {
    struct model *m=ctx;m->writes++;
    if(off==0x30) {
        if(m->phase==0)assert(value==0x23);
        else if(m->phase==1){assert(value==1);m->phase=2;}
        else if(m->phase==2) {
            assert(value==0x20);
            if(m->requested>1) {
                assert(!m->armed&&m->blocks<m->requested);m->armed=1;
            } else {
                assert(m->words==128);m->phase=3;
            }
        } else {assert(m->phase==3&&value==2);m->phase=4;}
        return;
    }
    assert(off==8||off==0x34||off==0x38);m->reg[off/2]=value;
}
static uint64_t ticks(void *ctx) {
    struct model *m=ctx;m->tick_reads++;
    if(m->fault!=STOPPED_TIMER) {
        m->now+=(m->fault==CMD_TIMEOUT||m->fault==DATA_TIMEOUT||m->fault==END_TIMEOUT||m->fault==BUSY_TIMEOUT)?100000:1;
    }
    return m->now;
}
static void barrier(void *ctx){(void)ctx;}
static void setup(struct model *m,struct iosw_sd_host *h,enum fault fault) {
    *m=(struct model){.frequency=1000000,.fault=fault};
    m->reg[4/2]=0x7200;m->reg[6/2]=8;m->reg[12/2]=0x33;
    m->reg[8/2]=0x1234;m->reg[0x34/2]=0x8003;m->reg[0x38/2]=0x8002;
    *h=(struct iosw_sd_host){.io={read8,read16,read32,write16,write32,ticks,barrier},
        .context=m,.frequency=m->frequency,.capacity_sectors=UINT64_C(0x100000000),
        .permitted_first_sector=100,.permitted_sector_count=1000,.block_addressing_verified=1};
}
static unsigned cases;
static void run(uint32_t count,enum fault fault,uint32_t fault_block,int expected) {
    struct model m;struct iosw_sd_host h;setup(&m,&h,fault);m.fault_block=fault_block;
    uint32_t original[128];memcpy(original,m.reg,sizeof original);
    uint8_t private_data[65538],client[65536];memset(private_data,0xa5,sizeof private_data);memset(client,0x5a,sizeof client);
    struct iosw_sd_result r={0};
    int result=iosw_sd_read_sectors(&h,100,count,private_data+1,&r);
    assert(result==expected);
    assert(private_data[0]==0xa5 && private_data[count*512+1]==0xa5);
    /* Real caller contract: incomplete private staging is never published. */
    if(result==0)memcpy(client,private_data+1,count*512);
    if(!result) {
        assert(r.stage==5&&r.words_read==count*128&&m.commands==1&&!h.blocked);
        assert(!memcmp(original,m.reg,sizeof original));
        for(uint32_t i=0;i<count*512;i++)assert(client[i]==byte_at(100,i));
        if(count>1)assert(r.stop_response==0x900&&r.auto_cmd_error==0&&m.reads[0x3c]==1&&m.reads[0x1c]==1);
        else assert(!m.reads[0x3c]&&!m.reads[0x1c]);
        if(count==128 && fault==NONE) {
            assert(m.reads[0x20]==16384 && m.reads[0x30]<300 && m.tick_reads<600);
            printf("64KiB read: %u FIFO reads, %u status reads, %u timer samples\n",m.reads[0x20],m.reads[0x30],m.tick_reads);
        }
    } else {
        for(uint32_t i=0;i<sizeof client;i++)assert(client[i]==0x5a);
        if(fault==EARLY_DONE||fault==DONE_WITH_READY||fault==DONE_READY_NO_BUFFER||fault==DONE_READY_ERROR)
            assert(m.words==m.fault_block*128); /* Refused block never drained. */
        if(count==1)for(uint32_t i=0;i<514;i++)assert(private_data[i]==0xa5);
        if(fault==BAD_POWER||fault==PREEXISTING)assert(!m.commands&&!m.writes&&!h.blocked);
        else {
            assert(m.commands==1&&h.blocked&&m.reg[0x38/2]==0);
            unsigned writes=m.writes;
            assert(iosw_sd_read_sectors(&h,100,count,private_data+1,&r)==IOSW_SD_ALREADY_BLOCKED);
            assert(m.writes==writes);
        }
    }
    const unsigned once[]={0xfe,0x28,0x29,0x2c,0x2f,4};
    for(unsigned i=0;i<sizeof once/sizeof *once;i++)assert(m.reads[once[i]]==1);
    cases++;
}
int main(void) {
    run(1,NONE,0,0);run(2,NONE,0,0);run(128,NONE,0,0);run(128,BUSY_THEN_READY,0,0);
    run(1,END_ERROR,0,IOSW_SD_CONTROLLER_ERROR);
    run(128,BAD_POWER,0,IOSW_SD_NOT_READY);run(128,PREEXISTING,0,IOSW_SD_PREEXISTING_ERROR);
    run(128,CMD_ERROR,0,IOSW_SD_CONTROLLER_ERROR);run(128,CMD_TIMEOUT,0,IOSW_SD_TIMEOUT);
    run(128,CARD_ERROR,0,IOSW_SD_CARD_ERROR);run(128,NO_BUFFER,1,IOSW_SD_BUFFER_STATE);
    run(128,EARLY_DONE,1,IOSW_SD_BUFFER_STATE);run(128,DONE_WITH_READY,1,IOSW_SD_BUFFER_STATE);
    run(2,DONE_WITH_READY,1,0);run(128,DONE_WITH_READY,127,0);
    run(2,DONE_WITH_READY,0,IOSW_SD_BUFFER_STATE);
    run(128,EARLY_DONE,127,IOSW_SD_BUFFER_STATE);
    run(128,DONE_READY_NO_BUFFER,127,IOSW_SD_BUFFER_STATE);
    run(128,DONE_READY_ERROR,127,IOSW_SD_CONTROLLER_ERROR);
    run(128,END_ERROR,0,IOSW_SD_CONTROLLER_ERROR);run(128,END_TIMEOUT,0,IOSW_SD_TIMEOUT);
    run(128,AUTO_ERROR,0,IOSW_SD_CONTROLLER_ERROR);run(128,AUTO_STATUS_ONLY,0,IOSW_SD_CONTROLLER_ERROR);
    run(128,STOP_CARD_ERROR,0,IOSW_SD_CARD_ERROR);run(128,BUSY_TIMEOUT,0,IOSW_SD_TIMEOUT);
    run(128,TOTAL_TIMEOUT,0,IOSW_SD_TIMEOUT);run(128,STOPPED_TIMER,0,IOSW_SD_TIMEOUT);
    run(128,LATE_ERROR,0,IOSW_SD_CONTROLLER_ERROR);
    const unsigned failed_blocks[]={0,1,64,127};
    for(unsigned i=0;i<4;i++) {
        run(128,DATA_ERROR,failed_blocks[i],IOSW_SD_CONTROLLER_ERROR);
        run(128,DATA_TIMEOUT,failed_blocks[i],IOSW_SD_TIMEOUT);
    }
    /* Request/partition/address limits must reject before any MMIO write. */
    for(unsigned scenario=0;scenario<10;scenario++) {
        struct model m;struct iosw_sd_host h;setup(&m,&h,NONE);
        uint64_t lba=100;uint32_t count=2;uint8_t out[65536];struct iosw_sd_result r={0};
        if(scenario==0)count=0;
        if(scenario==1)count=129;
        if(scenario==2)lba=99;
        if(scenario==3)lba=1099;
        if(scenario==4)lba=UINT64_MAX;
        if(scenario==5)h.block_addressing_verified=0;
        if(scenario==6)h.capacity_sectors=UINT64_C(0x100000001);
        if(scenario==7)h.permitted_sector_count=UINT64_MAX;
        if(scenario==8)h.io.write16=0;
        if(scenario==9)h.frequency=0;
        assert(iosw_sd_read_sectors(&h,lba,count,out,&r)!=0 && !m.writes&&!m.commands);cases++;
    }
    /* High LBA fits CMD18's 32-bit block argument and may end exactly at capacity. */
    struct model m;struct iosw_sd_host h;setup(&m,&h,NONE);
    h.permitted_first_sector=0;h.permitted_sector_count=h.capacity_sectors;
    uint8_t out[65536];struct iosw_sd_result r={0};
    assert(iosw_sd_read_sectors(&h,UINT64_C(0xffffff80),128,out,&r)==0);
    for(unsigned i=0;i<sizeof out;i++)assert(out[i]==byte_at(0xffffff80,i));cases++;
    setup(&m,&h,NONE);h.capacity_sectors=h.permitted_sector_count=1;h.permitted_first_sector=0;h.block_addressing_verified=0;
    assert(iosw_sd_read_sectors(&h,0,2,out,&r)==IOSW_SD_ARGUMENT && !m.writes);cases++;
    printf("%u bounded SDHCI cases passed: CMD17/CMD18, exact FIFO blocks, Auto CMD12, busy, deadlines, range refusals, fail-stop, private staging\n",cases);
    return 0;
}
