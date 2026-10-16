"""mDNS lifecycle: PWR startup, ECO cleanup and failure rollback."""
import re
import unittest
from test_runtime import ROOT, run_c


class MdnsTests(unittest.TestCase):
    def test_lifecycle_and_partial_failure_cleanup(self):
        code = (ROOT / 'main/mdns_service.c').read_text()
        code = re.sub(r'^#include[^\n]*\n', '', code, flags=re.M)
        run_c(r'''
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <stddef.h>
typedef int esp_err_t;
typedef struct {const char *key,*value;} mdns_txt_item_t;
#define ESP_OK 0
#define ESP_LOGI(...) ((void)0)
#define CFG_HOSTNAME "power-test"
static int fail_at,step,live,starts,frees,services;
static int mdns_init(void){starts++;step=1;if(fail_at==step)return 9;assert(!live);live=1;return 0;}
static int mdns_hostname_set(const char*host){assert(live&&!strcmp(host,CFG_HOSTNAME));return fail_at==++step?9:0;}
static int mdns_instance_name_set(const char*name){assert(live&&!strcmp(name,CFG_HOSTNAME));return fail_at==++step?9:0;}
static int mdns_service_add(const char *instance,const char *service,const char *proto,int port,const mdns_txt_item_t *txt,int count){
assert(live&&!instance&&!strcmp(service,"_http")&&!strcmp(proto,"_tcp")&&port==80&&count==1);
assert(!strcmp(txt[0].key,"path")&&!strcmp(txt[0].value,"/"));services++;return fail_at==++step?9:0;}
static void mdns_free(void){assert(live);live=0;frees++;}
''' + code + r'''
int main(void){
assert(network_mdns_set_enabled(false)==0&&!starts&&!frees);
for(int failure=1;failure<=4;failure++){
fail_at=failure;assert(network_mdns_set_enabled(true)==9&&!live&&!s_started);
fail_at=0;assert(network_mdns_set_enabled(true)==0&&live&&s_started);
int old=starts;assert(network_mdns_set_enabled(true)==0&&starts==old);
assert(network_mdns_set_enabled(false)==0&&!live&&!s_started);
old=frees;assert(network_mdns_set_enabled(false)==0&&frees==old);
}
assert(services>0);return 0;}
''')
