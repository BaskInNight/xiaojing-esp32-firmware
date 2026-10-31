#include "ts.h"

/*****************辰哥单片机设计******************
											STM32
 * 文件			:	TS-300B浊度传感器c文件                   
 * 版本			: V1.0
 * 日期			: 2024.2.2
 * MCU			:	STM32F103C8T6
 * 接口			:	见代码				
 * IP账号		:	辰哥单片机设计（同BILIBILI|抖音|快手|小红书|CSDN|公众号|视频号等）
 * 作者			:	辰哥 
 * 工作室		: 异方辰电子工作室
 * 讲解视频	:	https://www.bilibili.com/video/BV1biNweREAL/?share_source=copy_web
 * 官方网站	:	www.yfcdz.cn

**********************BEGIN***********************/

void TS_Init(void)
{
	{
		GPIO_InitTypeDef GPIO_InitStructure;
		
		RCC_APB2PeriphClockCmd (TS_GPIO_CLK, ENABLE );	// 打开 ADC IO端口时钟
		GPIO_InitStructure.GPIO_Pin = TS_GPIO_PIN;					// 配置 ADC IO 引脚模式
		GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AIN;		// 设置为模拟输入
		
		GPIO_Init(TS_GPIO_PORT, &GPIO_InitStructure);				// 初始化 ADC IO

		ADCx_Init();
	}

}


uint16_t TS_ADC_Read(void)
{
	//设置指定ADC的规则组通道，采样时间
	return ADC_GetValue(ADC_CHANNEL, ADC_SampleTime_55Cycles5);
}



float TS_GetData(void)
{
	float  tempData = 0;
	float TS_DAT;

	for (uint8_t i = 0; i < TS_READ_TIMES; i++)
	{
		tempData += TS_ADC_Read();
		delay_ms(5);
	}
	tempData /= TS_READ_TIMES;
	
	TS_DAT = (tempData/4096.0)*3.3;
	TS_DAT = -865.68*TS_DAT+TS_K;

	if(TS_DAT<35) TS_DAT = 0;
	
	return TS_DAT;

}



