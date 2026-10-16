"""Run the production driver against a fake INA226 register map (no hardware).
Run: python3 -m unittest discover -s tests -v
"""
import pathlib
import re
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class Ina226Tests(unittest.TestCase):
    def test_overcurrent_registers_units_validation_and_bus_errors(self):
        source = (ROOT / 'main/ina226.c').read_text()
        source = re.sub(r'^#include[^\n]*\n', '', source, flags=re.M)
        program = r'''
#include <stdint.h>
#include <stddef.h>
#include <math.h>
#include <assert.h>
typedef int esp_err_t;
typedef void *i2c_master_dev_handle_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_INVALID_RESPONSE 2
#define ESP_LOGE(...) ((void)TAG)
#define ESP_LOGI(...) ((void)TAG)
#define INA226_AVG_512 6
static uint16_t registers[8];
static unsigned writes,write_reg_log[16],write_value_log[16];
static int fail_register=-1;
static esp_err_t i2c_master_transmit(void *dev,const uint8_t *bytes,size_t size,int timeout){
assert(dev&&size==3);assert(bytes[0]<=7); /* Reject the original invalid map. */
assert(writes<16);write_reg_log[writes]=bytes[0];
write_value_log[writes++]=(bytes[1]<<8)|bytes[2];
if(bytes[0]==fail_register)return ESP_ERR_INVALID_RESPONSE;
registers[bytes[0]]=(bytes[1]<<8)|bytes[2];return ESP_OK;
}
static esp_err_t i2c_master_transmit_receive(void *dev,const uint8_t *reg,size_t wn,uint8_t *bytes,size_t rn,int timeout){
assert(dev&&wn==1&&rn==2&&*reg<=7);bytes[0]=registers[*reg]>>8;bytes[1]=registers[*reg]&255;return ESP_OK;
}
''' + source + r'''
int main(void){
assert(ina226_set_ocp_threshold(2.5f)==ESP_ERR_INVALID_ARG&&writes==0);
assert(ina226_init((void*)1,10.0f,0.005f)==ESP_OK);
uint16_t cal=registers[5],config=registers[0];writes=0;
assert(ina226_set_ocp_threshold(2.5f)==ESP_OK);
assert(writes==2&&write_reg_log[0]==7&&write_reg_log[1]==6);
assert(registers[7]==5000&&registers[6]==0x8000);
assert(registers[5]==cal&&registers[0]==config);
/* Threshold depends on actual shunt, not max-current calibration. */
writes=0;assert(ina226_init((void*)1,8.0f,0.005f)==ESP_OK);writes=0;
assert(ina226_set_ocp_threshold(2.5f)==ESP_OK&&registers[7]==5000);
writes=0;assert(ina226_init((void*)1,10.0f,0.002f)==ESP_OK);writes=0;
assert(ina226_set_ocp_threshold(4.5f)==ESP_OK&&registers[7]==3600);
/* Invalid thresholds must not touch any chip register. */
const float invalid[]={0.0f,-1.0f,NAN,INFINITY,1000.0f,0.000001f};
for(unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);i++){
writes=0;assert(ina226_set_ocp_threshold(invalid[i])==ESP_ERR_INVALID_ARG&&writes==0);
}
fail_register=7;writes=0;
assert(ina226_set_ocp_threshold(4.5f)==ESP_ERR_INVALID_RESPONSE&&writes==1);
fail_register=6;writes=0;
assert(ina226_set_ocp_threshold(4.5f)==ESP_ERR_INVALID_RESPONSE&&writes==2);
/* A failed reinitialization invalidates the shunt used for alerts. */
fail_register=5;writes=0;assert(ina226_init((void*)1,10,0.005f)==ESP_ERR_INVALID_RESPONSE);
writes=0;assert(ina226_set_ocp_threshold(2.5f)==ESP_ERR_INVALID_ARG&&writes==0);
return 0;}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory)
            (path / 'test.c').write_text(program)
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Wno-unused-parameter',
                            str(path / 'test.c'), '-lm', '-o', str(path / 'test')], check=True)
            subprocess.run([str(path / 'test')], check=True)


if __name__ == '__main__':
    unittest.main()
