#include "app_config.h"
#include "ril.h"
#include "ril_sms.h"
#include "ql_gpio.h"
#include "ql_stdlib.h"
#include "ql_system.h"
#include "ql_error.h"
#include "ql_type.h"
#include "ql_uart.h"


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

#define UCS2_SMS_MAX_HEX_DIGITS  280  /* 70 UCS2 chars * 4 */

static int is_hex_char2(char c)
{
    return ((c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F'));
}

/* فقط hex ها رو نگه می‌داره، طول رو مضرب 4 می‌کنه، و به 280 رقم hex محدود می‌کنه */
static void ucs2_hex_trim_for_sms(const char *in, char *out, u32 outsz)
{
    u32 i=0, o=0;
    if (!out || outsz==0) return;
    out[0]='\0';
    if (!in) return;

    while (in[i] && (o+1) < outsz)
    {
        char c = in[i++];
        if (is_hex_char2(c)) out[o++] = c;
    }
    out[o] = '\0';

    /* اگر newline داشت (000A)، همونجا قطع کن که پیام کوتاه‌تر بشه */
    {
        char *p = Ql_strstr(out, "000A");
        if (p) *p = '\0';
    }

    /* مضرب 4 */
    o = (u32)Ql_strlen(out);
    o = (o/4)*4;
    out[o] = '\0';

    /* محدودیت UCS2 SMS */
    if (o > UCS2_SMS_MAX_HEX_DIGITS)
    {
        out[UCS2_SMS_MAX_HEX_DIGITS] = '\0';
        out[(UCS2_SMS_MAX_HEX_DIGITS/4)*4] = '\0';
    }
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


s32 Send_Persian_SMS_Fixed(char* phoneNumber, char* ussdHexMessage)
{
    u32 nMsgRef = 0;
    s32 ret;
    char msgTrim[600];

    if (phoneNumber == NULL || ussdHexMessage == NULL) {
        return QL_RET_ERR_INVALID_PARAMETER;
    }

    Ql_memset(msgTrim, 0, sizeof(msgTrim));
    ucs2_hex_trim_for_sms(ussdHexMessage, msgTrim, sizeof(msgTrim));

    if (Ql_strlen(msgTrim) < 4) {
        return QL_RET_ERR_INVALID_PARAMETER;
    }

    /* مهم: برای UCS2، همین HEX رو می‌فرستیم */
    ret = RIL_SMS_SendSMS_Text(
        phoneNumber,
        (u8)Ql_strlen(phoneNumber),
        LIB_SMS_CHARSET_UCS2,
        (u8*)msgTrim,
        (u16)Ql_strlen(msgTrim),
        &nMsgRef
    );

    /* اگر می‌خوای لاگ بگیری */
    {
        char b[80];
        Ql_sprintf(b, "[SMS_UCS2] ret=%ld msgRef=%lu\r\n", (long)ret, (unsigned long)nMsgRef);
        Ql_UART_Write(UART_PORT1, (u8*)b, Ql_strlen(b));
    }

    return ret;
}
