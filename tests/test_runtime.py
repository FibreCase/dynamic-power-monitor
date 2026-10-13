"""Behavioral checks of production C routines using small hardware/RTOS fakes.
Run with: python3 -m unittest discover -s tests -v
"""
import pathlib
import re
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


def routine(file, start, end):
    text = (ROOT / file).read_text()
    return text[text.index(start):text.index(end, text.index(start))]


def run_c(source):
    with tempfile.TemporaryDirectory() as directory:
        path = pathlib.Path(directory)
        (path / 'check.c').write_text(source)
        subprocess.run(['cc', '-std=c11', str(path / 'check.c'), '-lm', '-o', str(path / 'check')], check=True)
        subprocess.run([str(path / 'check')], check=True)


class RuntimeTests(unittest.TestCase):
    def test_oled_unchanged_and_failed_frame_retry(self):
        code = routine('main/display.c', 'static void display_send_if_changed', 'void display_init')
        run_c('''
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
static int s_u8g2, sends;
static bool failed;
static uint8_t buffer[1024];
static uint8_t *u8g2_GetBufferPtr(void *p){return buffer;}
static void u8g2_esp_hal_frame_begin(void){}
static bool u8g2_esp_hal_frame_ok(void){return !failed;}
static void u8g2_SendBuffer(void *p){sends++;}
''' + code + '''
int main(void){
display_send_if_changed();assert(sends==1);
for(int i=0;i<100;i++)display_send_if_changed();assert(sends==1);
buffer[17]=1;failed=true;display_send_if_changed();assert(sends==2);
buffer[17]=0;failed=false;display_send_if_changed();assert(sends==3);
display_send_if_changed();assert(sends==3);
buffer[17]=1;display_send_if_changed();assert(sends==4);
buffer[17]=2;failed=true;display_send_if_changed();assert(sends==5);
failed=false;display_send_if_changed();assert(sends==6);
display_send_if_changed();assert(sends==6);
return 0;}
''')

    def test_pwm_modes_use_coherent_power_snapshot(self):
        code = routine('main/status.c', 'static void nmos_set_outputs', 'void status_init_gpio')
        run_c('''
#include <stdbool.h>
#include <stdatomic.h>
#include <assert.h>
enum nmos_mode {NMOS_RA,NMOS_ON,NMOS_BL,NMOS_OFF};
struct pwm_settings{unsigned ctl1,ctl2;};
#define portMAX_DELAY 0
#define xSemaphoreTake(...) ((void)0)
#define xSemaphoreGive(...) ((void)0)
static bool s_power_saving;static atomic_int s_nmos_mode;
static unsigned output[2];
static void pwm_settings_get(struct pwm_settings *out){*out=(struct pwm_settings){35,65};}
static void pwm_set(unsigned n,unsigned duty){output[n]=duty;}
''' + code + '''
int main(void){
for(int saving=0;saving<2;saving++)for(int presence=0;presence<2;presence++){
s_power_saving=saving;
atomic_store(&s_nmos_mode,NMOS_RA);nmos_set_outputs(presence);
assert(output[0]==(presence?(saving?100:35):0));assert(output[1]==(!saving&&presence?65:0));
atomic_store(&s_nmos_mode,NMOS_BL);nmos_set_outputs(presence);
assert(output[0]==(presence?(saving?100:35):0));assert(output[1]==0);
atomic_store(&s_nmos_mode,NMOS_ON);nmos_set_outputs(presence);assert(output[0]==100&&output[1]==100);
atomic_store(&s_nmos_mode,NMOS_OFF);nmos_set_outputs(presence);assert(output[0]==0&&output[1]==0);
}return 0;}
''')

    def test_nvs_commit_does_not_hold_snapshot_lock(self):
        code = (ROOT / 'main/pwm_settings.c').read_text()
        code = re.sub(r'^#include[^\n]*\n', '', code, flags=re.M)
        run_c('''
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
typedef int esp_err_t;typedef int nvs_handle_t;typedef int SemaphoreHandle_t;
struct pwm_settings{uint8_t ctl1,ctl2;};
#define CFG_CTL1_PWM_DUTY_PCT 100
#define CFG_CTL2_PWM_DUTY_PCT 100
#define ESP_OK 0
#define ESP_ERR_NO_MEM 1
#define ESP_ERR_INVALID_ARG 2
#define ESP_ERR_INVALID_STATE 3
#define ESP_ERR_NVS_NOT_FOUND 4
#define NVS_READONLY 0
#define NVS_READWRITE 1
#define portMAX_DELAY 0
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
static int s_mutex=3, allocated, held[4], commits, fail_commit;
static bool s_power_saving,s_boot_loading,stored;
static uint8_t disk[3],pending[3];
void pwm_settings_get(struct pwm_settings *out);
static int xSemaphoreCreateMutex(void){return ++allocated;}
static void xSemaphoreTake(int h,int t){assert(!held[h]);held[h]=1;}
static void xSemaphoreGive(int h){assert(held[h]);held[h]=0;}
static int nvs_open(const char *n,int mode,int *h){*h=1;return mode==NVS_READONLY&&!stored?4:0;}
static void nvs_close(int h){}
static int nvs_get_blob(int h,const char *k,void *p,size_t *n){memcpy(p,disk,3);*n=3;return 0;}
static int nvs_set_blob(int h,const char *k,const void *p,size_t n){assert(n==3&&!held[1]);memcpy(pending,p,3);return 0;}
static int nvs_commit(int h){
assert(!held[1]);struct pwm_settings old;pwm_settings_get(&old);assert(old.ctl1==100);
commits++;if(fail_commit)return 9;memcpy(disk,pending,3);stored=true;return 0;}
''' + code + '''
int main(void){
struct pwm_settings r;
assert(pwm_settings_init()==0);
fail_commit=1;assert(pwm_settings_save(35,65)==9);pwm_settings_get(&r);assert(r.ctl1==100);
fail_commit=0;assert(pwm_settings_save(35,65)==0);pwm_settings_get(&r);assert(r.ctl1==35&&r.ctl2==65);
assert(pwm_settings_save(35,65)==0&&commits==2);
assert(disk[0]==1&&disk[1]==35&&disk[2]==65);
s_settings=(struct pwm_settings){0,0};allocated=0;assert(pwm_settings_init()==0);pwm_settings_get(&r);assert(r.ctl1==35&&r.ctl2==65);
s_power_saving=true;assert(pwm_settings_save(20,30)==ESP_ERR_INVALID_STATE);
assert(pwm_settings_save(101,30)==ESP_ERR_INVALID_ARG);
return 0;}
''')

    def test_sampling_deadlines_and_two_second_boot_gate(self):
        code = routine('main/sample_task.c', 'static uint32_t s_monitor_start', 'void sample_task_create')
        run_c('''
#include <stdbool.h>
#include <stdint.h>
#include <assert.h>
#include <setjmp.h>
typedef uint32_t TickType_t;
#define NORMAL_INTERVAL_MS 100
#define SLOW_INTERVAL_MS 1000
#define CFG_EXTERNAL_POWER_MIN_V 4.5f
#define ESP_OK 0
#define pdTRUE 1
#define pdFALSE 0
#define portMAX_DELAY 0
#define pdMS_TO_TICKS(x) (x)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define xSemaphoreTake(...) ((void)0)
#define xSemaphoreGive(...) ((void)0)
static uint32_t clock_ms, s_interval_ms=1000;static int waits, resets,notifies,radio_calls;
static bool s_power_saving=true,s_boot_loading=true;
static jmp_buf done;
struct reading{float v,i,p,t;int64_t ts;};
static TickType_t xTaskGetTickCount(void){return clock_ms;}
static uint32_t now_ms(void){return clock_ms;}
static void vTaskDelay(unsigned ticks){clock_ms+=ticks;}
static int xTaskDelayUntil(TickType_t *last,unsigned interval){
if(waits++==14)longjmp(done,1);assert(interval==100||interval==1000);*last+=interval;clock_ms=*last;return pdTRUE;}
static void wifi_set_enabled(bool enabled){assert(clock_ms>=2000);assert(enabled);radio_calls++;}
static bool wifi_is_ready(void){return clock_ms>=3000&&clock_ms<4000;}
static void set_sampling_interval(unsigned ms){s_interval_ms=ms;}
static void nmos_control_reset(void){resets++;}
static void http_api_notify_power_change(void){notifies++;}
static void environment_sensor_notify_power_change(void){}
static int ina226_read_voltage(float *v){*v=5;return 0;}
static float ina226_get_current_ma(void){return 100;}
static float ina226_get_power_mw(void){return 500;}
static float temp_sensor_read_celsius(void){return 25;}
static int64_t get_epoch_ms(void){return clock_ms;}
static void reading_publish(struct reading *r){if(clock_ms<2000)assert(s_boot_loading);}
''' + code + '''
int main(void){power_mode_monitor_start();if(!setjmp(done))sample_task(0);
assert(resets==2&&notifies==2);assert(!s_boot_loading&&s_power_saving);assert(radio_calls>0);
assert(clock_ms==5000);return 0;}
''')

    def test_wifi_retries_and_sleeps_when_stable(self):
        code = routine('main/wifi_ntp.c', '  bool started = false', '\nesp_err_t wifi_background_start')
        run_c('''
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <assert.h>
#include <setjmp.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define pdTRUE 1
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(x) (x)
static atomic_bool s_enabled=true,s_connect_pending,s_reassociate;
static uint32_t clock_ms;static int starts,stops,connects,disconnects,waits;static jmp_buf done;
static uint32_t now_ms(void){return clock_ms;}
static int esp_wifi_start(void){starts++;atomic_store(&s_connect_pending,true);return 0;}
static int esp_wifi_stop(void){return ++stops==1?9:0;}
static int esp_wifi_connect(void){return ++connects==1?9:0;}
static int esp_wifi_disconnect(void){disconnects++;return 0;}
static void ulTaskNotifyTake(int clear,uint32_t delay){
switch(++waits){
case 1:assert(delay==1000);clock_ms+=1000;break;
case 2:assert(delay==portMAX_DELAY);atomic_store(&s_enabled,false);break;
case 3:assert(delay==1000);clock_ms+=1000;break;
case 4:assert(delay==portMAX_DELAY);atomic_store(&s_enabled,true);atomic_store(&s_reassociate,true);clock_ms+=1000;break;
case 5:assert(delay==portMAX_DELAY);atomic_store(&s_reassociate,true);clock_ms+=1000;break;
case 6:assert(delay==portMAX_DELAY);longjmp(done,1);
default:assert(0);}}
static void run_worker(void){
''' + code + '''
int main(void){if(!setjmp(done))run_worker();assert(starts==2&&stops==2&&connects==4&&disconnects==1);return 0;}
''')

    def test_http_lifecycle_waits_and_retries(self):
        code = routine('main/http_api.c', 'static void http_control_task', 'esp_err_t http_api_control_start')
        run_c('''
#include <stdbool.h>
#include <stdint.h>
#include <assert.h>
#include <setjmp.h>
#include <stddef.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define xSemaphoreTake(...) ((void)0)
#define xSemaphoreGive(...) ((void)0)
#define pdTRUE 1
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(x) (x)
static bool s_power_saving=true,s_boot_loading=true;static void *s_server;
static int starts,stops,waits;static jmp_buf done;
static int http_server_start(void){if(++starts==2)return 9;s_server=(void*)1;return 0;}
static int httpd_stop(void *h){assert(h);return ++stops==1?9:0;}
static void ulTaskNotifyTake(int clear,uint32_t delay){
switch(++waits){
case 1:assert(delay==portMAX_DELAY&&!s_server);s_boot_loading=false;s_power_saving=false;break;
case 2:assert(delay==portMAX_DELAY&&s_server);s_power_saving=true;break;
case 3:assert(delay==1000&&s_server);break;
case 4:assert(delay==portMAX_DELAY&&!s_server);s_power_saving=false;break;
case 5:assert(delay==1000&&!s_server);break;
case 6:assert(delay==portMAX_DELAY&&s_server);s_power_saving=true;break;
case 7:assert(delay==portMAX_DELAY&&!s_server);longjmp(done,1);
default:assert(0);}}
''' + code + '''
int main(void){if(!setjmp(done))http_control_task(0);assert(starts==3&&stops==3);return 0;}
''')

    def test_oled_transport_bounds_and_failure_short_circuit(self):
        code = routine('main/u8g2_esp_hal.c', 'static uint8_t u8x8_byte_esp_i2c', '/* GPIO/delay shim')
        run_c('''
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <stddef.h>
typedef int u8x8_t;typedef int esp_err_t;
enum {U8X8_MSG_BYTE_SEND,U8X8_MSG_BYTE_INIT,U8X8_MSG_BYTE_SET_DC,U8X8_MSG_BYTE_START_TRANSFER,U8X8_MSG_BYTE_END_TRANSFER};
#define ESP_OK 0
#define ESP_LOGE(...) ((void)0)
#define portTICK_PERIOD_MS 1
static bool s_frame_failed;static uint32_t s_last_error_log;
static int s_oled_dev, writes, error;
static uint32_t xTaskGetTickCount(void){return 10000;}
static int i2c_master_transmit(int h,const uint8_t *p,unsigned n,int t){assert(n<=32);writes++;return error;}
''' + code + '''
int main(void){
uint8_t data[33]={0};
u8x8_byte_esp_i2c(0,U8X8_MSG_BYTE_START_TRANSFER,0,0);
assert(u8x8_byte_esp_i2c(0,U8X8_MSG_BYTE_SEND,4,data)==1);
error=9;assert(u8x8_byte_esp_i2c(0,U8X8_MSG_BYTE_END_TRANSFER,0,0)==0);assert(s_frame_failed&&writes==1);
u8x8_byte_esp_i2c(0,U8X8_MSG_BYTE_START_TRANSFER,0,0);
u8x8_byte_esp_i2c(0,U8X8_MSG_BYTE_SEND,4,data);u8x8_byte_esp_i2c(0,U8X8_MSG_BYTE_END_TRANSFER,0,0);assert(writes==1);
s_frame_failed=false;u8x8_byte_esp_i2c(0,U8X8_MSG_BYTE_START_TRANSFER,0,0);
assert(u8x8_byte_esp_i2c(0,U8X8_MSG_BYTE_SEND,33,data)==0);assert(s_frame_failed&&writes==1);
s_frame_failed=false;error=0;u8x8_byte_esp_i2c(0,U8X8_MSG_BYTE_START_TRANSFER,0,0);
u8x8_byte_esp_i2c(0,U8X8_MSG_BYTE_SEND,4,data);assert(u8x8_byte_esp_i2c(0,U8X8_MSG_BYTE_END_TRANSFER,0,0)==1);assert(writes==2);
return 0;}
''')


if __name__ == '__main__':
    unittest.main()
