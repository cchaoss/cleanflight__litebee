#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdint.h>
#include <platform.h>
#include "nrf2401.h"
#include "stdio.h"
#include "bus_spi.h"
#include "system.h"
#include "gpio.h"

uint8_t  TXData[32];
uint8_t  TX_ADDRESS[5]= {0x11,0xff,0xff,0xff,0xff};//tx_address

uint8_t  NRF24L01_RXDATA[RX_PLOAD_WIDTH];//rx_data
uint8_t  RX_ADDRESS[RX_ADR_WIDTH]= {0x11,0xff,0xff,0xff,0xff};//rx_address



uint16_t bound(uint16_t val,uint16_t max,uint16_t min){return val > max? max : val < min? min : val;}

bool NRF_Write_Reg(uint8_t reg, uint8_t data)
{
    SPI_CSN_L();
    spiTransferByte(SPI2, reg + 0x20);
    spiTransferByte(SPI2, data);
    SPI_CSN_H();

    return true;

}


bool NRF_Write_Buf(uint8_t reg, uint8_t *data, uint8_t length)
{
    SPI_CSN_L();
    spiTransferByte(SPI2, reg + 0x20);
    spiTransfer(SPI2, NULL, data, length);
    SPI_CSN_H();

    return true;
}

bool NRF_Read_Buf(uint8_t reg, uint8_t *data, uint8_t length)
{
    SPI_CSN_L();
    spiTransferByte(SPI2, reg); // read transaction
    spiTransfer(SPI2, data, NULL, length);
    SPI_CSN_H();

    return true;
}


/*************************************NRF24L01_Receive***************************************/
 
uint16_t Nrf_Irq(int8_t channel)
{
    uint8_t sta; 
	static uint16_t buf[4] = {1500,1500,1500,1000};

    NRF_Read_Buf(NRFRegSTATUS, &sta, 1);
    if(sta & (1<<RX_DR))
    {
        NRF_Read_Buf(RD_RX_PLOAD,NRF24L01_RXDATA,RX_PLOAD_WIDTH);// read receive payload from RX_FIFO buffer 

		if((NRF24L01_RXDATA[0] == '$')&&(NRF24L01_RXDATA[1] == 'M')&&(NRF24L01_RXDATA[2] == '<'))
	 	{
			 switch(NRF24L01_RXDATA[4])
			 {
				 case MSP_SET_4CON:					
						 buf[3]=NRF24L01_RXDATA[5] + (NRF24L01_RXDATA[6]<<8);//UdataBuf[6]<<8 | UdataBuf[5];
						 buf[2]=NRF24L01_RXDATA[7] + (NRF24L01_RXDATA[8]<<8);   //UdataBuf[8]<<8 | UdataBuf[7];
						 buf[1]=NRF24L01_RXDATA[9] + (NRF24L01_RXDATA[10]<<8);  //UdataBuf[10]<<8 | UdataBuf[9];
						 buf[0]=NRF24L01_RXDATA[11]+ (NRF24L01_RXDATA[12]<<8); //UdataBuf[12]<<8 | UdataBuf[11];

						 for(uint8_t i =0;i<4;i++)	buf[i] = bound(buf[i],1900,1000);
						 break;

			}
		}
		NRF_Write_Reg(NRFRegSTATUS, 0x40);//清除nrf的中断标志位

    }
	return buf[channel];
  
}

	

/*************************************NRF24L01_TX***************************************/

void NRF24L01_TXDATA(void)
{
	uint8_t sta;
	int16_t t_data = micros();
	SetTX_Mode();

	t_data = t_data * 10;
	TXData[0] = t_data; 
	TXData[1] = t_data >> 8;
	t_data = t_data * 10;
	TXData[2] = t_data;
	TXData[3] = t_data >> 8;
	t_data = t_data * 10;
	TXData[4] = t_data;
	TXData[5] = t_data >> 8;


	SPI_CE_L();
    NRF_Write_Buf(WR_TX_PLOAD - 0x20,TXData,TX_PLOAD_WIDTH);//写数据到TX BUF  32个字节
 	SPI_CE_H();//启动发送

	delay(5);
	NRF_Read_Buf(NRFRegSTATUS,&sta,1); //读取状态寄存器的值	   
	NRF_Write_Reg(NRFRegSTATUS,sta); //清除TX_DS或MAX_RT中断标志
/*
	if(nrf_ & (1<<6)){
		NRF_Write_Reg(NRFRegSTATUS,nrf_flag); //清除TX_DS或MAX_RT中断标志
		SetRX_Mode();
	}
	if(nrf_flag & MAX_TX)//达到最大重发次数
		NRF_Write_Reg(FLUSH_TX - 0X20,0xff);//清除TX FIFO寄存器
	*/
   
}

/*
bool NRF24L01_TxPacket()
{
	uint8_t sta;  

	while(NRF24L01_IRQ!=0);//等待发送完成
	sta=NRF_Read_Reg(NRFRegSTATUS);  //读取状态寄存器的值	   
	NRF_Write_Reg(NRF_WRITE_REG+NRFRegSTATUS,sta); //清除TX_DS或MAX_RT中断标志
	if(sta&MAX_TX)//达到最大重发次数
	{
		NRF_Write_Reg(FLUSH_TX,0xff);//清除TX FIFO寄存器
		return MAX_TX; 
	}
	if(sta&TX_OK)//发送完成
	{
		return TX_OK;
	}
	return 0xff;//其他原因发送失败
}
*/
   
/*************************************NFR24L01_Init*************************************/

bool NRF24L01_INIT(void)
{


		if(NRF24L01_Check())
		{
			SetRX_Mode();
			//SetTX_Mode();
			return true;
		}
		else return false;

	
}


bool NRF24L01_Check(void) 
{ 

	uint8_t buf = 0x77; 
   	uint8_t buf1; 
	
	NRF_Write_Buf(TX_ADDR,&buf,1); 
	delay(2);
	NRF_Read_Buf(TX_ADDR,&buf1,1); 

	if(buf1 == 0x77)
		return true;
	else	return false;

} 

void SetRX_Mode(void)
{
    SPI_CE_L();
	NRF_Write_Reg(FLUSH_RX,0xff);//清除TX FIFO寄存器			 
  	NRF_Write_Buf(RX_ADDR_P0,(uint8_t*)RX_ADDRESS,RX_ADR_WIDTH);//写RX节点地址
   	NRF_Write_Reg(EN_AA,0x01);       //使能通道0的自动应答    
  	NRF_Write_Reg(EN_RXADDR,0x01);   //使能通道0的接收地址  	 
  	NRF_Write_Reg(RF_CH,40);	     //设置RF通信频率		  
  	NRF_Write_Reg(RX_PW_P0,RX_PLOAD_WIDTH);//选择通道0的有效数据宽度 	    
  	NRF_Write_Reg(RF_SETUP,0x0f);   //设置TX发射参数,0db增益,2Mbps,低噪声增益开启   
  	NRF_Write_Reg(CONFIG, 0x0f);    //配置基本工作模式的参数;PWR_UP,EN_CRC,16BIT_CRC,接收模式 
    SPI_CE_H();

} 

//发送模式
void SetTX_Mode(void)
{
    SPI_CE_L();
    NRF_Write_Reg(FLUSH_TX,0xff);//清除TX FIFO寄存器		  
    NRF_Write_Buf(TX_ADDR,(uint8_t*)TX_ADDRESS,TX_ADR_WIDTH);		//写TX节点地址 
  	NRF_Write_Buf(RX_ADDR_P0,(uint8_t*)RX_ADDRESS,RX_ADR_WIDTH); 	//设置TX节点地址,主要为了使能ACK	  
  	NRF_Write_Reg(EN_AA,0x01);     //使能通道0的自动应答    
  	NRF_Write_Reg(EN_RXADDR,0x01); //使能通道0的接收地址  
  	NRF_Write_Reg(SETUP_RETR,0x1a);//设置自动重发间隔时间:500us + 86us;最大自动重发次数:10次
  	NRF_Write_Reg(RF_CH,40);       //设置RF通道为40
  	NRF_Write_Reg(RF_SETUP,0x0f);  //设置TX发射参数,0db增益,2Mbps,低噪声增益开启   
  	NRF_Write_Reg(CONFIG,0x0e);    //配置基本工作模式的参数;PWR_UP,EN_CRC,16BIT_CRC,接收模式,开启所有中断
    SPI_CE_H();
  
} 


