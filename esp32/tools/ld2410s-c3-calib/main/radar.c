#include "radar.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <inttypes.h>
#include "sdkconfig.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#define UART_PORT UART_NUM_1
#define WAIT_MS 2500
static radar_state st;
static SemaphoreHandle_t state_mutex, ack_sem;
static QueueHandle_t jobs;
static uint16_t pending;
static bool waiting;
static uint8_t ack[LD_PAYLOAD_MAX];
static size_t ack_size;
static const char *TAG="radar";
static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time()/1000); }
static void lock(void) { xSemaphoreTake(state_mutex,portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(state_mutex); }
static void note(const char *fmt,...) {
    char text[LOG_LEN]; va_list ap; va_start(ap,fmt); vsnprintf(text,sizeof(text),fmt,ap); va_end(ap);
    ESP_LOGI(TAG,"%s",text);
    lock(); snprintf(st.logs[st.log_next],LOG_LEN,"%"PRIu32"s %.125s",now_ms()/1000,text);
    st.log_next=(st.log_next+1)%LOG_SIZE; if(st.log_count<LOG_SIZE) st.log_count++; unlock();
}
void radar_copy(radar_state *out) {
    lock();
    if(st.calibration_state==2 && (uint32_t)(now_ms()-st.calibration_started_ms)>((uint32_t)st.calibration_seconds+30)*1000) {
        st.calibration_state=4;
        snprintf(st.error,sizeof(st.error),"Calibration timed out; completion unconfirmed. Read settings before trying again.");
    }
    memcpy(out,&st,sizeof(st)); unlock();
}
static void receive(ld_frame_kind kind,const uint8_t *data,size_t size,void *ctx) {
    (void)ctx;
    if(kind==LD_ACK && size>=2) {
        uint16_t cmd=ld_u16(data);
        bool signal=false;
        lock();
        if(cmd==0x109 && size>=4 && ld_u16(data+2) && st.calibration_state==2) {
            st.calibration_state=4; snprintf(st.error,sizeof(st.error),"Sensor rejected calibration (status %u)",ld_u16(data+2));
        }
        if(waiting && cmd==pending) { memcpy(ack,data+2,size-2); ack_size=size-2; waiting=false; signal=true; }
        unlock();
        if(signal) xSemaphoreGive(ack_sem);
        char hex[80]; size_t used=0;
        for(size_t i=0;i<size && used+4<sizeof(hex);i++) used+=(size_t)snprintf(hex+used,sizeof(hex)-used,"%02x ",data[i]);
        hex[used]=0; note("ACK %04x: %s",cmd,hex); return;
    }
    uint16_t progress;
    if(ld_decode_progress(kind,data,size,&progress)) {
        lock();
        if(st.calibration_state==2) { st.calibration_progress=progress; if(progress==10000) st.calibration_state=3; }
        unlock(); return;
    }
    ld_reading r;
    if(ld_decode_reading(kind,data,size,&r)) {
        uint32_t time=now_ms(); lock();
        st.latest=r; st.last_ms=time; st.seen=true; st.reports++;
        st.history[st.history_next]=(radar_sample){.uptime_ms=time,.reading=r};
        st.history_next=(st.history_next+1)%HISTORY_SIZE;
        if(st.history_count<HISTORY_SIZE) st.history_count++;
        unlock(); return;
    }
    note("Unknown frame: type=%d bytes=%u",kind,(unsigned)size);
}
static void reader(void *arg) {
    (void)arg; ld_parser parser={0}; uint8_t bytes[128]; uint32_t last_byte=now_ms();
    for(;;) {
        int n=uart_read_bytes(UART_PORT,bytes,sizeof(bytes),pdMS_TO_TICKS(100));
        if(n>0) { ld_feed(&parser,bytes,n,receive,NULL); last_byte=now_ms(); }
        else if(parser.used && (uint32_t)(now_ms()-last_byte)>500) { parser.bad_bytes+=parser.used; parser.used=0; }
        lock(); st.bad_bytes=parser.bad_bytes; unlock();
    }
}
static esp_err_t command(uint16_t cmd,const uint8_t *payload,size_t size,uint8_t *result,size_t *result_size) {
    uint8_t bytes[LD_PAYLOAD_MAX+12]; size_t n=ld_command(cmd,payload,size,bytes,sizeof(bytes));
    if(!n) return ESP_ERR_INVALID_SIZE;
    while(xSemaphoreTake(ack_sem,0)==pdTRUE) {}
    lock(); pending=cmd|0x100; waiting=true; ack_size=0; unlock();
    note("TX %04x (%u bytes)",cmd,(unsigned)size);
    if(uart_write_bytes(UART_PORT,bytes,n)!=(int)n) { lock(); waiting=false; unlock(); return ESP_FAIL; }
    if(xSemaphoreTake(ack_sem,pdMS_TO_TICKS(WAIT_MS))!=pdTRUE) {
        lock(); waiting=false; unlock(); note("No ACK for %04x",cmd); return ESP_ERR_TIMEOUT;
    }
    lock();
    size_t count=ack_size;
    if(result && result_size && *result_size>=count) memcpy(result,ack,count);
    else if(result && count) { unlock(); return ESP_ERR_INVALID_SIZE; }
    if(result_size) *result_size=count;
    unlock(); return ESP_OK;
}
static esp_err_t checked(uint16_t cmd,const uint8_t *payload,size_t size,uint8_t *result,size_t *result_size) {
    uint8_t raw[LD_PAYLOAD_MAX]; size_t n=sizeof(raw);
    esp_err_t err=command(cmd,payload,size,raw,&n);
    if(err!=ESP_OK) return err;
    if(n<2) return ESP_ERR_INVALID_RESPONSE;
    if(ld_u16(raw)!=0) { note("Rejected %04x, status %u",cmd,ld_u16(raw)); return ESP_FAIL; }
    n-=2;
    if(result) {
        if(!result_size || *result_size<n) return ESP_ERR_INVALID_SIZE;
        memcpy(result,raw+2,n);
    }
    if(result_size) *result_size=n;
    return ESP_OK;
}
static esp_err_t begin(void) { const uint8_t v[]={1,0}; return checked(0xff,v,2,NULL,NULL); }
static esp_err_t end(void) { return checked(0xfe,NULL,0,NULL,NULL); }
static esp_err_t close_config(esp_err_t err) { esp_err_t exit_err=end(); return err==ESP_OK?exit_err:err; }
static void settings_to_values(const ld_settings *s,uint32_t *v) {
    v[0]=s->far_gate; v[1]=s->near_gate; v[2]=s->absence_s; v[3]=s->status_hz_x10; v[4]=s->distance_hz_x10; v[5]=s->response;
}
static void values_to_settings(const uint8_t *p,ld_settings *s) {
    s->far_gate=ld_u32(p); s->near_gate=ld_u32(p+4); s->absence_s=ld_u32(p+8);
    s->status_hz_x10=ld_u32(p+12); s->distance_hz_x10=ld_u32(p+16); s->response=ld_u32(p+20);
}
static esp_err_t read_settings(ld_settings *out) {
    esp_err_t err=begin(); if(err!=ESP_OK) return err;
    uint8_t raw[LD_PAYLOAD_MAX],ids[32]; size_t n=sizeof(raw);
    err=command(0,NULL,0,raw,&n);
    if(err==ESP_OK) {
        lock(); size_t used=0;
        for(size_t i=0;i<n && used+4<sizeof(st.firmware_raw);i++) used+=(size_t)snprintf(st.firmware_raw+used,sizeof(st.firmware_raw)-used,"%02x ",raw[i]);
        st.firmware_raw[used]=0;
        /* 1.1.1 module supplied by user: metadata + trailing 3 version words. */
        if(n==6 || n==12 || n==14) snprintf(st.firmware,sizeof(st.firmware),"%u.%u.%u",ld_u16(raw+n-6),ld_u16(raw+n-4),ld_u16(raw+n-2));
        else snprintf(st.firmware,sizeof(st.firmware),"Unknown (see raw reply)");
        unlock();
    }
    if(err==ESP_OK) { n=sizeof(raw); err=checked(0x11,NULL,0,raw,&n); }
    if(err==ESP_OK && n<2) err=ESP_ERR_INVALID_RESPONSE;
    if(err==ESP_OK) {
        size_t count=ld_u16(raw); if(count>n-2 || count>=sizeof(st.serial)) err=ESP_ERR_INVALID_RESPONSE;
        else { lock(); memcpy(st.serial,raw+2,count); st.serial[count]=0; unlock(); }
    }
    static const uint16_t common_ids[]={5,10,6,2,12,11};
    for(unsigned i=0;i<6;i++) ld_put16(ids+2*i,common_ids[i]);
    if(err==ESP_OK) { n=sizeof(raw); err=checked(0x71,ids,12,raw,&n); }
    if(err==ESP_OK) { if(n!=24) err=ESP_ERR_INVALID_RESPONSE; else values_to_settings(raw,out); }
    for(unsigned i=0;i<16;i++) ld_put16(ids+2*i,i);
    for(unsigned k=0;k<2 && err==ESP_OK;k++) {
        n=sizeof(raw); err=checked(k?0x77:0x73,ids,32,raw,&n);
        if(err==ESP_OK) {
            if(n!=64) err=ESP_ERR_INVALID_RESPONSE;
            else for(unsigned i=0;i<16;i++) (k?out->hold:out->trigger)[i]=ld_u32(raw+4*i);
        }
    }
    err=close_config(err);
    if(err==ESP_OK) { lock(); st.settings=*out; st.settings_valid=true; unlock(); }
    return err;
}
static esp_err_t preserve(const ld_settings *s) {
    nvs_handle_t handle; esp_err_t err=nvs_open("ld2410s_bench",NVS_READWRITE,&handle);
    if(err!=ESP_OK) return err;
    err=nvs_set_blob(handle,"previous",s,sizeof(*s));
    if(err==ESP_OK) err=nvs_commit(handle);
    nvs_close(handle);
    if(err==ESP_OK) { lock(); st.backup=*s; st.backup_valid=true; unlock(); note("Previous settings saved to flash"); }
    return err;
}
static esp_err_t write_settings(const ld_settings *target) {
    if(ld_validate(target)) return ESP_ERR_INVALID_ARG;
    ld_settings before;
    esp_err_t err=read_settings(&before); if(err!=ESP_OK) return err;
    err=preserve(&before); if(err!=ESP_OK) return err;
    err=begin(); if(err!=ESP_OK) return err;
    uint8_t payload[96]; uint32_t values[6]; settings_to_values(target,values);
    static const uint16_t ids[]={5,10,6,2,12,11};
    for(unsigned i=0;i<6;i++) { ld_put16(payload+6*i,ids[i]); ld_put32(payload+6*i+2,values[i]); }
    err=checked(0x70,payload,36,NULL,NULL);
    for(unsigned k=0;k<2 && err==ESP_OK;k++) {
        for(unsigned i=0;i<16;i++) { ld_put16(payload+6*i,i); ld_put32(payload+6*i+2,(k?target->hold:target->trigger)[i]); }
        err=checked(k?0x76:0x72,payload,96,NULL,NULL);
    }
    err=close_config(err);
    if(err==ESP_OK) {
        ld_settings actual;
        err=read_settings(&actual);
        if(err==ESP_OK && memcmp(&actual,target,sizeof(actual))) err=ESP_ERR_INVALID_RESPONSE;
    }
    if(err!=ESP_OK) note("Write failed: partial changes possible. Previous settings remain saved.");
    return err;
}
static esp_err_t mode(bool standard) {
    esp_err_t err=begin(); if(err!=ESP_OK) return err;
    uint8_t v[6]={0}; ld_put32(v+2,standard?1:0);
    return close_config(checked(0x7a,v,6,NULL,NULL));
}
static esp_err_t calibrate(const radar_job *j) {
    ld_settings before; esp_err_t err=read_settings(&before); if(err!=ESP_OK) return err;
    err=preserve(&before); if(err!=ESP_OK) return err;
    lock(); st.calibration_state=1; st.calibration_progress=0; st.calibration_due_ms=now_ms()+j->delay*1000; st.calibration_seconds=j->seconds; unlock();
    note("Calibration starts in %u seconds. Keep the sensor fixed and area empty.",j->delay);
    vTaskDelay(pdMS_TO_TICKS(j->delay*1000));
    err=begin(); if(err!=ESP_OK) return err;
    uint8_t p[6],bytes[18]; ld_put16(p,j->trigger); ld_put16(p+2,j->hold); ld_put16(p+4,j->seconds);
    size_t n=ld_command(9,p,6,bytes,sizeof(bytes));
    lock(); st.calibration_state=2; st.calibration_started_ms=now_ms(); unlock();
    if(uart_write_bytes(UART_PORT,bytes,n)!=(int)n) err=ESP_FAIL;
    /* 0009 responds with progress frames, not a conventional ACK. */
    err=close_config(err);
    if(err==ESP_OK) note("Calibration scan requested; waiting for device progress");
    return err;
}
static void worker(void *arg) {
    (void)arg; radar_job j;
    for(;;) {
        xQueueReceive(jobs,&j,portMAX_DELAY);
        esp_err_t err=ESP_OK;
        switch(j.kind) {
            case JOB_READ: { ld_settings current; err=read_settings(&current); break; }
            case JOB_WRITE: case JOB_RESTORE: err=write_settings(&j.settings); break;
            case JOB_STANDARD: err=mode(true); break;
            case JOB_MINIMAL: err=mode(false); break;
            case JOB_CALIBRATE: err=calibrate(&j); break;
        }
        lock(); st.job_busy=false;
        if(j.kind==JOB_CALIBRATE && st.calibration_state==4 && err==ESP_OK) err=ESP_FAIL;
        if(err==ESP_OK) {
            snprintf(st.job_status,sizeof(st.job_status),"%s",j.kind==JOB_CALIBRATE?"Scan requested — watch device progress":"Done");
            if(j.kind!=JOB_CALIBRATE || st.calibration_state!=4) st.error[0]=0;
        }
        else {
            snprintf(st.job_status,sizeof(st.job_status),"Failed: %s. Check UART wiring/logs; a write may be partial.",esp_err_to_name(err));
            snprintf(st.error,sizeof(st.error),"%s",st.job_status);
            if(j.kind==JOB_CALIBRATE) st.calibration_state=4;
        }
        unlock(); note("Job finished: %s",esp_err_to_name(err));
    }
}
bool radar_submit(const radar_job *j,uint32_t *id,const char **error) {
    static const char *names[]={"read","write","standard","minimal","calibrate","restore"};
    lock();
    if(st.job_busy || st.calibration_state==1 || st.calibration_state==2) { unlock(); *error="A job or calibration is already running"; return false; }
    st.job_busy=true; st.job_id++; *id=st.job_id; st.error[0]=0;
    snprintf(st.job_action,sizeof(st.job_action),"%s",names[j->kind]); snprintf(st.job_status,sizeof(st.job_status),"Working");
    unlock();
    if(xQueueSend(jobs,j,0)!=pdTRUE) { lock(); st.job_busy=false; unlock(); *error="Job queue is full"; return false; }
    return true;
}
void radar_start(void) {
    state_mutex=xSemaphoreCreateMutex(); ack_sem=xSemaphoreCreateBinary(); jobs=xQueueCreate(1,sizeof(radar_job));
    configASSERT(state_mutex && ack_sem && jobs);
    uart_config_t config={.baud_rate=115200,.data_bits=UART_DATA_8_BITS,.parity=UART_PARITY_DISABLE,.stop_bits=UART_STOP_BITS_1,.flow_ctrl=UART_HW_FLOWCTRL_DISABLE,.source_clk=UART_SCLK_DEFAULT};
    ESP_ERROR_CHECK(uart_param_config(UART_PORT,&config));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT,CONFIG_BENCH_UART_TX,CONFIG_BENCH_UART_RX,UART_PIN_NO_CHANGE,UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(UART_PORT,4096,0,0,NULL,0));
    nvs_handle_t h;
    if(nvs_open("ld2410s_bench",NVS_READONLY,&h)==ESP_OK) {
        size_t size=sizeof(st.backup);
        st.backup_valid=nvs_get_blob(h,"previous",&st.backup,&size)==ESP_OK && size==sizeof(st.backup);
        nvs_close(h);
    }
    configASSERT(xTaskCreate(reader,"radar_rx",4096,NULL,5,NULL)==pdPASS);
    configASSERT(xTaskCreate(worker,"radar_jobs",6144,NULL,4,NULL)==pdPASS);
    note("115200 8N1; ESP TX GPIO%d -> sensor RX; ESP RX GPIO%d <- sensor OT1",CONFIG_BENCH_UART_TX,CONFIG_BENCH_UART_RX);
    vTaskDelay(pdMS_TO_TICKS(1500));
    radar_job j={.kind=JOB_READ}; uint32_t id; const char *error;
    radar_submit(&j,&id,&error);
}
static const char *keys[]={"far_gate","near_gate","absence_s","status_hz_x10","distance_hz_x10","response"};
cJSON *radar_settings_json(const ld_settings *s) {
    cJSON *o=cJSON_CreateObject(),*common=cJSON_AddObjectToObject(o,"common");
    uint32_t v[6]; settings_to_values(s,v);
    for(unsigned i=0;i<6;i++) cJSON_AddNumberToObject(common,keys[i],v[i]);
    cJSON *trigger=cJSON_AddArrayToObject(o,"trigger"),*hold=cJSON_AddArrayToObject(o,"hold");
    for(unsigned i=0;i<16;i++) { cJSON_AddItemToArray(trigger,cJSON_CreateNumber(s->trigger[i])); cJSON_AddItemToArray(hold,cJSON_CreateNumber(s->hold[i])); }
    return o;
}
static bool number(const cJSON *o,const char *key,uint32_t *out) {
    const cJSON *v=cJSON_GetObjectItemCaseSensitive(o,key);
    if(!cJSON_IsNumber(v) || !isfinite(v->valuedouble) || v->valuedouble<0 || v->valuedouble>UINT32_MAX || floor(v->valuedouble)!=v->valuedouble) return false;
    *out=(uint32_t)v->valuedouble; return true;
}
bool radar_parse_settings(const cJSON *o,ld_settings *out,const char **error) {
    memset(out,0,sizeof(*out));
    const cJSON *common=cJSON_GetObjectItemCaseSensitive(o,"common"); uint32_t v[6];
    if(!cJSON_IsObject(o) || cJSON_GetArraySize(o)!=3 || !cJSON_IsObject(common) || cJSON_GetArraySize(common)!=6) { *error="Settings need common, trigger and hold"; return false; }
    for(unsigned i=0;i<6;i++) if(!number(common,keys[i],&v[i])) { *error="Invalid common parameter"; return false; }
    out->far_gate=v[0]; out->near_gate=v[1]; out->absence_s=v[2]; out->status_hz_x10=v[3]; out->distance_hz_x10=v[4]; out->response=v[5];
    for(unsigned k=0;k<2;k++) {
        const cJSON *arr=cJSON_GetObjectItemCaseSensitive(o,k?"hold":"trigger");
        if(!cJSON_IsArray(arr) || cJSON_GetArraySize(arr)!=16) { *error="Each threshold array requires 16 gates"; return false; }
        for(unsigned i=0;i<16;i++) {
            const cJSON *item=cJSON_GetArrayItem(arr,i);
            if(!cJSON_IsNumber(item) || !isfinite(item->valuedouble) || item->valuedouble<10 || item->valuedouble>95 || floor(item->valuedouble)!=item->valuedouble) { *error="Thresholds must be integers from 10 to 95"; return false; }
            (k?out->hold:out->trigger)[i]=(uint32_t)item->valuedouble;
        }
    }
    *error=ld_validate(out); return !*error;
}
