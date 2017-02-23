#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <math.h>
#include "common/maths.h"
#include <platform.h>
#include "build/debug.h"
#include "fc/fc_tasks.h"
#include "config/parameter_group.h"
#include "drivers/dma.h"
#include "drivers/system.h"
#include "drivers/serial.h"
#include "drivers/serial_uart.h"
#include "io/serial.h"
#include "common/crc.h"
#include "rx/rx.h"
#include "app.h"


uint8_t App_data[APP_DATA_LENGTH];
bool App_data_ok = false;

static void App_DataReceive(uint16_t data);

void wifi_uart_init(void)
{
	openSerialPort(SERIAL_PORT_UART2,FUNCTION_TELEMETRY_MAVLINK, App_DataReceive, 19200, MODE_RX, SERIAL_NOT_INVERTED);//SERIAL_STOPBITS_1
}


void App_DataReceive(uint16_t data)
{
	static uint8_t count, i, n, buffer[APP_DATA_LENGTH];

	switch(count)
	{
   		case 0: if(data == 0x66) count = 1;
				break;

 		case 1: if(i < 4) buffer[i++] = data;
				else {i = 0;count = 2;}
				break;

		case 2: if(data == ((buffer[0]^buffer[1]^buffer[2]^buffer[3]^buffer[4])&0xff)) {
					for(uint8_t j = 0;j<5;j++)	App_data[j] = buffer[j];
					n++;
				}else n--;
				count = 0;
				break;
 	
		default:count = 0;i = 0;break;
	}

	if(n > 60)//大概3s的样子 
		App_data_ok = true;
		else App_data_ok = false;
}