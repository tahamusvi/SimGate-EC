#include "ril.h"
#include "ril_util.h"
#include "ril_sms.h"
#include "ql_stdlib.h"
#include "ql_error.h"
#include "ql_trace.h"
#include "ql_system.h"
#include "ql_gpio.h"

#define STATUS_LED       PINNAME_NETLIGHT 
#define TARGET_PHONE    "+989939136493"

void proc_main_task(s32 iTaskID)
{
    ST_MSG taskMsg;
    u32 nMsgRef;
    s32 ret;

    Ql_GPIO_Init(STATUS_LED, PINDIRECTION_OUT, PINLEVEL_LOW, PINPULLSEL_DISABLE);

    while (TRUE) 
    {
        Ql_OS_GetMessage(&taskMsg);
        switch (taskMsg.message)
        {
            case MSG_ID_RIL_READY:
                Ql_RIL_Initialize();
                break;

            case MSG_ID_URC_INDICATION:
                if (URC_SYS_INIT_STATE_IND == taskMsg.param1 && SYS_STATE_SMSOK == taskMsg.param2)
                {
                    Ql_GPIO_SetLevel(STATUS_LED, PINLEVEL_HIGH);
                    Ql_Sleep(3000);

                    Ql_RIL_SendATCmd("AT+CMGF=1", 10, NULL, NULL, 0);
                    Ql_Sleep(500);
                    
                    Ql_RIL_SendATCmd("AT+CPMS=\"SM\",\"SM\",\"SM\"", 20, NULL, NULL, 0);
                    Ql_Sleep(500);

                    char* testMsg = "Hiii Azin!";
                    ret = RIL_SMS_SendSMS_Text(TARGET_PHONE, Ql_strlen(TARGET_PHONE), LIB_SMS_CHARSET_GSM, (u8*)testMsg, Ql_strlen(testMsg), &nMsgRef);

                    if (ret == RIL_AT_SUCCESS) {
                        for(int i=0; i<5; i++){
                            Ql_GPIO_SetLevel(STATUS_LED, PINLEVEL_LOW); Ql_Sleep(200);
                            Ql_GPIO_SetLevel(STATUS_LED, PINLEVEL_HIGH); Ql_Sleep(200);
                        }
                    } else {
                        // اگر شکست خورد: LED خاموش می‌شود
                        Ql_GPIO_SetLevel(STATUS_LED, PINLEVEL_LOW);
                    }
                }
                break;
        }
    }
}