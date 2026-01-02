#include "app_config.h"
#include "ril.h"
#include "ql_system.h"
#include "ql_gpio.h"

ST_RIL_SMS_TextInfo s_smsInfo; 

void proc_main_task(s32 iTaskID)
{
    ST_MSG taskMsg;
    
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
                switch (taskMsg.param1) 
                {
                    case URC_SYS_INIT_STATE_IND:
                        if (SYS_STATE_SMSOK == taskMsg.param2) {
                            Ql_RIL_SendATCmd(SMS_TEXT_MODE, 10, NULL, NULL, 0);
                            Ql_RIL_SendATCmd(SMS_NEW_IND, 20, NULL, NULL, 0);
                            Send_Persian_SMS(TARGET_PHONE, "0633064406270645");
                        }
                        break;

                    case URC_NEW_SMS_IND:
                        Handle_SMS_Logic(taskMsg.param2);
                        break;
                }
                break;
        }
    }
}