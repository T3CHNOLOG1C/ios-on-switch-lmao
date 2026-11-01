"""Exercise the read-only PIO state machine against an independent register model."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]

class NativeSdmmcTests(unittest.TestCase):
    def test_register_protocol_bounds_faults_and_no_media_write_commands(self):
        compiler=shutil.which('clang')
        if not compiler:
            raise unittest.SkipTest('C compiler unavailable')
        source=r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "sdmmc_read.h"
struct model {
    uint32_t reg[128]; unsigned mode, phase, words, writes, commands, forbidden;
    uint32_t reads[256];
    uint16_t version;
    uint64_t now; uint32_t status;
};
static uint8_t r8(void *p,uint32_t o) {
    struct model *m=p;
    m->reads[o]++;
    if(o==0x28)return m->mode==2||m->mode==23?0x22:2;
    if(o==0x29)return m->mode==3||m->mode==23||m->mode==27?0:11;
    if(o==0x2f)return m->mode==22||m->mode==23?1:0;
    assert(0);return 0;
}
static uint16_t r16(void *p,uint32_t o) {
    struct model *m=p;
    m->reads[o]++;
    if(o==0xfe)return m->mode==1||m->mode==23?1:m->mode==24?0xab02:
        m->mode==25||m->mode==27?0x0303:m->mode==26?m->version:2;
    if(o==0x2c)return m->mode==4||m->mode==23?3:7;
    if(o==4&&(m->mode==19||m->mode==23))return 513;
    assert(o==4||o==6||o==12);return (uint16_t)m->reg[o/2];
}
static uint32_t r32(void *p,uint32_t o) {
    struct model *m=p;
    m->reads[o]++;
    if(o==0x24) {
        uint32_t v=0x1fb0000; /* actual OLED pre-command photo */
        if(m->mode==5)v|=1;
        if(m->mode==6)v&=~0x10000;
        if(m->mode==17)v|=4;
        if(m->mode==18)v|=0x100;
        if(m->phase>=2&&m->mode!=13)v|=0x800;
        if(m->phase==3&&m->mode==15)v|=2;
        if(m->phase==3&&m->mode==20)v|=0x200;
        if(m->mode==23)v=1;
        return v;
    }
    if(o==0x30) {
        if(m->mode==7&&!m->phase)return 0x8000;
        if(m->phase==1) {
            if(m->mode==8||m->mode==16)return 0;
            if(m->mode==9)return 0x20001; /* error plus command complete */
            return 1;
        }
        if(m->phase==2) {
            if(m->mode==11)return 0;
            if(m->mode==12)return 0x208020; /* error plus buffer ready */
            if(m->mode==21)return 0x808020; /* bus power error plus buffer ready */
            return 0x20;
        }
        if(m->phase==3)return m->mode==14?0x208002:2;
        return 0;
    }
    if(o==0x10)return m->mode==10?0x400000:0x900;
    if(o==0x20) {
        assert(m->phase==2&&m->words<128);
        uint32_t w=0;
        for(unsigned j=0;j<4;j++)w|=(uint32_t)(uint8_t)(m->words*4+j)<<(j*8);
        m->words++;return w;
    }
    assert(o==0x34||o==0x38||o==8);return m->reg[o/2];
}
static void w16(void *p,uint32_t o,uint16_t v) {
    struct model *m=p;m->writes++;
    if(o==0xe) {
        m->commands++;assert(v==0x113a);assert(m->phase==0);
        assert(m->reg[4/2]==512&&m->reg[6/2]==1&&m->reg[12/2]==0x12);
        assert((m->reg[0x34/2]&0x03ff8023)==0x03ff8023);
        assert(m->reg[0x38/2]==0);m->phase=1;return;
    }
    assert(o==4||o==6||o==12);m->reg[o/2]=v;
}
static void w32(void *p,uint32_t o,uint32_t v) {
    struct model *m=p;m->writes++;
    if(o==0x30) {
        if(m->phase==0)assert(v==0x23);
        else if(m->phase==1) {assert(v==1);m->phase=2;}
        else if(m->phase==2) {assert(v==0x20&&m->words==128);m->phase=3;}
        else {assert(m->phase==3&&v==2);}
        return;
    }
    assert(o==8||o==0x34||o==0x38);m->reg[o/2]=v;
}
static uint64_t ticks(void *p) {
    struct model *m=p;
    if(m->mode!=16)m->now+=100;
    return m->now;
}
static void barrier(void *p) {(void)p;}
int main(void) {
    const unsigned errors[]={0,3,3,3,3,3,3,4,5,6,7,5,6,8,6,8,5,3,3,3,8,6,3,3,0,0};
    const unsigned masks[]={0,1,8,16,32,2,4,0,0,0,0,0,0,0,0,0,0,2,2,128,0,0,64,255,0,0};
    for(unsigned mode=0;mode<=25;mode++) {
        struct model m={.mode=mode};
        m.reg[4/2]=0x7200;m.reg[6/2]=8;m.reg[12/2]=0x33;
        m.reg[8/2]=0x1234;m.reg[0x34/2]=0x8003;m.reg[0x38/2]=0x8002;
        uint32_t prior[128];memcpy(prior,m.reg,sizeof prior);
        struct iosw_sd_host h={.io={r8,r16,r32,w16,w32,ticks,barrier},.context=&m,
            .frequency=1000,.capacity_sectors=2000,.permitted_first_sector=100,
            .permitted_sector_count=100,.block_addressing_verified=1};
        uint8_t dest[514];memset(dest,0xaa,sizeof dest);struct iosw_sd_result report;
        int result=iosw_sd_read_sector(&h,199,dest+1,&report);
        assert(result==(int)errors[mode]);assert(dest[0]==0xaa&&dest[513]==0xaa);
        assert(report.readiness_failures==masks[mode]);
        /* Every displayed readiness sample is the actual one used, and each
         * register is read only once even when an early predicate fails. */
        const unsigned once[]={0xfe,0x28,0x29,0x2c,0x2f,4};
        for(unsigned i=0;i<sizeof once/sizeof *once;i++)assert(m.reads[once[i]]==1);
        assert(report.version==(mode==1||mode==23?1:mode==24?0xab02:mode==25?0x0303:2));
        assert(report.host_control==(mode==2||mode==23?0x22:2));
        assert(report.power==(mode==3||mode==23?0:11));
        assert(report.clock==(mode==4||mode==23?3:7));
        assert(report.reset==(mode==22||mode==23?1:0));
        assert(report.block_size==(mode==19||mode==23?513:0x7200));
        uint32_t expected_present=0x1fb0000;
        if(mode==5)expected_present|=1;
        if(mode==6)expected_present&=~0x10000;
        if(mode==17)expected_present|=4;
        if(mode==18)expected_present|=0x100;
        if(mode==23)expected_present=1;
        assert(report.present==expected_present);
        assert(report.initial_status==(mode==7?0x8000:0));
        if(masks[mode])assert(report.stage==1&&m.reads[0x24]==1&&m.reads[0x30]==0);
        if(!mode||mode==24||mode==25) {
            assert(report.stage==5&&report.words_read==128&&m.commands==1&&!h.blocked);
            for(unsigned i=0;i<512;i++)assert(dest[i+1]==(uint8_t)i);
            assert(!memcmp(prior,m.reg,sizeof prior));
        } else {
            for(unsigned i=0;i<514;i++)assert(dest[i]==0xaa);
            if(mode<=7||(mode>=17&&mode<=19)||mode==22||mode==23)assert(m.writes==0&&m.commands==0&&!h.blocked);
            else {
                assert(m.commands==1&&h.blocked&&m.reg[0x38/2]==0);
                unsigned writes=m.writes;
                assert(iosw_sd_read_sector(&h,100,dest+1,&report)==IOSW_SD_ALREADY_BLOCKED);
                assert(writes==m.writes);
            }
        }
    }
    /* Every unreviewed low-byte revision stays refused, independent of the
     * vendor byte. Also prove the new 4.0 revision cannot bypass power. */
    for(unsigned scenario=0;scenario<513;scenario++) {
        struct model m={.mode=scenario==512?27:26,
            .version=(uint16_t)((scenario&255)|(scenario&256?0xab00:0))};
        m.reg[4/2]=0x7200;
        uint32_t prior[128];memcpy(prior,m.reg,sizeof prior);
        struct iosw_sd_host h={.io={r8,r16,r32,w16,w32,ticks,barrier},.context=&m,
            .frequency=1000,.capacity_sectors=1,.permitted_sector_count=1};
        uint8_t out[512];memset(out,0xaa,sizeof out);struct iosw_sd_result report;
        int allowed=scenario<512&&((scenario&255)==2||(scenario&255)==3);
        assert(iosw_sd_read_sector(&h,0,out,&report)==(allowed?0:IOSW_SD_NOT_READY));
        assert(report.version==(scenario==512?0x0303:m.version));
        assert(report.readiness_failures==(allowed?0:scenario==512?IOSW_SD_READY_POWER:IOSW_SD_READY_VERSION));
        if(allowed) {
            assert(report.stage==5&&report.words_read==128&&m.commands==1&&!h.blocked);
            assert(!memcmp(prior,m.reg,sizeof prior));
            for(unsigned i=0;i<512;i++)assert(out[i]==(uint8_t)i);
        } else {
            assert(report.stage==1&&m.writes==0&&m.commands==0&&!h.blocked);
            for(unsigned i=0;i<512;i++)assert(out[i]==0xaa);
        }
    }
    for(unsigned scenario=0;scenario<9;scenario++) {
        struct model m={0};
        struct iosw_sd_host h={.io={r8,r16,r32,w16,w32,ticks,barrier},.context=&m,
            .frequency=1000,.capacity_sectors=2000,.permitted_first_sector=100,
            .permitted_sector_count=100,.block_addressing_verified=1};
        uint8_t out[512];struct iosw_sd_result report;uint64_t lba=100;
        if(scenario==0)lba=99;
        if(scenario==1)lba=200;
        if(scenario==2)h.block_addressing_verified=2;
        if(scenario==3)h.block_addressing_verified=0;
        if(scenario==4)h.capacity_sectors=UINT64_C(0x100000001);
        if(scenario==5)h.permitted_sector_count=2000;
        if(scenario==6)h.frequency=0;
        if(scenario==7)h.permitted_sector_count=0;
        if(scenario==8)h.io.barrier=0;
        assert(iosw_sd_read_sector(&h,lba,out,&report)!=0&&m.writes==0);
    }
    struct model m={0};
    m.reg[4/2]=512;
    struct iosw_sd_host h={.io={r8,r16,r32,w16,w32,ticks,barrier},.context=&m,
        .frequency=1000,.capacity_sectors=1,.permitted_sector_count=1};
    uint8_t out[512];struct iosw_sd_result report;
    assert(iosw_sd_read_sector(&h,1,out,&report)==IOSW_SD_ARGUMENT&&m.writes==0);
    assert(iosw_sd_read_sector(&h,0,out,&report)==0&&m.commands==1);
    puts("550 cases passed; exact readiness samples and masks; only CMD17 issued; no power/clock/reset/DMA/media-write commands");
}
'''
        with tempfile.TemporaryDirectory(prefix='iosw-sdmmc-') as directory:
            p=Path(directory);(p/'test.c').write_text(source)
            subprocess.run([compiler,'-std=c11','-O2','-Wall','-Wextra','-Werror',
                '-I',str(ROOT/'native/storage'),str(p/'test.c'),str(ROOT/'native/storage/sdmmc_read.c'),
                '-o',str(p/'test')],check=True,capture_output=True,text=True)
            result=subprocess.run([str(p/'test')],capture_output=True,text=True,timeout=10)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            self.assertIn('550 cases passed',result.stdout)

if __name__=='__main__':unittest.main()
