#include "app_config.h"
#include "ril.h"
#include "ril_sms.h"
#include "ql_gpio.h"
#include "ql_stdlib.h"
#include "ql_system.h"
#include "ql_error.h"
#include "ql_type.h"

extern ST_RIL_SMS_TextInfo s_smsInfo;

// Visual feedback by blinking the status LED
void Visual_Feedback(u8 counts) {
    int i;
    for(i = 0; i < (counts * 2); i++) {
        Ql_GPIO_SetLevel(STATUS_LED, (i % 2 == 0)); 
        Ql_Sleep(100);
    }
}


// Forward SMS to the target number
s32 Forward_SMS(char* targetNumber, char* messageContent, u16 contentLen) {
    u32 nMsgRef;
    return RIL_SMS_SendSMS_Text(targetNumber, (u8)Ql_strlen(targetNumber), 
                                LIB_SMS_CHARSET_GSM, (u8*)messageContent, 
                                contentLen, &nMsgRef);
}



// Retrieve SMS details and delete the SMS from storage
bool Get_SMS_Details(u32 index, ST_RIL_SMS_TextInfo* outputStruct) {
    s32 ret;
    Ql_memset(outputStruct, 0, sizeof(ST_RIL_SMS_TextInfo));
    
    ret = RIL_SMS_ReadSMS_Text(index, LIB_SMS_CHARSET_GSM, outputStruct);
    
    RIL_SMS_DeleteSMS(index, RIL_SMS_DEL_INDEXED_MSG);
    
    return (ret == RIL_AT_SUCCESS);
}


// Process incoming SMS logic
void Handle_SMS_Logic(u32 index) {
    ST_RIL_SMS_TextInfo incomingSMS;
    
    if (Get_SMS_Details(index, &incomingSMS)) {
        
        char* messageBody = (char*)incomingSMS.param.deliverParam.data;
        char* senderNum = (char*)incomingSMS.param.deliverParam.oa;

        if (Ql_strstr(messageBody, "ON")) {
            Ql_GPIO_SetLevel(STATUS_LED, PINLEVEL_HIGH);
        }
        
        Forward_SMS(TARGET_PHONE, messageBody, incomingSMS.param.deliverParam.length);
        
        Visual_Feedback(1);
    }
}



s32 Send_Direct_SMS(char* phoneNumber, char* message) {
    u32 nMsgRef;
    s32 ret;
    // Visual_Feedback(8);

    if (phoneNumber == NULL || message == NULL) {
        return QL_RET_ERR_INVALID_PARAMETER;
    }

    ret = RIL_SMS_SendSMS_Text(
        phoneNumber,
        (u8)Ql_strlen(phoneNumber),
        LIB_SMS_CHARSET_GSM,
        (u8*)message,   
        (u16)Ql_strlen(message),
        &nMsgRef
    );

    return ret;
}


s32 Send_Persian_SMS(char* phoneNumber, char* hexMessage) {
    u32 nMsgRef;
    s32 ret;
    Visual_Feedback(8);


    if (phoneNumber == NULL || hexMessage == NULL) {
        return -1;
    }

    ret = RIL_SMS_SendSMS_Text(
        phoneNumber, 
        (u8)Ql_strlen(phoneNumber), 
        LIB_SMS_CHARSET_UCS2,
        (u8*)hexMessage, 
        (u16)Ql_strlen(hexMessage), 
        &nMsgRef
    );

    return ret;
}