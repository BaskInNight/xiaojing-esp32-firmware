#include "stm32f10x.h"
#include "led.h"
#include "usart.h"
#include "delay.h"
#include "oled.h"
#include "ts.h"
#include "adcx.h"

/*****************辰哥单片机设计******************
											STM32
 * 项目			:	TS-300B浊度传感器实验                     
 * 版本			: V1.0
 * 日期			: 2025.2.2
 * MCU			:	STM32F103C8T6
 * 接口			:	参看ts.h				
 * IP账号		:	辰哥单片机设计（同BILIBILI|抖音|快手|小红书|CSDN|公众号|视频号等）
 * 作者			:	辰哥 
 * 工作室		: 异方辰电子工作室
 * 讲解视频	:	https://www.bilibili.com/video/BV1biNweREAL/?share_source=copy_web
 * 官方网站	:	www.yfcdz.cn

**********************BEGIN***********************/

float value;
u8 buff[30];//参数显示缓存数组
float ts_data;

int main(void)
{ 
	
  SystemInit();//配置系统时钟为72M	
	delay_init(72);
	LED_Init();
	LED_On();
	TS_Init();
	USART1_Config();//串口初始化
	
	OLED_Init();
	printf("Start \n");
	delay_ms(1000);
	
	OLED_Clear();
	//显示“TS:”
	OLED_ShowString(0,0,"TS:",16,1);

  while (1)
  {
		LED_Toggle();
		value = TS_GetData();  
		
		printf("TS: %f\r\n",value);
		OLED_ShowNum(60,0,value,4,16,1);
		

		delay_ms(200);

  }
	
}


