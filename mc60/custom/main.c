/*****************************************************************************
* MC60 OpenCPU: Final SMS Bridge (Via Debug Port)
* Author: Taha
******************************************************************************/

#include "ql_trace.h"
#include "ql_system.h"
#include "ql_uart.h"
#include "ril.h"
#include "ql_gpio.h"
#include "ql_stdlib.h"

#define STATUS_LED PINNAME_NETLIGHT

// بافر برای ساخت پیام نهایی
char strFinalMsg[512];
char cmdBuffer[100];

// --- تابع ارسال به ESP32 ---
void Send_To_ESP32(char* smsText)
{
    // حذف کاراکترهای اضافه مثل \r و \n از ته پیام (اختیاری ولی تمیزتره)
    // (اینجا ساده میگیریم و مستقیم میفرستیم)
    
    // فرمت کردن پیام: SMS_DATA:MatnePayam
    // استفاده از Ql_Debug_Trace چون دیدیم این پورت وصله و کار میکنه
    Ql_sprintf(strFinalMsg, "SMS_DATA:%s\r\n", smsText);
    Ql_Debug_Trace(strFinalMsg);
    
    // یه چشمک بزن که بفهمیم ارسال شد
    Ql_GPIO_SetLevel(STATUS_LED, PINLEVEL_LOW);
    Ql_Sleep(200);
    Ql_GPIO_SetLevel(STATUS_LED, PINLEVEL_HIGH);
}


static s32 ReadSMS_Handler(char* line, u32 len, void* userData)
{
    char dbgBuffer[200];
    
    // 1. چاپ دیتای خام (بدون هیچ تغییری) برای اینکه ببینیم چی میاد
    // از براکت [] استفاده میکنیم تا اگر فاصله یا اینتر اولش بود دیده بشه
    Ql_sprintf(dbgBuffer, "RAW_DATA:[%s]\r\n", line);
    Ql_Debug_Trace(dbgBuffer);

    // 2. منطق ساده‌تر شده برای پیدا کردن متن پیام
    // اگر خط خالی نبود و دستورات سیستمی توش نبود، بفرستش
    if (len > 1 &&                         // طول بیشتر از 1 (حتی Hi هم رد بشه)
        Ql_strstr(line, "OK") == NULL && 
        Ql_strstr(line, "ERROR") == NULL &&
        Ql_strstr(line, "+CMGR:") == NULL)
    {
        Ql_Debug_Trace("-> MATCH FOUND! Sending to ESP32...\r\n");
        Send_To_ESP32(line);
    }
    
    return 0;
}

// --- کد اصلاح شده MC60 (بخش Main Task) ---

void proc_main_task(s32 taskId)
{
    ST_MSG msg;

    // تنظیم LED
    Ql_GPIO_Init(STATUS_LED, PINDIRECTION_OUT, PINLEVEL_HIGH, PINPULLSEL_DISABLE);

    // کمی صبر برای بالا اومدن سیستم
    Ql_Sleep(2000);
    
    // ارسال پیام شروع
    Ql_Debug_Trace("\r\n>>> SMS BRIDGE READY <<<\r\n");

    while (TRUE)
    {
        Ql_OS_GetMessage(&msg);
        switch(msg.message)
        {
            // وقتی ماژول آماده شد (RIL Ready)
            case MSG_ID_RIL_READY:
                Ql_Debug_Trace("Setting up SMS mode...\r\n");
                
                // 1. تنظیم فرمت متنی (Text Mode)
                Ql_RIL_SendATCmd("AT+CMGF=1", Ql_strlen("AT+CMGF=1"), NULL, NULL, 0);
                
                // 2. تنظیم مهم: زبان روی GSM (برای اینکه متن انگلیسی درست بیاد)
                Ql_RIL_SendATCmd("AT+CSCS=\"GSM\"", Ql_strlen("AT+CSCS=\"GSM\""), NULL, NULL, 0);
                
                // 3. نمایش متن پیام مستقیم در خروجی
                Ql_RIL_SendATCmd("AT+CSDH=1", Ql_strlen("AT+CSDH=1"), NULL, NULL, 0);
                
                // 4. تنظیم نحوه اطلاع رسانی
                Ql_RIL_SendATCmd("AT+CNMI=2,1", Ql_strlen("AT+CNMI=2,1"), NULL, NULL, 0);
                
                // 5. محل ذخیره (روی سیم‌کارت)
                Ql_RIL_SendATCmd("AT+CPMS=\"SM\",\"SM\",\"SM\"", Ql_strlen("AT+CPMS=\"SM\",\"SM\",\"SM\""), NULL, NULL, 0);
                
                Ql_Debug_Trace("Setup Complete! Waiting for SMS...\r\n");
                break;

            case MSG_ID_URC_INDICATION:
                switch(msg.param1)
                {
                    case URC_NEW_SMS_IND:
                        Ql_Debug_Trace("New SMS Detected! Reading...\r\n");
                        if (msg.param2 != 0)
                        {
                            char* strURC = (char*)msg.param2;
                            char* pComma = Ql_strstr(strURC, ",");
                            if (pComma != NULL)
                            {
                                int index = Ql_atoi(pComma + 1);
                                Ql_sprintf(cmdBuffer, "AT+CMGR=%d", index);
                                // خواندن پیام
                                Ql_RIL_SendATCmd(cmdBuffer, Ql_strlen(cmdBuffer), ReadSMS_Handler, NULL, 0);
                            }
                        }
                        break;
                }
                break;
        }
    }
}