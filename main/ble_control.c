/* Small read/write GATT service. Only cached state is accessed in host context;
 * no I2C, NVS writes, UART waits, or changes to WiFi/power policy. */
#include "ble_control.h"
#include "config.h"
#include "status.h"
#include "environment_sensor.h"
#include <stdio.h>
#include <stdatomic.h>
#include <string.h>
#include "esp_log.h"
#include "nimble/nimble_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#if !defined(CONFIG_BT_CTRL_MODEM_SLEEP) || !defined(CONFIG_BT_CTRL_LPCLK_SEL_MAIN_XTAL)
#error "BLE ECO requires modem sleep with main XTAL; update sdkconfig using sdkconfig.defaults"
#endif
#if !defined(CONFIG_ESP_COEX_SW_COEXIST_ENABLE)
#error "PWR WiFi/BLE requires software coexistence"
#endif

/* UUIDs: 5e7a000{1,2,3}-8e6d-4b28-a5c1-9f0260d84e31
 * service, state (READ), mode (WRITE with response). */
#define CONTROL_UUID(n) BLE_UUID128_INIT(0x31,0x4e,0xd8,0x60,0x02,0x9f,0xc1,0xa5,0x28,0x4b,0x6d,0x8e,n,0x00,0x7a,0x5e)
static const ble_uuid128_t service_uuid=CONTROL_UUID(1);
static const ble_uuid128_t state_uuid=CONTROL_UUID(2);
static const ble_uuid128_t mode_uuid=CONTROL_UUID(3);
static uint8_t own_address;
static char device_name[27];
static struct ble_npl_callout adv_retry, policy_retry;
static struct ble_npl_event policy_event;
static atomic_bool started;
/* These fields belong exclusively to the NimBLE host task. */
static bool synced;
static uint16_t connection_handle=BLE_HS_CONN_HANDLE_NONE;
static unsigned policy_attempts;

static bool saving_mode(void) {
  enum nmos_mode mode;bool saving,loading;
  nmos_control_snapshot(&mode,&saving,&loading);
  return saving;
}

static void put_float(uint8_t *out, float value) {
  uint32_t bits;memcpy(&bits,&value,4);
  for (unsigned i=0;i<4;i++) out[i]=(uint8_t)(bits>>(i*8));
}
static void state_packet(uint8_t out[28]) {
  struct environment_reading r;
  environment_sensor_get(&r);
  enum nmos_mode mode;bool saving,loading;
  nmos_control_snapshot(&mode,&saving,&loading);
  memset(out,0,28);
  out[0]=1; /* protocol version */
  out[1]=(loading?1:0)|(saving?2:0)|(r.available?4:0)|(r.error?8:0);
  out[2]=(uint8_t)mode;out[3]=r.validity&3;
  put_float(out+4,r.temperature_c);put_float(out+8,r.humidity_rh_pct);
  put_float(out+12,r.compensation_temperature_c);put_float(out+16,r.compensation_humidity_rh_pct);
  out[20]=r.eco2_ppm;out[21]=r.eco2_ppm>>8;
  out[22]=r.tvoc_ppb;out[23]=r.tvoc_ppb>>8;
  out[24]=r.aqi;out[25]=r.new_data;
}
static int access_control(uint16_t connection, uint16_t attribute,
                          struct ble_gatt_access_ctxt *ctx, void *arg) {
  (void)connection;(void)attribute;(void)arg;
  if (ctx->op==BLE_GATT_ACCESS_OP_READ_CHR) {
    uint8_t packet[28];state_packet(packet);
    return os_mbuf_append(ctx->om,packet,sizeof(packet))==0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
  }
  if (ctx->op==BLE_GATT_ACCESS_OP_WRITE_CHR) {
    uint8_t mode;
    if (OS_MBUF_PKTLEN(ctx->om)!=1) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    if (ble_hs_mbuf_to_flat(ctx->om,&mode,1,NULL)!=0) return BLE_ATT_ERR_UNLIKELY;
    enum nmos_mode requested=(enum nmos_mode)mode;
    esp_err_t err=nmos_control_change(NMOS_CHANGE_SET,&requested,false);
    if (err==ESP_ERR_INVALID_ARG) return BLE_ATT_ERR_VALUE_NOT_ALLOWED;
    if (err!=ESP_OK) return 0x80; /* loading, or BL requested in ECO */
    return 0;
  }
  return BLE_ATT_ERR_UNLIKELY;
}
static const struct ble_gatt_svc_def services[]={
  {.type=BLE_GATT_SVC_TYPE_PRIMARY,.uuid=&service_uuid.u,
   .characteristics=(struct ble_gatt_chr_def[]){
     {.uuid=&state_uuid.u,.access_cb=access_control,.flags=BLE_GATT_CHR_F_READ},
     {.uuid=&mode_uuid.u,.access_cb=access_control,.flags=BLE_GATT_CHR_F_WRITE},
     {0}}},
  {0}
};
static int gap_event(struct ble_gap_event *event, void *arg);
static void advertise(struct ble_npl_event *event) {
  (void)event;
  if (!synced || connection_handle!=BLE_HS_CONN_HANDLE_NONE || ble_gap_adv_active()) return;
  struct ble_hs_adv_fields fields={0};
  fields.flags=BLE_HS_ADV_F_DISC_GEN|BLE_HS_ADV_F_BREDR_UNSUP;
  fields.uuids128=(ble_uuid128_t *)&service_uuid;fields.num_uuids128=1;fields.uuids128_is_complete=1;
  int rc=ble_gap_adv_set_fields(&fields);
  struct ble_hs_adv_fields response={0};
  response.name=(uint8_t *)device_name;response.name_len=strlen(device_name);response.name_is_complete=1;
  if (!rc) rc=ble_gap_adv_rsp_set_fields(&response);
  bool saving=saving_mode();
  struct ble_gap_adv_params params={.conn_mode=BLE_GAP_CONN_MODE_UND,.disc_mode=BLE_GAP_DISC_MODE_GEN,
                                     .itvl_min=saving?3200:1600,.itvl_max=saving?3840:1920}; /* ECO 2–2.4s, PWR 1–1.2s */
  if (!rc) rc=ble_gap_adv_start(own_address,NULL,BLE_HS_FOREVER,&params,gap_event,NULL);
  if (rc) {
    ESP_LOGW("ble","advertising retry: %d",rc);
    ble_npl_callout_reset(&adv_retry,ble_npl_time_ms_to_ticks32(1000));
  }
}
static void connection_policy(struct ble_npl_event *event) {
  (void)event;
  if (!synced || connection_handle==BLE_HS_CONN_HANDLE_NONE) return;
  bool saving=saving_mode();
  struct ble_gap_upd_params params={.itvl_min=saving?80:40,.itvl_max=saving?160:80,
                                    .latency=3,.supervision_timeout=600};
  int rc=ble_gap_update_params(connection_handle,&params);
  /* A phone may keep its own parameters. Bound retries rather than create
   * permanent wakeups after a rejection or an overlapping procedure. */
  if (rc && ++policy_attempts<3)
    ble_npl_callout_reset(&policy_retry,ble_npl_time_ms_to_ticks32(1000));
}
static void power_policy(struct ble_npl_event *event) {
  (void)event;
  if (!synced) return;
  policy_attempts=0;
  ble_npl_callout_stop(&policy_retry);
  if (connection_handle!=BLE_HS_CONN_HANDLE_NONE) {
    connection_policy(NULL); /* Preserve the existing BLE connection. */
  } else {
    ble_npl_callout_stop(&adv_retry);
    if (ble_gap_adv_active()) ble_gap_adv_stop();
    advertise(NULL);
  }
}
void ble_control_notify_power_change(void) {
  if (atomic_load(&started)) ble_npl_eventq_put(nimble_port_get_dflt_eventq(),&policy_event);
}
static int gap_event(struct ble_gap_event *event, void *arg) {
  (void)arg;
  if (event->type==BLE_GAP_EVENT_CONNECT && event->connect.status==0) {
    connection_handle=event->connect.conn_handle;
    ble_npl_callout_stop(&adv_retry);
    policy_attempts=0;
    connection_policy(NULL);
  } else if (event->type==BLE_GAP_EVENT_DISCONNECT) {
    connection_handle=BLE_HS_CONN_HANDLE_NONE;
    ble_npl_callout_stop(&policy_retry);
    advertise(NULL);
  } else if (event->type==BLE_GAP_EVENT_ADV_COMPLETE ||
             (event->type==BLE_GAP_EVENT_CONNECT && event->connect.status!=0)) {
    advertise(NULL);
  }
  return 0;
}
static void reset_host(int reason) {
  synced=false;
  connection_handle=BLE_HS_CONN_HANDLE_NONE;
  ble_npl_callout_stop(&adv_retry);
  ble_npl_callout_stop(&policy_retry);
  ESP_LOGW("ble","host reset: %d",reason);
}
static void sync_host(void) {
  int rc=ble_hs_util_ensure_addr(0);
  if (!rc) rc=ble_hs_id_infer_auto(0,&own_address);
  if (!rc) {synced=true;advertise(NULL);}
  else ESP_LOGE("ble","address initialization failed: %d",rc);
}
static void host_task(void *arg) {
  (void)arg;nimble_port_run();vTaskDelete(NULL);
}
esp_err_t ble_control_start(void) {
  esp_err_t err=nimble_port_init();
  if (err!=ESP_OK) return err;
  snprintf(device_name,sizeof(device_name),"5V-%.23s",CFG_HOSTNAME);
  ble_svc_gap_init();ble_svc_gatt_init();
  int rc=ble_svc_gap_device_name_set(device_name);
  if (!rc) rc=ble_gatts_count_cfg(services);
  if (!rc) rc=ble_gatts_add_svcs(services);
  if (rc) {nimble_port_deinit();return ESP_FAIL;}
  ble_hs_cfg.sync_cb=sync_host;
  ble_hs_cfg.reset_cb=reset_host;
  ble_npl_callout_init(&adv_retry,nimble_port_get_dflt_eventq(),advertise,NULL);
  ble_npl_callout_init(&policy_retry,nimble_port_get_dflt_eventq(),connection_policy,NULL);
  ble_npl_event_init(&policy_event,power_policy,NULL);
  if (xTaskCreate(host_task,"ble-control",4096,NULL,2,NULL)!=pdPASS) {
    nimble_port_deinit();
    return ESP_ERR_NO_MEM;
  }
  atomic_store(&started,true);
  return ESP_OK;
}
