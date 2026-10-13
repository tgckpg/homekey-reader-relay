#include "protocol.h"
#include <string.h>
static const uint8_t ch[] = {0xfd,0xfc,0xfb,0xfa}, ct[] = {4,3,2,1};
static const uint8_t dh[] = {0xf4,0xf3,0xf2,0xf1}, dt[] = {0xf8,0xf7,0xf6,0xf5};
uint16_t ld_u16(const uint8_t *p) { return (uint16_t)p[0] | (uint16_t)p[1]<<8; }
uint32_t ld_u32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24; }
void ld_put16(uint8_t *p, uint16_t v) { p[0]=v; p[1]=v>>8; }
void ld_put32(uint8_t *p, uint32_t v) { for(unsigned i=0;i<4;i++) p[i]=v>>(8*i); }
size_t ld_command(uint16_t command, const uint8_t *payload, size_t size, uint8_t *out, size_t capacity) {
    if(size > LD_PAYLOAD_MAX-2 || capacity < size+12 || (size && !payload)) return 0;
    memcpy(out,ch,4); ld_put16(out+4,size+2); ld_put16(out+6,command);
    if(size) memcpy(out+8,payload,size);
    memcpy(out+8+size,ct,4); return size+12;
}
static void consume(ld_parser *p,size_t n) {
    memmove(p->bytes,p->bytes+n,p->used-n); p->used-=n;
}
static void parse(ld_parser *p,ld_frame_cb cb,void *ctx) {
    while(p->used) {
        uint8_t *b=p->bytes;
        if(b[0]==0x6e) {
            if(p->used<5) return;
            if(b[1]<=3 && b[4]==0x62) { cb(LD_MINIMAL,b,5,ctx); consume(p,5); continue; }
        } else if(b[0]==ch[0] || b[0]==dh[0]) {
            const uint8_t *head=b[0]==ch[0]?ch:dh,*tail=b[0]==ch[0]?ct:dt;
            ld_frame_kind kind=b[0]==ch[0]?LD_ACK:LD_DATA;
            if(p->used<4 && !memcmp(b,head,p->used)) return;
            if(p->used>=4 && !memcmp(b,head,4)) {
                if(p->used<6) return;
                size_t n=ld_u16(b+4);
                if(n>=1 && n<=LD_PAYLOAD_MAX) {
                    if(p->used<n+10) return;
                    if(!memcmp(b+6+n,tail,4)) { cb(kind,b+6,n,ctx); consume(p,n+10); continue; }
                }
            }
        }
        p->bad_bytes++; consume(p,1);
    }
}
void ld_feed(ld_parser *p,const uint8_t *data,size_t size,ld_frame_cb cb,void *ctx) {
    for(size_t i=0;i<size;i++) {
        if(p->used==sizeof(p->bytes)) { p->bad_bytes++; consume(p,1); }
        p->bytes[p->used++]=data[i]; parse(p,cb,ctx);
    }
}
bool ld_decode_reading(ld_frame_kind kind,const uint8_t *data,size_t size,ld_reading *out) {
    memset(out,0,sizeof(*out));
    if(kind==LD_MINIMAL && size==5 && data[1]<=3) {
        out->state=data[1]; out->distance_cm=ld_u16(data+2); return true;
    }
    if(kind==LD_DATA && size==70 && data[0]==1 && data[1]<=3) {
        out->state=data[1]; out->distance_cm=ld_u16(data+2); out->energy_valid=true;
        for(unsigned i=0;i<LD_GATES;i++) out->energy[i]=ld_u32(data+6+4*i);
        return true;
    }
    return false;
}
bool ld_decode_progress(ld_frame_kind kind,const uint8_t *data,size_t size,uint16_t *hundredths) {
    if(kind!=LD_DATA || size!=3 || data[0]!=3) return false;
    /* The wire value is an integer percentage: 100 (64 00) means complete.
     * Keep the dashboard's internal hundredths-of-a-percent representation. */
    uint16_t percent=ld_u16(data+1);
    if(percent>100) return false;
    *hundredths=percent*100; return true;
}
const char *ld_validate(const ld_settings *s) {
    if(s->far_gate<1 || s->far_gate>16 || s->near_gate>s->far_gate) return "Invalid distance gates";
    if(s->absence_s<10 || s->absence_s>120) return "Absence delay must be 10–120 s";
    if(s->status_hz_x10<5 || s->status_hz_x10>80 || s->status_hz_x10%5 ||
       s->distance_hz_x10<5 || s->distance_hz_x10>80 || s->distance_hz_x10%5) return "Report rates must use 0.5 Hz steps from 0.5 to 8 Hz";
    if(s->response!=5 && s->response!=10) return "Response must be normal (5) or fast (10)";
    for(unsigned i=0;i<LD_GATES;i++) if(s->trigger[i]<10 || s->trigger[i]>95 || s->hold[i]<10 || s->hold[i]>95) return "Thresholds must be 10–95";
    return NULL;
}
