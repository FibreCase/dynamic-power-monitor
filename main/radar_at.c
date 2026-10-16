/* USRR235L ASCII protocol, matching uart-at-debug/usrr235.py. UART0 has
 * exactly one owner; no background reports can be mistaken for command ACKs. */
#include "radar_at.h"
#include "config.h"
#include "core.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "driver/uart.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#ifndef CFG_RADAR_UART_TX_PIN
#define CFG_RADAR_UART_TX_PIN GPIO_NUM_21
#endif
#ifndef CFG_RADAR_UART_RX_PIN
#define CFG_RADAR_UART_RX_PIN GPIO_NUM_20
#endif
#ifndef CFG_RADAR_UART_BAUD
#define CFG_RADAR_UART_BAUD 115200
#endif
#define UART_PORT UART_NUM_0
#define TIMEOUT_US 3000000LL
const struct radar_parameter radar_parameters[] = {
  {"BAUD",1,8,"波特率序号：1=9600 / 2=19200 / 3=38400 / 4=57600 / 5=115200 / 6=230400 / 7=256000 / 8=460800",true},
  {"GPIO",0,1,"输出电平极性；改变后可能影响有人/无人控制",true},
  {"TAGOUT",0,2,"目标输出：0 关闭 / 1 状态 / 2 状态、距离和能量",true},
  {"HOLD",1,100000,"无人保持帧数",true},
  {"R1",0,2000,"距离边界 cm；必须 R1 ≤ R2 ≤ R3",true},
  {"R2",0,2000,"距离边界 cm；必须 R1 ≤ R2 ≤ R3",true},
  {"R3",0,2000,"距离边界 cm；必须 R1 ≤ R2 ≤ R3",true},
  {"MR1TH",1,10000,"区域 1 触发能量门限，越小越灵敏",true},
  {"MR2TH",1,10000,"区域 2 触发能量门限，越小越灵敏",true},
  {"MR3TH",1,10000,"区域 3 触发能量门限，越小越灵敏",true},
  {"R1TH",1,10000,"区域 1 保持能量门限，越小越灵敏",true},
  {"R2TH",1,10000,"区域 2 保持能量门限，越小越灵敏",true},
  {"R3TH",1,10000,"区域 3 保持能量门限，越小越灵敏",true},
  {"TRITH",1,10,"十帧内触发帧数",true}, {"ONTH",1,10,"十帧内保持帧数",true},
  {"OUTTRI",1,1,"赋值为有人状态",false},
  {"FREQ",0,4,"工作频率：0..4 = 0.5 / 1 / 2 / 4 / 8 Hz",false},
  {"FTIME",5,100000,"有人工作周期 ms",false}, {"STIME",5,100000,"无人工作周期 ms",false},
};
const size_t radar_parameter_count = sizeof(radar_parameters)/sizeof(radar_parameters[0]);
_Static_assert(sizeof(radar_parameters)/sizeof(radar_parameters[0]) == RADAR_PARAMETER_COUNT, "parameter storage size");
static const unsigned bauds[] = {0,9600,19200,38400,57600,115200,230400,256000,460800};
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static struct radar_at_state s_state = {.phase="idle", .baud=CFG_RADAR_UART_BAUD};

#define OUTPUT_CAPACITY 4096u
static uint8_t s_output[OUTPUT_CAPACITY];
static uint64_t s_output_head;

static void output_record(const uint8_t *data, size_t size) {
  xSemaphoreTake(s_lock,portMAX_DELAY);
  for (size_t i=0;i<size;i++) s_output[s_output_head++ % OUTPUT_CAPACITY]=data[i];
  xSemaphoreGive(s_lock);
}
size_t radar_at_output(uint64_t after, bool latest, uint8_t *data, size_t capacity,
                       uint64_t *next, uint64_t *dropped, bool *reset, bool *more) {
  xSemaphoreTake(s_lock,portMAX_DELAY);
  uint64_t oldest=s_output_head>OUTPUT_CAPACITY ? s_output_head-OUTPUT_CAPACITY : 0;
  *dropped=0; *reset=!latest && after>s_output_head;
  if (latest || *reset) after=s_output_head>capacity ? s_output_head-capacity : 0;
  if (after<oldest) {*dropped=oldest-after;after=oldest;}
  size_t count=s_output_head-after<capacity ? (size_t)(s_output_head-after) : capacity;
  for (size_t i=0;i<count;i++) data[i]=s_output[(after+i)%OUTPUT_CAPACITY];
  *next=after+count;
  *more=*next<s_output_head;
  xSemaphoreGive(s_lock);
  return count;
}
static void drain_input(void) {
  uint8_t bytes[128];
  /* Drain only the driver's bounded RX backlog, never chase a live stream. */
  for (unsigned i=0;i<16;i++) {
    int count=uart_read_bytes(UART_PORT,bytes,sizeof(bytes),0);
    if (count<=0) break;
    output_record(bytes,count);
  }
  uart_flush_input(UART_PORT);
}

/* Bound both memory and total elapsed time even for a continuous target stream. */
static bool read_line(char *line, size_t size, int64_t deadline) {
  size_t used=0;
  bool overflow=false;
  while (esp_timer_get_time()<deadline) {
    uint8_t c;
    if (uart_read_bytes(UART_PORT,&c,1,pdMS_TO_TICKS(20)+1)!=1) continue;
    output_record(&c,1);
    if (c=='\n') {
      if (!overflow && used) {line[used]=0; return true;}
      used=0; overflow=false;
    } else if (c!='\r' && c!=' ' && c!='\t') {
      if (used+1<size) line[used++]=(char)toupper(c);
      else overflow=true;
    }
  }
  return false;
}
static esp_err_t send_command(const char *command) {
  char buf[64];
  int len=snprintf(buf,sizeof(buf),"%s\r\n",command);
  if (len<0 || len>=(int)sizeof(buf)) return ESP_ERR_INVALID_ARG;
  if (uart_write_bytes(UART_PORT,buf,len)!=len) return ESP_FAIL;
  return uart_wait_tx_done(UART_PORT,pdMS_TO_TICKS(500));
}
static esp_err_t request(const char *command, const char *prefix, unsigned *value) {
  esp_err_t err=send_command(command);
  if (err!=ESP_OK) return err;
  char line[128];
  int64_t deadline=esp_timer_get_time()+TIMEOUT_US;
  while (read_line(line,sizeof(line),deadline)) {
    if (!strcmp(line,"AT+ERR")) return ESP_FAIL;
    if (!prefix && (!strcmp(line,"OK") || !strcmp(line,"AT+OK"))) return ESP_OK;
    if (prefix && !strncmp(line,prefix,strlen(prefix))) {
      const char *p=line+strlen(prefix);
      if (!*p) continue;
      unsigned n=0;
      while (*p>='0' && *p<='9' && n<=100000) n=n*10+(*p++-'0');
      if (!*p && n<=100000) {*value=n; return ESP_OK;}
    }
  }
  return ESP_ERR_TIMEOUT;
}
static esp_err_t enter_stop(void) {
  drain_input();
  esp_err_t err=send_command("       ");
  if (err!=ESP_OK) return err;
  char line[128];
  int64_t deadline=esp_timer_get_time()+TIMEOUT_US;
  while (read_line(line,sizeof(line),deadline))
    if (!strcmp(line,"STOP") || !strcmp(line,"AT+ERR")) break;
  err=request("AT",NULL,NULL);
  return err==ESP_FAIL ? request("AT",NULL,NULL) : err;
}
static esp_err_t reset_module(unsigned baud) {
  drain_input();
  esp_err_t err=send_command("AT+RESET");
  if (err!=ESP_OK) return err;
  err=uart_set_baudrate(UART_PORT,baud);
  if (err!=ESP_OK) return err;
  char line[128];
  bool received=false;
  int64_t deadline=esp_timer_get_time()+TIMEOUT_US;
  while (read_line(line,sizeof(line),deadline)) {
    if (!strcmp(line,"AT+ERR")) return ESP_FAIL;
    /* Ignore reset echo/ACK; require boot parameters or detection output. */
    bool printable=true;
    for (const unsigned char *p=(const unsigned char *)line;*p;p++)
      if (*p<33 || *p>126) printable=false;
    if (printable && strcmp(line,"AT+RESET") && strcmp(line,"OK") &&
        strcmp(line,"AT+OK")) received=true;
  }
  return received ? ESP_OK : ESP_ERR_TIMEOUT;
}
static esp_err_t save_baud(unsigned baud) {
  nvs_handle_t nvs;
  esp_err_t err=nvs_open("radar",NVS_READWRITE,&nvs);
  if (err!=ESP_OK) return err;
  err=nvs_set_u32(nvs,"baud",baud);
  if (err==ESP_OK) err=nvs_commit(nvs);
  nvs_close(nvs);
  return err;
}
static void worker(void *arg) {
  (void)arg;
  for (;;) {
    if (!ulTaskNotifyTake(pdTRUE,0)) {
      uint8_t bytes[128];
      xSemaphoreTake(s_mutex,portMAX_DELAY);
      bool saving=s_power_saving;
      xSemaphoreGive(s_mutex);
      int count=uart_read_bytes(UART_PORT,bytes,sizeof(bytes),pdMS_TO_TICKS(saving?500:50)+1);
      if (count>0) output_record(bytes,count);
      continue;
    }
    struct radar_at_state job;
    radar_at_get(&job);
    bool baud_change=!job.query && !strcmp(job.parameter,"BAUD");
    unsigned next_baud=baud_change ? bauds[job.value] : job.baud;
    esp_err_t err=enter_stop();
    /* Recover an externally changed baud, or a BAUD write whose ACK was lost.
     * Only STOP and the harmless AT probe are retried; never repeat a write. */
    if (err!=ESP_OK) {
      for (unsigned i=1;i<9;i++) {
        if (bauds[i]==job.baud) continue;
        uart_set_baudrate(UART_PORT,bauds[i]);
        if (enter_stop()==ESP_OK) {job.baud=bauds[i];err=ESP_OK;break;}
      }
      if (err==ESP_OK) save_baud(job.baud);
      else uart_set_baudrate(UART_PORT,job.baud);
    }
    const char *error=err==ESP_OK ? NULL : "stop_failed";
    if (job.query) {
      if (!error) {
        for (size_t i=0;i<radar_parameter_count;i++) {
          if (!radar_parameters[i].queryable) continue;
          char cmd[24], prefix[24];
          snprintf(cmd,sizeof(cmd),"AT+%s?",radar_parameters[i].name);
          snprintf(prefix,sizeof(prefix),"AT+%s=",radar_parameters[i].name);
          err=request(cmd,prefix,&job.values[i]);
          if (err==ESP_OK && job.values[i]>=radar_parameters[i].min &&
              job.values[i]<=radar_parameters[i].max) job.results[i]=1;
          else {job.results[i]=2;error="query_failed";}
        }
      }
      /* Read-only operation: START resumes detection without rebooting. */
      job.resumed=request("AT+START",NULL,NULL)==ESP_OK;
      if (!job.resumed) {
        job.restarted=reset_module(job.baud)==ESP_OK;
        job.resumed=job.restarted;
      }
      if (!job.resumed && !error) error="resume_failed";
      goto completed;
    }
    if (!error && job.parameter[0]=='R' && job.parameter[1]>='1' &&
        job.parameter[1]<='3' && !job.parameter[2]) {
      unsigned ranges[3];
      for (unsigned i=0;i<3;i++) {
        char cmd[12],prefix[12];
        snprintf(cmd,sizeof(cmd),"AT+R%u?",i+1);
        snprintf(prefix,sizeof(prefix),"AT+R%u=",i+1);
        err=request(cmd,prefix,&ranges[i]);
        if (err!=ESP_OK || ranges[i]>2000) {error="range_query_failed";break;}
      }
      if (!error) {
        ranges[job.parameter[1]-'1']=job.value;
        if (ranges[0]>ranges[1] || ranges[1]>ranges[2]) error="invalid_distance_order";
      }
    }
    if (!error) {
      char cmd[40];
      snprintf(cmd,sizeof(cmd),"AT+%s=%u",job.parameter,job.value);
      err=request(cmd,NULL,NULL);
      if (err!=ESP_OK) error=err==ESP_ERR_TIMEOUT ? "write_timeout" : "write_rejected";
      else job.written=true;
    }
    /* Always try to leave STOP, including after validation or ACK failures.
     * A lost write ACK can still mean BAUD was stored by the radar. */
    unsigned reset_baud=(baud_change && !error) ? next_baud : job.baud;
    if (baud_change && job.written) {
      if (save_baud(next_baud)!=ESP_OK) error="baud_save_failed";
      reset_baud=next_baud;
    }
    job.restarted=reset_module(reset_baud)==ESP_OK;
    job.baud=reset_baud;
    if (!job.restarted && !error) error="restart_unconfirmed";
    job.resumed=job.restarted;
completed:
    job.error=error;
    job.phase=error ? "error" : "done";
    job.busy=false;
    xSemaphoreTake(s_lock,portMAX_DELAY);
    s_state=job;
    xSemaphoreGive(s_lock);
  }
}
void radar_at_get(struct radar_at_state *out) {
  xSemaphoreTake(s_lock,portMAX_DELAY); *out=s_state; xSemaphoreGive(s_lock);
}
esp_err_t radar_at_submit(const char *name, unsigned value) {
  size_t i;
  for(i=0;i<radar_parameter_count;i++) if (!strcmp(name,radar_parameters[i].name)) break;
  if(i==radar_parameter_count || value<radar_parameters[i].min || value>radar_parameters[i].max)
    return ESP_ERR_INVALID_ARG;
  xSemaphoreTake(s_lock,portMAX_DELAY);
  if(s_state.busy) {xSemaphoreGive(s_lock);return ESP_ERR_INVALID_STATE;}
  s_state.query=false; s_state.resumed=false;
  memset(s_state.results,0,sizeof(s_state.results));
  s_state.busy=true; s_state.id++; s_state.value=value;
  snprintf(s_state.parameter,sizeof(s_state.parameter),"%s",name);
  s_state.phase="working"; s_state.error=NULL;
  s_state.written=false; s_state.restarted=false;
  xSemaphoreGive(s_lock);
  xTaskNotifyGive(s_task);
  return ESP_OK;
}
esp_err_t radar_at_query_all(void) {
  xSemaphoreTake(s_lock,portMAX_DELAY);
  if(s_state.busy) {xSemaphoreGive(s_lock);return ESP_ERR_INVALID_STATE;}
  s_state.query=true; s_state.resumed=false;
  s_state.busy=true; s_state.id++; s_state.value=0;
  s_state.parameter[0]=0; s_state.phase="working"; s_state.error=NULL;
  s_state.written=false; s_state.restarted=false;
  memset(s_state.values,0,sizeof(s_state.values));
  memset(s_state.results,0,sizeof(s_state.results));
  xSemaphoreGive(s_lock);
  xTaskNotifyGive(s_task);
  return ESP_OK;
}
esp_err_t radar_at_init(void) {
  s_lock=xSemaphoreCreateMutex();
  if(!s_lock) return ESP_ERR_NO_MEM;
  nvs_handle_t nvs;
  if(nvs_open("radar",NVS_READONLY,&nvs)==ESP_OK) {
    uint32_t baud;
    if(nvs_get_u32(nvs,"baud",&baud)==ESP_OK)
      for(unsigned i=1;i<9;i++) if(bauds[i]==baud) s_state.baud=baud;
    nvs_close(nvs);
  }
  uart_config_t cfg={.baud_rate=s_state.baud,.data_bits=UART_DATA_8_BITS,
    .parity=UART_PARITY_DISABLE,.stop_bits=UART_STOP_BITS_1,
    .flow_ctrl=UART_HW_FLOWCTRL_DISABLE,.source_clk=UART_SCLK_DEFAULT};
  esp_err_t err=uart_param_config(UART_PORT,&cfg);
  if(err==ESP_OK) err=uart_set_pin(UART_PORT,CFG_RADAR_UART_TX_PIN,CFG_RADAR_UART_RX_PIN,
                                  UART_PIN_NO_CHANGE,UART_PIN_NO_CHANGE);
  if(err==ESP_OK) err=uart_driver_install(UART_PORT,2048,0,0,NULL,0);
  if(err!=ESP_OK) return err;
  return xTaskCreate(worker,"radar-at",4096,NULL,1,&s_task)==pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
