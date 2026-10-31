#ifndef __TS_H
#define	__TS_H
#include "stm32f10x.h"
#include "adcx.h"
#include "delay.h"
#include "math.h"

/*****************辰哥单片机设计******************
											STM32
 * 文件			:	TS-300B浊度传感器h文件                   
 * 版本			: V1.0
 * 日期			: 2025.2.2
 * MCU			:	STM32F103C8T6
 * 接口			:	见代码						
 * IP账号		:	辰哥单片机设计（同BILIBILI|抖音|快手|小红书|CSDN|公众号|视频号等）
 * 作者			:	辰哥 
 * 工作室		: 异方辰电子工作室
 * 讲解视频	:	https://www.bilibili.com/video/BV1biNweREAL/?share_source=copy_web
 * 官方网站	:	www.yfcdz.cn

**********************BEGIN***********************/

#define TS_READ_TIMES	10  //TS浊度传感器ADC循环读取次数
#define TS_K 2047.19

/***************根据自己需求更改****************/
// TS GPIO宏定义

#define		TS_GPIO_CLK								RCC_APB2Periph_GPIOA
#define 	TS_GPIO_PORT							GPIOA
#define 	TS_GPIO_PIN								GPIO_Pin_0
#define   ADC_CHANNEL								ADC_Channel_0	// ADC 通道宏定义


/*********************END**********************/


void TS_Init(void);
float TS_GetData(void);

#endif /* __TS_H */

