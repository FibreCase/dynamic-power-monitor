"""BLE wire format and shared PWR/ECO control policy, without hardware."""
import unittest
from test_runtime import routine, run_c


class BleTests(unittest.TestCase):
    def test_state_packet(self):
        code = routine('main/ble_control.c', 'static void put_float', 'static int access_control')
        run_c('''
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
enum nmos_mode {NMOS_RA,NMOS_ON,NMOS_BL,NMOS_OFF};
enum nmos_change {NMOS_CHANGE_SET,NMOS_CHANGE_API_NEXT,NMOS_CHANGE_BUTTON_NEXT};
struct environment_reading {bool available;float temperature_c,humidity_rh_pct,compensation_temperature_c,compensation_humidity_rh_pct;uint16_t eco2_ppm,tvoc_ppb;uint8_t aqi,validity;bool new_data;const char *error;};
static struct environment_reading reading={true,25.5,43.25,27.5,40.5,1234,567,3,1,true,0};
static bool saving,loading;
static enum nmos_mode mode=NMOS_BL;
static void environment_sensor_get(struct environment_reading*out){*out=reading;}
static void nmos_control_snapshot(enum nmos_mode*m,bool*s,bool*l){*m=mode;*s=saving;*l=loading;}
''' + code + '''
int main(void){uint8_t bytes[28];float temp;
state_packet(bytes);assert(bytes[0]==1&&bytes[1]==4&&bytes[2]==NMOS_BL&&bytes[3]==1);
assert(bytes[20]==0xd2&&bytes[21]==4&&bytes[22]==0x37&&bytes[23]==2&&bytes[24]==3&&bytes[25]==1);
memcpy(&temp,bytes+4,4);assert(temp==25.5);memcpy(&temp,bytes+16,4);assert(temp==40.5);
loading=saving=true;mode=NMOS_RA;reading.error="failed";state_packet(bytes);assert(bytes[1]==15&&bytes[2]==0);
reading.available=false;state_packet(bytes);assert(bytes[1]==11);
return 0;}
''')

    def test_eco_control_policy_and_http_guard(self):
        code = routine('main/status.c', 'void nmos_control_snapshot', 'bool environment_ui_selected')
        run_c('''
#include <stdbool.h>
#include <stdatomic.h>
#include <assert.h>
typedef int esp_err_t;
enum nmos_mode {NMOS_RA,NMOS_ON,NMOS_BL,NMOS_OFF};
enum nmos_change {NMOS_CHANGE_SET,NMOS_CHANGE_API_NEXT,NMOS_CHANGE_BUTTON_NEXT};
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_INVALID_STATE 2
#define xSemaphoreTake(...) ((void)0)
#define xSemaphoreGive(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
static bool s_power_saving,s_boot_loading;
static atomic_int s_nmos_mode;
''' + code + '''
int main(void){
for(int saving=0;saving<2;saving++)for(int loading=0;loading<2;loading++){
s_power_saving=saving;s_boot_loading=loading;
for(int m=NMOS_RA;m<=NMOS_OFF;m++){
atomic_store(&s_nmos_mode,NMOS_RA);
enum nmos_mode requested=m;
int result=nmos_control_change(NMOS_CHANGE_SET,&requested,false);
assert(result==((loading||(saving&&m==NMOS_BL))?ESP_ERR_INVALID_STATE:ESP_OK));
if(result==ESP_OK)assert(atomic_load(&s_nmos_mode)==m);
requested=m;assert(nmos_control_change(NMOS_CHANGE_SET,&requested,true)==((saving||loading)?ESP_ERR_INVALID_STATE:ESP_OK));
}
}
enum nmos_mode invalid=99;
assert(nmos_control_change(NMOS_CHANGE_SET,&invalid,false)==ESP_ERR_INVALID_ARG);
enum nmos_mode m;bool saving,loading;
nmos_control_snapshot(&m,&saving,&loading);assert(saving&&loading);
/* HTTP and button cycles share the same locked reducer and boot guard. */
s_boot_loading=false;s_power_saving=false;atomic_store(&s_nmos_mode,NMOS_RA);
m=NMOS_RA;
assert(nmos_control_change(NMOS_CHANGE_API_NEXT,&m,true)==ESP_OK&&m==NMOS_BL);
assert(nmos_control_change(NMOS_CHANGE_API_NEXT,&m,true)==ESP_OK&&m==NMOS_RA);
for(int i=0;i<4;i++){enum nmos_mode want[]={NMOS_ON,NMOS_BL,NMOS_OFF,NMOS_RA};assert(nmos_control_change(NMOS_CHANGE_BUTTON_NEXT,&m,false)==ESP_OK&&m==want[i]);}
s_power_saving=true;
for(int i=0;i<3;i++){enum nmos_mode want[]={NMOS_ON,NMOS_OFF,NMOS_RA};assert(nmos_control_change(NMOS_CHANGE_BUTTON_NEXT,&m,false)==ESP_OK&&m==want[i]);}
s_boot_loading=true;m=NMOS_ON;
assert(nmos_control_change(NMOS_CHANGE_BUTTON_NEXT,&m,false)==ESP_ERR_INVALID_STATE);
assert(atomic_load(&s_nmos_mode)==NMOS_RA);
assert(nmos_control_change(NMOS_CHANGE_SET,0,false)==ESP_ERR_INVALID_ARG);
return 0;}
''')

    def test_ble_power_policy_preserves_connection_and_bounds_retries(self):
        code = routine('main/ble_control.c', 'static int gap_event(struct ble_gap_event *event, void *arg);', 'static void host_task')
        run_c(r"""
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdatomic.h>
#include <assert.h>
#define BLE_HS_CONN_HANDLE_NONE 65535
#define BLE_HS_ADV_F_DISC_GEN 1
#define BLE_HS_ADV_F_BREDR_UNSUP 2
#define BLE_GAP_CONN_MODE_UND 1
#define BLE_GAP_DISC_MODE_GEN 1
#define BLE_HS_FOREVER 0
#define BLE_GAP_EVENT_CONNECT 1
#define BLE_GAP_EVENT_DISCONNECT 2
#define BLE_GAP_EVENT_ADV_COMPLETE 3
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
typedef int ble_uuid128_t;
struct ble_npl_event {int unused;};
struct ble_npl_callout {int unused;};
struct ble_hs_adv_fields {unsigned flags;ble_uuid128_t *uuids128;unsigned num_uuids128,uuids128_is_complete;uint8_t *name;unsigned name_len,name_is_complete;};
struct ble_gap_adv_params {unsigned conn_mode,disc_mode,itvl_min,itvl_max;};
struct ble_gap_upd_params {unsigned itvl_min,itvl_max,latency,supervision_timeout;};
struct ble_gap_event {int type;struct {int status;uint16_t conn_handle;} connect;};
static bool synced,saving=true,advertising,fail_update;
static uint16_t connection_handle=BLE_HS_CONN_HANDLE_NONE;
static unsigned policy_attempts,adv_starts,adv_stops,updates,retries,queued;
static unsigned adv_min,conn_min;
static atomic_bool started=true;
static uint8_t own_address;
static const ble_uuid128_t service_uuid;
static char device_name[]="5V-test";
static struct ble_npl_event policy_event;
static struct ble_npl_callout adv_retry,policy_retry;
static bool saving_mode(void){return saving;}
static bool ble_gap_adv_active(void){return advertising;}
static int ble_gap_adv_set_fields(void *fields){return 0;}
static int ble_gap_adv_rsp_set_fields(void *fields){return 0;}
static int ble_gap_adv_start(int a,void*b,int c,struct ble_gap_adv_params*p,void*e,void*f){assert(p->itvl_min<=p->itvl_max);advertising=true;adv_min=p->itvl_min;adv_starts++;return 0;}
static int ble_gap_adv_stop(void){advertising=false;adv_stops++;return 0;}
static int ble_gap_update_params(uint16_t h,struct ble_gap_upd_params*p){assert(h==7);assert(p->latency==3);conn_min=p->itvl_min;updates++;return fail_update;}
static void ble_npl_callout_reset(void *callout,unsigned ticks){assert(ticks==1000);retries++;}
static void ble_npl_callout_stop(void *callout){}
static unsigned ble_npl_time_ms_to_ticks32(unsigned ticks){return ticks;}
static void* nimble_port_get_dflt_eventq(void){return 0;}
static void ble_npl_eventq_put(void *queue,void *event){queued++;}
static int ble_hs_util_ensure_addr(int a){return 0;}
static int ble_hs_id_infer_auto(int a,uint8_t *out){*out=0;return 0;}
""" + code + r"""
int main(void){
advertise(0);assert(!adv_starts);
sync_host();assert(synced&&adv_min==3200&&adv_starts==1);
/* GAP stops advertising before reporting a successful connection. */
advertising=false;struct ble_gap_event event={.type=BLE_GAP_EVENT_CONNECT,.connect={.status=0,.conn_handle=7}};
gap_event(&event,0);assert(connection_handle==7&&updates==1&&conn_min==80);
saving=false;ble_control_notify_power_change();assert(queued==1&&updates==1);
power_policy(0);assert(conn_min==40&&connection_handle==7&&adv_starts==1&&!adv_stops);
saving=true;power_policy(0);assert(conn_min==80&&connection_handle==7&&adv_starts==1);
fail_update=true;policy_attempts=0;retries=0;
for(int i=0;i<3;i++)connection_policy(0);assert(retries==2);
reset_host(9);assert(!synced&&connection_handle==BLE_HS_CONN_HANDLE_NONE);
advertise(0);assert(adv_starts==1);
sync_host();assert(adv_starts==2&&adv_min==3200);
saving=false;power_policy(0);assert(adv_stops==1&&adv_starts==3&&adv_min==1600);
return 0;}
""")
