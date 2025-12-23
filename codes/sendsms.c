/*****************************************************************************
* MC60 OpenCPU: Persistent SMS Sender (Retry Logic)
******************************************************************************/

#include "ril.h"
#include "ril_util.h"
#include "ril_sms.h"
#include "ql_stdlib.h"
#include "ql_error.h"
#include "ql_trace.h"
#include "ql_uart.h"
#include "ql_system.h"
#include "ql_gpio.h"

// --- تنظیمات کاربر ---
#define STATUS_LED      PINNAME_NETLIGHT 
// نکته: شماره را حتما با فرمت بین‌المللی +98 بنویسید
#define TARGET_PHONE    "+989303016386"  // <--- شماره خود را اصلاح کنید
#define MCI_SMSC        "+9891100500"    // مرکز پیام همراه اول

// --- تعاریف ثابت ---
#ifndef SIM_STAT_READY
#define SIM_STAT_READY 1
#endif
#ifndef NW_STAT_REGISTERED
#define NW_STAT_REGISTERED 1
#endif
#ifndef NW_STAT_REGISTERED_ROAMING
#define NW_STAT_REGISTERED_ROAMING 5
#endif

// متغیر وضعیت ارسال
bool smsSent = FALSE;
int attemptCount = 0; // شمارنده تلاش‌ها

void proc_main_task(s32 iTaskID)
{
    s32 iResult = 0;
    ST_MSG taskMsg;
    u32 nMsgRef;
    char smscCmd[60];

    // 1. تنظیم LED
    Ql_GPIO_Init(STATUS_LED, PINDIRECTION_OUT, PINLEVEL_LOW, PINPULLSEL_DISABLE);
    
    // تست اولیه (2 ثانیه روشن)
    Ql_GPIO_SetLevel(STATUS_LED, PINLEVEL_HIGH);
    Ql_Sleep(2000);
    Ql_GPIO_SetLevel(STATUS_LED, PINLEVEL_LOW);

    while (TRUE) 
    {
        Ql_OS_GetMessage(&taskMsg);
        switch (taskMsg.message)
        {
        case MSG_ID_RIL_READY:
            Ql_RIL_Initialize();
            break;

        case MSG_ID_URC_INDICATION:
            switch (taskMsg.param1)
            {
            case URC_SYS_INIT_STATE_IND:
                // وقتی سیستم آماده SMS شد
                if (SYS_STATE_SMSOK == taskMsg.param2)
                {
                    // حلقه تلاش برای ارسال (Retry Loop)
                    while (smsSent == FALSE)
                    {
                        attemptCount++;
                        
                        // اعلام تلاش با چشمک (یک چشمک کوتاه)
                        Ql_GPIO_SetLevel(STATUS_LED, PINLEVEL_HIGH);
                        Ql_Sleep(100);
                        Ql_GPIO_SetLevel(STATUS_LED, PINLEVEL_LOW);
                        
                        // صبر اضافه برای اطمینان از شبکه (خیلی مهم)
                        Ql_Sleep(5000); 

                        // 1. تنظیمات اولیه
                        Ql_RIL_SendATCmd("AT+CMGF=1", 10, NULL, NULL, 0);
                        Ql_Sleep(200);
                        Ql_RIL_SendATCmd("AT+CSCS=\"GSM\"", 15, NULL, NULL, 0);
                        Ql_Sleep(200);

                        // 2. تنظیم حافظه روی سیم‌کارت (ممکن است حافظه پیش‌فرض مشکل داشته باشد)
                        Ql_RIL_SendATCmd("AT+CPMS=\"SM\",\"SM\",\"SM\"", 25, NULL, NULL, 0);
                        Ql_Sleep(500);

                        // 3. تنظیم مرکز پیام
                        Ql_sprintf(smscCmd, "AT+CSCA=\"%s\"", MCI_SMSC);
                        Ql_RIL_SendATCmd(smscCmd, Ql_strlen(smscCmd), NULL, NULL, 0);
                        Ql_Sleep(500);

                        // 4. تلاش برای ارسال
                        char* msgBody = "Retry Test OK";
                        iResult = RIL_SMS_SendSMS_Text(TARGET_PHONE, Ql_strlen(TARGET_PHONE), LIB_SMS_CHARSET_GSM, (u8*)msgBody, Ql_strlen(msgBody), &nMsgRef);
                        
                        if (iResult == RIL_AT_SUCCESS)
                        {
                            // *** موفقیت ***
                            smsSent = TRUE;
                            // چراغ را برای همیشه روشن کن
                            Ql_GPIO_SetLevel(STATUS_LED, PINLEVEL_HIGH);
                            while(1) { Ql_Sleep(1000); } // توقف برنامه
                        }
                        else
                        {
                            // *** شکست ***
                            // به جای چشمک سریع، 5 ثانیه صبر می‌کنیم و دوباره حلقه تکرار می‌شود
                            // شاید دفعه بعد شبکه آزاد باشد
                            Ql_Sleep(5000);
                        }
                    }
                }
                break;
            }
            break;
        }
    }
}