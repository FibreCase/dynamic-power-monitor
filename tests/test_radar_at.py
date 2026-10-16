"""Exercise the production UART worker with fragmented, interleaved replies."""
import re
import unittest
from test_runtime import ROOT, run_c


class RadarTests(unittest.TestCase):
    def test_protocol_and_worker(self):
        code = (ROOT / 'main/radar_at.c').read_text()
        code = re.sub(r'^#include[^\n]*\n', '', code, flags=re.M)
        header = (ROOT / 'main/radar_at.h').read_text()
        header = re.sub(r'^#(?:include|pragma)[^\n]*\n', '', header, flags=re.M)
        run_c(r'''
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <assert.h>
#include <setjmp.h>
typedef int esp_err_t;
typedef int SemaphoreHandle_t;
typedef int TaskHandle_t;
typedef int nvs_handle_t;
typedef struct {unsigned baud_rate;int data_bits,parity,stop_bits,flow_ctrl,source_clk;} uart_config_t;
#define ESP_OK 0
#define ESP_FAIL 1
#define ESP_ERR_INVALID_ARG 2
#define ESP_ERR_TIMEOUT 3
#define ESP_ERR_INVALID_STATE 4
#define ESP_ERR_NO_MEM 5
#define GPIO_NUM_21 21
#define GPIO_NUM_20 20
#define UART_NUM_0 0
#define UART_DATA_8_BITS 8
#define UART_PARITY_DISABLE 0
#define UART_STOP_BITS_1 1
#define UART_HW_FLOWCTRL_DISABLE 0
#define UART_SCLK_DEFAULT 0
#define UART_PIN_NO_CHANGE -1
#define NVS_READWRITE 1
#define NVS_READONLY 0
#define portMAX_DELAY 0
#define pdPASS 1
#define pdTRUE 1
#define pdMS_TO_TICKS(x) (x)
static int64_t now;
static char rx[8192];static size_t pos;
static unsigned local_baud=115200,module_baud=115200,pending_baud=115200,saved_baud;
static unsigned writes,resets,notify_count,queries;
static int scenario;
static jmp_buf done;
static int64_t esp_timer_get_time(void){return now;}
static int uart_read_bytes(int p,uint8_t *b,int n,int ticks){if(rx[pos]){*b=rx[pos++];return 1;}now+=25000;return 0;}
static void uart_flush_input(int p){rx[0]=0;pos=0;}
static int uart_set_baudrate(int p,unsigned b){local_baud=b;return 0;}
static int uart_wait_tx_done(int p,int t){return 0;}
static int uart_write_bytes(int p,const char *buf,int len){
 char cmd[64];memcpy(cmd,buf,len);cmd[len-2]=0;rx[0]=0;pos=0;
 if(local_baud!=module_baud)return len;
 if(!strcmp(cmd,"       "))strcpy(rx,"STOP\r\n");
 else if(!strcmp(cmd,"AT"))strcpy(rx,"OK\r\n");
 else if(!strcmp(cmd,"AT+RESET")){resets++;module_baud=pending_baud;if(scenario!=4)strcpy(rx,"AT+RESET\r\nAT+HOLD=40\r\n");}
 else if(!strcmp(cmd,"AT+START")){strcpy(rx,scenario==7?"AT+ERR\r\n":"AT+OK\r\n");}
 else if(strchr(cmd,'?')){queries++;char name[16];snprintf(name,sizeof(name),"%s",cmd+3);name[strlen(name)-1]=0;unsigned value=10;if(!strcmp(name,"BAUD"))value=5;else if(!strcmp(name,"GPIO"))value=1;else if(!strcmp(name,"TAGOUT"))value=2;else if(!strcmp(name,"HOLD"))value=100000;else if(strlen(name)==2&&name[0]=='R'){unsigned vals[]={200,500,1000};value=vals[name[1]-'1'];}if(scenario==6&&!strcmp(name,"HOLD"))strcpy(rx,"AT+ERR\r\n");else snprintf(rx,sizeof(rx),"1,59cm,62\r\n%s\r\nAT+%s = %u\r\n",cmd,name,value);}
 else {writes++;if(scenario==2)strcpy(rx,"AT+ERR\r\n");else if(scenario==3)strcpy(rx,"1,59cm,62\r\n");else {if(!strncmp(cmd,"AT+BAUD=",8)){unsigned b[]={0,9600,19200,38400,57600,115200,230400,256000,460800};pending_baud=b[atoi(cmd+8)];}snprintf(rx,sizeof(rx),"1,59cm,62\r\n%s\r\nAT+OK\r\n",cmd);}}
 return len;
}
static int xSemaphoreCreateMutex(void){return 1;}
#define xSemaphoreTake(...) ((void)0)
#define xSemaphoreGive(...) ((void)0)
#define xTaskNotifyGive(...) ((void)0)
static unsigned ulTaskNotifyTake(int clear,int ticks){if(notify_count++)longjmp(done,1);return 1;}
static int nvs_open(const char *s,int mode,int *n){*n=1;return mode==NVS_READONLY?1:0;}
static int nvs_set_u32(int n,const char *key,unsigned b){saved_baud=b;return 0;}
static int nvs_commit(int n){return 0;}
static void nvs_close(int n){}
static int nvs_get_u32(int n,const char*k,uint32_t*b){return 1;}
static int uart_param_config(int p,const uart_config_t*c){return 0;}
static int uart_set_pin(int p,int tx,int rx,int rts,int cts){return 0;}
static int uart_driver_install(int p,int a,int b,int c,void*d,int e){return 0;}
static int xTaskCreate(void(*fn)(void*),const char*n,int stack,void*arg,int pri,int*t){return 1;}
''' + header + code + r'''
static void execute(const char *name,unsigned value,int mode){
 scenario=mode;writes=resets=notify_count=0;now=0;
 assert(radar_at_submit(name,value)==ESP_OK);
 assert(radar_at_submit("HOLD",40)==ESP_ERR_INVALID_STATE);
 if(!setjmp(done))worker(NULL);
 assert(!s_state.busy);
}
int main(void){
 assert(radar_at_init()==0);
 assert(radar_at_submit("HOLD",0)==ESP_ERR_INVALID_ARG);
 assert(radar_at_submit("HOLD\r\nAT+RESET",40)==ESP_ERR_INVALID_ARG);
 assert(radar_at_submit("BAUD",9)==ESP_ERR_INVALID_ARG);
 execute("HOLD",40,0);assert(writes==1&&resets==1&&s_state.written&&s_state.restarted&&!s_state.error);
 execute("R1",600,0);assert(writes==0&&resets==1&&!s_state.written&&s_state.restarted&&!strcmp(s_state.error,"invalid_distance_order"));
 execute("R2",600,0);assert(writes==1&&s_state.written&&!s_state.error);
 execute("TRITH",2,2);assert(writes==1&&s_state.restarted&&!strcmp(s_state.error,"write_rejected"));
 execute("GPIO",1,3);assert(writes==1&&!s_state.written&&!strcmp(s_state.error,"write_timeout"));
 execute("HOLD",50,4);assert(s_state.written&&!s_state.restarted&&!strcmp(s_state.error,"restart_unconfirmed"));
 execute("BAUD",6,0);assert(s_state.baud==230400&&saved_baud==230400&&module_baud==230400&&!s_state.error);
 /* Externally changed baud is recovered before any write. */
 module_baud=pending_baud=9600;
 execute("HOLD",60,0);assert(s_state.baud==9600&&writes==1&&s_state.restarted);
 /* Query all reads matching replies and resumes without writes or reboot. */
 for(int mode=0;mode<3;mode++){
 scenario=mode==0?0:mode==1?6:7;writes=resets=notify_count=queries=0;
 assert(radar_at_query_all()==ESP_OK);
 assert(radar_at_submit("HOLD",40)==ESP_ERR_INVALID_STATE);
 if(!setjmp(done))worker(NULL);
 assert(s_state.query&&!s_state.busy&&s_state.resumed&&writes==0&&queries==15);
 for(int i=0;i<15;i++)assert(s_state.results[i]==(scenario==6&&i==3?2:1));
 for(int i=15;i<RADAR_PARAMETER_COUNT;i++)assert(s_state.results[i]==0);
 if(scenario==6)assert(!strcmp(s_state.error,"query_failed"));
 else {assert(!s_state.error);assert(s_state.values[3]==100000);}
 assert(resets==(scenario==7?1:0));
 }
 /* Monitor preserves raw bytes and independent cursors across wrap/reset. */
 uint8_t output[512],raw[5000];uint64_t next,dropped;bool reset,more;
 s_output_head=0;
 const uint8_t text[]={ 'a',' ', 'T','\r','\n',0,255 };
 output_record(text,sizeof(text));
 assert(radar_at_output(0,false,output,sizeof(output),&next,&dropped,&reset,&more)==sizeof(text));
 assert(!memcmp(text,output,sizeof(text))&&next==sizeof(text)&&!dropped&&!reset&&!more);
 assert(radar_at_output(next,false,output,sizeof(output),&next,&dropped,&reset,&more)==0);
 memset(raw,'z',sizeof(raw));output_record(raw,sizeof(raw));
 assert(radar_at_output(0,false,output,sizeof(output),&next,&dropped,&reset,&more)==512);
 assert(dropped==sizeof(text)+sizeof(raw)-OUTPUT_CAPACITY&&more);
 assert(radar_at_output(99999,false,output,sizeof(output),&next,&dropped,&reset,&more)==512&&reset&&!more);
 assert(radar_at_output(0,true,output,sizeof(output),&next,&dropped,&reset,&more)==512&&!reset&&!more);
 return 0;
}
''')
