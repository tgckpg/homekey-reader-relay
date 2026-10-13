#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <inttypes.h>
#include "sdkconfig.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "radar.h"

static const char *TAG="bench";
static char token[33];
static esp_netif_t *station;
extern const uint8_t web_start[] asm("_binary_web_html_start");
extern const uint8_t web_end[] asm("_binary_web_html_end");

static esp_err_t json_reply(httpd_req_t *req,cJSON *obj,const char *status) {
    char *text=cJSON_PrintUnformatted(obj); cJSON_Delete(obj);
    if(!text) return httpd_resp_send_err(req,HTTPD_500_INTERNAL_SERVER_ERROR,"Out of memory");
    httpd_resp_set_type(req,"application/json"); httpd_resp_set_hdr(req,"Cache-Control","no-store");
    if(status) httpd_resp_set_status(req,status);
    esp_err_t err=httpd_resp_send(req,text,HTTPD_RESP_USE_STRLEN); free(text); return err;
}
static esp_err_t error_reply(httpd_req_t *req,const char *message,const char *status) {
    cJSON *obj=cJSON_CreateObject(); cJSON_AddStringToObject(obj,"error",message); return json_reply(req,obj,status);
}
static bool valid_host(httpd_req_t *req) {
    char host[64];
    if(httpd_req_get_hdr_value_str(req,"Host",host,sizeof(host))!=ESP_OK) return false;
    char *port=strchr(host,':');
    if(port) { if(strcmp(port,":80")) return false; *port=0; }
    if(!strcmp(host,"192.168.4.1")) return true;
    if(station) {
        esp_netif_ip_info_t ip;
        if(esp_netif_get_ip_info(station,&ip)==ESP_OK && ip.ip.addr) {
            char addr[24]; snprintf(addr,sizeof(addr),IPSTR,IP2STR(&ip.ip));
            if(!strcmp(host,addr)) return true;
        }
    }
    return false;
}
static bool authorized(httpd_req_t *req) {
    if(!valid_host(req)) { error_reply(req,"Open the dashboard using the ESP32 IP address","403 Forbidden"); return false; }
    char supplied[40];
    if(httpd_req_get_hdr_value_str(req,"X-Bench-Token",supplied,sizeof(supplied))!=ESP_OK || strcmp(supplied,token)) {
        error_reply(req,"Reload the dashboard to refresh the session","403 Forbidden"); return false;
    }
    return true;
}
static cJSON *reading_json(const ld_reading *r,uint32_t stamp) {
    cJSON *o=cJSON_CreateObject();
    cJSON_AddNumberToObject(o,"uptime_ms",stamp); cJSON_AddNumberToObject(o,"state",r->state);
    cJSON_AddBoolToObject(o,"present",r->state>=2); cJSON_AddNumberToObject(o,"distance_cm",r->distance_cm);
    cJSON_AddStringToObject(o,"mode",r->energy_valid?"standard":"minimal");
    if(r->energy_valid) { cJSON *energy=cJSON_AddArrayToObject(o,"energy"); for(unsigned i=0;i<16;i++) cJSON_AddItemToArray(energy,cJSON_CreateNumber(r->energy[i])); }
    return o;
}
static cJSON *info_json(const radar_state *s) {
    cJSON *o=cJSON_CreateObject(); cJSON_AddStringToObject(o,"firmware",s->firmware);
    cJSON_AddNumberToObject(o,"uart_rx",CONFIG_BENCH_UART_RX); cJSON_AddNumberToObject(o,"uart_tx",CONFIG_BENCH_UART_TX);
    cJSON_AddStringToObject(o,"firmware_raw",s->firmware_raw); cJSON_AddStringToObject(o,"serial",s->serial); return o;
}
static radar_state *snapshot(void) {
    radar_state *s=malloc(sizeof(*s)); if(s) radar_copy(s); return s;
}
static esp_err_t state_reply(httpd_req_t *req) {
    radar_state *s=snapshot(); if(!s) return error_reply(req,"Out of memory","500 Internal Server Error");
    cJSON *o=cJSON_CreateObject();
    uint32_t now=(uint32_t)(esp_timer_get_time()/1000);
    cJSON_AddBoolToObject(o,"connected",s->seen && (uint32_t)(now-s->last_ms)<10000);
    cJSON_AddBoolToObject(o,"demo",false);
    cJSON_AddItemToObject(o,"latest",s->seen?reading_json(&s->latest,s->last_ms):cJSON_CreateObject());
    if(s->seen) cJSON_AddNumberToObject(o,"age",(uint32_t)(now-s->last_ms)/1000.0); else cJSON_AddNullToObject(o,"age");
    if(s->settings_valid) cJSON_AddItemToObject(o,"settings",radar_settings_json(&s->settings)); else cJSON_AddNullToObject(o,"settings");
    cJSON_AddItemToObject(o,"info",info_json(s)); cJSON_AddStringToObject(o,"error",s->error);
    cJSON_AddNumberToObject(o,"reports",s->reports); cJSON_AddNumberToObject(o,"samples",s->history_count);
    cJSON_AddNumberToObject(o,"bad_bytes",s->bad_bytes); cJSON_AddNumberToObject(o,"heap_free",esp_get_free_heap_size());
    cJSON_AddBoolToObject(o,"backup_available",s->backup_valid);
    cJSON *job=cJSON_AddObjectToObject(o,"job"); cJSON_AddNumberToObject(job,"id",s->job_id);
    cJSON_AddBoolToObject(job,"busy",s->job_busy); cJSON_AddStringToObject(job,"action",s->job_action); cJSON_AddStringToObject(job,"status",s->job_status);
    if(s->calibration_state) {
        static const char *names[]={"idle","countdown — leave the detection area","running","complete — read settings to see new thresholds","failed or timed out — completion unconfirmed"};
        cJSON *c=cJSON_AddObjectToObject(o,"calibration"); cJSON_AddNumberToObject(c,"state",s->calibration_state);
        cJSON_AddStringToObject(c,"status",names[s->calibration_state]); cJSON_AddNumberToObject(c,"progress",s->calibration_progress/100.0);
        cJSON_AddNumberToObject(c,"remaining_s",s->calibration_state==1?(int32_t)(s->calibration_due_ms-now)/1000.0:0);
    } else cJSON_AddNullToObject(o,"calibration");
    cJSON *history=cJSON_AddArrayToObject(o,"history");
    for(unsigned i=0;i<s->history_count;i++) {
        unsigned index=(s->history_next+HISTORY_SIZE-s->history_count+i)%HISTORY_SIZE;
        const radar_sample *r=&s->history[index]; cJSON *item=cJSON_CreateObject();
        cJSON_AddNumberToObject(item,"uptime_ms",r->uptime_ms); cJSON_AddNumberToObject(item,"distance_cm",r->reading.distance_cm);
        cJSON_AddBoolToObject(item,"present",r->reading.state>=2); cJSON_AddItemToArray(history,item);
    }
    cJSON *logs=cJSON_AddArrayToObject(o,"logs");
    for(unsigned i=0;i<s->log_count;i++) cJSON_AddItemToArray(logs,cJSON_CreateString(s->logs[(s->log_next+LOG_SIZE-s->log_count+i)%LOG_SIZE]));
    cJSON *network=cJSON_AddObjectToObject(o,"network"); cJSON_AddStringToObject(network,"ap_ssid",CONFIG_BENCH_AP_SSID);
    cJSON_AddStringToObject(network,"ap_ip","192.168.4.1");
    if(station) { esp_netif_ip_info_t ip; if(esp_netif_get_ip_info(station,&ip)==ESP_OK && ip.ip.addr) { char addr[24]; snprintf(addr,sizeof(addr),IPSTR,IP2STR(&ip.ip)); cJSON_AddStringToObject(network,"sta_ip",addr); } }
    free(s); return json_reply(req,o,NULL);
}
static esp_err_t backup_reply(httpd_req_t *req,bool previous) {
    radar_state *s=snapshot(); if(!s) return error_reply(req,"Out of memory","500 Internal Server Error");
    if(previous?!s->backup_valid:!s->settings_valid) { free(s); return error_reply(req,"No settings available; read the sensor first","409 Conflict"); }
    cJSON *o=cJSON_CreateObject(); cJSON_AddStringToObject(o,"format","ld2410s-settings-v1");
    cJSON_AddItemToObject(o,"info",info_json(s)); cJSON_AddItemToObject(o,"settings",radar_settings_json(previous?&s->backup:&s->settings));
    free(s); return json_reply(req,o,NULL);
}
static esp_err_t csv_reply(httpd_req_t *req) {
    radar_state *s=snapshot(); if(!s) return error_reply(req,"Out of memory","500 Internal Server Error");
    httpd_resp_set_type(req,"text/csv"); httpd_resp_set_hdr(req,"Cache-Control","no-store");
    char line[400]; int len=snprintf(line,sizeof(line),"uptime_ms,state,present,distance_cm");
    for(unsigned i=0;i<16;i++) len+=snprintf(line+len,sizeof(line)-len,",energy_%u",i);
    len+=snprintf(line+len,sizeof(line)-len,"\n");
    esp_err_t err=httpd_resp_send_chunk(req,line,len);
    for(unsigned i=0;i<s->history_count && err==ESP_OK;i++) {
        const radar_sample *r=&s->history[(s->history_next+HISTORY_SIZE-s->history_count+i)%HISTORY_SIZE];
        len=snprintf(line,sizeof(line),"%"PRIu32",%u,%u,%u",r->uptime_ms,r->reading.state,r->reading.state>=2,r->reading.distance_cm);
        for(unsigned k=0;k<16;k++) {
            if(r->reading.energy_valid) len+=snprintf(line+len,sizeof(line)-len,",%"PRIu32,r->reading.energy[k]);
            else len+=snprintf(line+len,sizeof(line)-len,",");
        }
        len+=snprintf(line+len,sizeof(line)-len,"\n"); err=httpd_resp_send_chunk(req,line,len);
    }
    free(s); if(err==ESP_OK) err=httpd_resp_send_chunk(req,NULL,0); return err;
}
static esp_err_t root(httpd_req_t *req) {
    if(!valid_host(req)) return error_reply(req,"Open the dashboard using the ESP32 IP address","403 Forbidden");
    httpd_resp_set_type(req,"text/html; charset=utf-8"); httpd_resp_set_hdr(req,"Cache-Control","no-store");
    const char *start=(const char *)web_start,*marker=strstr(start,"__TOKEN__");
    if(!marker) return httpd_resp_send_err(req,HTTPD_500_INTERNAL_SERVER_ERROR,"Missing UI token marker");
    esp_err_t err=httpd_resp_send_chunk(req,start,marker-start);
    if(err==ESP_OK) err=httpd_resp_send_chunk(req,token,strlen(token));
    if(err==ESP_OK) err=httpd_resp_send_chunk(req,marker+9,(const char *)web_end-marker-9-1);
    if(err==ESP_OK) err=httpd_resp_send_chunk(req,NULL,0);
    return err;
}
static esp_err_t get_api(httpd_req_t *req) {
    if(!authorized(req)) return ESP_OK;
    if(!strcmp(req->uri,"/api/state")) return state_reply(req);
    if(!strcmp(req->uri,"/api/backup")) return backup_reply(req,false);
    if(!strcmp(req->uri,"/api/previous-backup")) return backup_reply(req,true);
    if(!strcmp(req->uri,"/api/csv")) return csv_reply(req);
    return error_reply(req,"Not found","404 Not Found");
}
static bool integer(const cJSON *o,const char *key,unsigned lo,unsigned hi,uint16_t *out) {
    const cJSON *v=cJSON_GetObjectItemCaseSensitive(o,key);
    if(!cJSON_IsNumber(v) || !isfinite(v->valuedouble) || v->valuedouble<lo || v->valuedouble>hi || floor(v->valuedouble)!=v->valuedouble) return false;
    *out=(uint16_t)v->valuedouble; return true;
}
static esp_err_t post_api(httpd_req_t *req) {
    if(!authorized(req)) return ESP_OK;
    if(req->content_len<2 || req->content_len>4096) return error_reply(req,"Invalid request size","400 Bad Request");
    char *text=malloc(req->content_len+1); if(!text) return error_reply(req,"Out of memory","500 Internal Server Error");
    size_t used=0;
    while(used<req->content_len) {
        int n=httpd_req_recv(req,text+used,req->content_len-used);
        if(n<=0) { free(text); return error_reply(req,"Request receive failed","408 Request Timeout"); }
        used+=n;
    }
    text[used]=0; cJSON *args=cJSON_Parse(text); free(text);
    if(!cJSON_IsObject(args)) { cJSON_Delete(args); return error_reply(req,"Invalid JSON object","400 Bad Request"); }
    radar_job j={0}; const char *error=NULL;
    if(!strcmp(req->uri,"/api/read")) j.kind=JOB_READ;
    else if(!strcmp(req->uri,"/api/write") || !strcmp(req->uri,"/api/validate")) {
        if(!radar_parse_settings(cJSON_GetObjectItemCaseSensitive(args,"settings"),&j.settings,&error)) { cJSON_Delete(args); return error_reply(req,error,"400 Bad Request"); }
        if(!strcmp(req->uri,"/api/validate")) { cJSON_Delete(args); cJSON *o=cJSON_CreateObject(); cJSON_AddBoolToObject(o,"valid",true); return json_reply(req,o,NULL); }
        j.kind=JOB_WRITE;
    } else if(!strcmp(req->uri,"/api/mode")) {
        const cJSON *v=cJSON_GetObjectItemCaseSensitive(args,"mode");
        if(!cJSON_IsString(v) || (strcmp(v->valuestring,"standard") && strcmp(v->valuestring,"minimal"))) { cJSON_Delete(args); return error_reply(req,"Invalid mode","400 Bad Request"); }
        j.kind=!strcmp(v->valuestring,"standard")?JOB_STANDARD:JOB_MINIMAL;
    } else if(!strcmp(req->uri,"/api/calibrate")) {
        j.kind=JOB_CALIBRATE;
        if(!integer(args,"trigger",1,65535,&j.trigger) || !integer(args,"hold",1,65535,&j.hold) || !integer(args,"seconds",10,600,&j.seconds) || !integer(args,"delay",0,120,&j.delay)) { cJSON_Delete(args); return error_reply(req,"Invalid calibration factors, scan time, or start delay","400 Bad Request"); }
    } else if(!strcmp(req->uri,"/api/restore")) {
        radar_state *s=snapshot();
        if(!s) { cJSON_Delete(args); return error_reply(req,"Out of memory","500 Internal Server Error"); }
        if(!s->backup_valid) { free(s); cJSON_Delete(args); return error_reply(req,"No previous backup available","409 Conflict"); }
        j.kind=JOB_RESTORE; j.settings=s->backup; free(s);
    } else { cJSON_Delete(args); return error_reply(req,"Not found","404 Not Found"); }
    cJSON_Delete(args); uint32_t id;
    if(!radar_submit(&j,&id,&error)) return error_reply(req,error,"409 Conflict");
    cJSON *o=cJSON_CreateObject(); cJSON_AddNumberToObject(o,"job_id",id); return json_reply(req,o,"202 Accepted");
}
static void wifi_event(void *arg,esp_event_base_t base,int32_t id,void *data) {
    (void)arg;
    if(base==WIFI_EVENT && id==WIFI_EVENT_STA_START) esp_wifi_connect();
    if(base==WIFI_EVENT && id==WIFI_EVENT_STA_DISCONNECTED) { ESP_LOGW(TAG,"Home Wi-Fi disconnected; AP remains available; retrying"); esp_wifi_connect(); }
    if(base==IP_EVENT && id==IP_EVENT_STA_GOT_IP) { ip_event_got_ip_t *event=data; ESP_LOGI(TAG,"Home network dashboard: http://"IPSTR"/",IP2STR(&event->ip_info.ip)); }
}
static void wifi_start(void) {
    size_t ap_ssid_len=strlen(CONFIG_BENCH_AP_SSID),ap_password_len=strlen(CONFIG_BENCH_AP_PASSWORD);
    size_t sta_ssid_len=strlen(CONFIG_BENCH_STA_SSID),sta_password_len=strlen(CONFIG_BENCH_STA_PASSWORD);
    if(!ap_ssid_len || ap_ssid_len>32 || ap_password_len<8 || ap_password_len>63 || sta_ssid_len>32 || sta_password_len>63) { ESP_LOGE(TAG,"Invalid Wi-Fi configuration; check menuconfig"); abort(); }
    ESP_ERROR_CHECK(esp_netif_init()); ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();
    if(sta_ssid_len) station=esp_netif_create_default_wifi_sta();
    wifi_init_config_t config=WIFI_INIT_CONFIG_DEFAULT(); ESP_ERROR_CHECK(esp_wifi_init(&config));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT,ESP_EVENT_ANY_ID,wifi_event,NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT,IP_EVENT_STA_GOT_IP,wifi_event,NULL));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(sta_ssid_len?WIFI_MODE_APSTA:WIFI_MODE_AP));
    wifi_config_t ap={.ap={.ssid_len=ap_ssid_len,.channel=1,.authmode=WIFI_AUTH_WPA2_PSK,.max_connection=3,.pmf_cfg={.required=false}}};
    memcpy(ap.ap.ssid,CONFIG_BENCH_AP_SSID,ap_ssid_len); memcpy(ap.ap.password,CONFIG_BENCH_AP_PASSWORD,ap_password_len);
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP,&ap));
    if(sta_ssid_len) {
        wifi_config_t sta={0}; memcpy(sta.sta.ssid,CONFIG_BENCH_STA_SSID,sta_ssid_len); memcpy(sta.sta.password,CONFIG_BENCH_STA_PASSWORD,sta_password_len);
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA,&sta));
    }
    ESP_ERROR_CHECK(esp_wifi_start()); ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_LOGI(TAG,"Connect to Wi-Fi '%s'; open http://192.168.4.1/",CONFIG_BENCH_AP_SSID);
}
void app_main(void) {
    /* Preserve existing NVS; do not automatically erase an old board's data. */
    ESP_ERROR_CHECK(nvs_flash_init());
    if(CONFIG_BENCH_UART_TX==CONFIG_BENCH_UART_RX) { ESP_LOGE(TAG,"UART TX and RX must use different pins"); abort(); }
    for(unsigned i=0;i<4;i++) snprintf(token+8*i,9,"%08"PRIx32,esp_random());
    radar_start(); wifi_start();
    httpd_config_t cfg=HTTPD_DEFAULT_CONFIG(); cfg.uri_match_fn=httpd_uri_match_wildcard;
    cfg.stack_size=8192; cfg.lru_purge_enable=true; cfg.max_open_sockets=4;
    httpd_handle_t server; ESP_ERROR_CHECK(httpd_start(&server,&cfg));
    const httpd_uri_t routes[]={
        {.uri="/",.method=HTTP_GET,.handler=root},
        {.uri="/api/*",.method=HTTP_GET,.handler=get_api},
        {.uri="/api/*",.method=HTTP_POST,.handler=post_api},
    };
    for(unsigned i=0;i<sizeof(routes)/sizeof(routes[0]);i++) ESP_ERROR_CHECK(httpd_register_uri_handler(server,&routes[i]));
}
