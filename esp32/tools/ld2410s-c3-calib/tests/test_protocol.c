#include "protocol.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned frames[3]; static ld_reading latest; static uint16_t progress;
static void got(ld_frame_kind kind,const uint8_t *data,size_t size,void *ctx) {
    (void)ctx; frames[kind]++;
    ld_reading r; if(ld_decode_reading(kind,data,size,&r)) latest=r;
    uint16_t p; if(ld_decode_progress(kind,data,size,&p)) progress=p;
}
int main(void) {
    uint8_t out[140],payload[96]={0};
    assert(ld_command(0xff,(uint8_t[]){1,0},2,out,sizeof(out))==14);
    assert(!memcmp(out,(uint8_t[]){0xfd,0xfc,0xfb,0xfa,4,0,0xff,0,1,0,4,3,2,1},14));
    assert(ld_command(0x72,payload,96,out,sizeof(out))==108);
    assert(ld_command(0x72,payload,96,out,100)==0);
    uint8_t standard[80]={0xf4,0xf3,0xf2,0xf1,70,0,1,3,150,0,0,0};
    for(unsigned i=0;i<16;i++) ld_put32(standard+12+i*4,i+10);
    memcpy(standard+76,(uint8_t[]){0xf8,0xf7,0xf6,0xf5},4);
    const uint8_t minimal[]={0x6e,0,80,0,0x62};
    for(unsigned split=1;split<90;split++) {
        ld_parser p={0}; memset(frames,0,sizeof(frames));
        ld_feed(&p,(uint8_t *)"noise",5,got,NULL);
        ld_feed(&p,minimal,5,got,NULL); assert(latest.state==0 && latest.distance_cm==80);
        for(size_t i=0;i<80;i+=split) ld_feed(&p,standard+i,80-i<split?80-i:split,got,NULL);
        assert(frames[LD_DATA]==1 && frames[LD_MINIMAL]==1);
        assert(latest.state==3 && latest.distance_cm==150 && latest.energy_valid && latest.energy[15]==25);
        assert(p.bad_bytes==5);
        uint8_t calibration[]={0xf4,0xf3,0xf2,0xf1,3,0,3,0x64,0,0xf8,0xf7,0xf6,0xf5};
        for(size_t i=0;i<sizeof(calibration);i+=split)
            ld_feed(&p,calibration+i,sizeof(calibration)-i<split?sizeof(calibration)-i:split,got,NULL);
        assert(progress==10000);
        out[0]=0xfd; out[1]=0xfc; out[2]=0xfb; out[3]=0xfa;out[4]=255;out[5]=255;
        ld_feed(&p,out,6,got,NULL); ld_feed(&p,minimal,5,got,NULL); assert(frames[LD_MINIMAL]==2);
        /* Header bytes inside a valid energy payload must not be mistaken for frames. */
        memcpy(standard+20,(uint8_t[]){0xfd,0xfc,0xfb,0xfa},4);
        ld_feed(&p,standard,80,got,NULL); assert(frames[LD_DATA]==3);
    }
    /* Device percentages 0..100 become the dashboard's hundredths, not 0..1%. */
    for(unsigned percent=0;percent<=100;percent++) {
        uint8_t body[]={3,(uint8_t)percent,0}; uint16_t decoded=65535;
        assert(ld_decode_progress(LD_DATA,body,3,&decoded));
        assert(decoded==percent*100);
    }
    uint16_t decoded=1234;
    assert(!ld_decode_progress(LD_DATA,(uint8_t[]){3,101,0},3,&decoded));
    assert(!ld_decode_progress(LD_DATA,(uint8_t[]){3,0x10,0x27},3,&decoded));
    assert(!ld_decode_progress(LD_DATA,(uint8_t[]){3,100},2,&decoded));
    assert(!ld_decode_progress(LD_ACK,(uint8_t[]){3,100,0},3,&decoded));
    ld_settings s={.far_gate=2,.near_gate=0,.absence_s=10,.status_hz_x10=5,.distance_hz_x10=10,.response=5};
    for(unsigned i=0;i<16;i++) {s.trigger[i]=40;s.hold[i]=30;}
    assert(!ld_validate(&s));s.near_gate=3;assert(ld_validate(&s));s.near_gate=0;
    s.distance_hz_x10=6;assert(ld_validate(&s));s.distance_hz_x10=10;s.trigger[0]=100;assert(ld_validate(&s));
    puts("Protocol checks passed: framing, every fragmentation size, telemetry, progress, corrupt lengths, header bytes in payload, settings limits");
}
