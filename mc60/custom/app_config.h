#ifndef __APP_CONFIG_H__
#define __APP_CONFIG_H__

#include "ql_type.h"
#include "ril_sms.h"


// HardWare pin definitions
#define STATUS_LED       PINNAME_NETLIGHT 

// Application specific definitions
#define TARGET_PHONE     "+989303016386"
#define SMS_TEXT_MODE    "AT+CMGF=1"
#define SMS_NEW_IND      "AT+CNMI=2,1,0,0,0"

// Function prototypes (to make compiler aware of them)
void Visual_Feedback(u8 counts);
void Handle_SMS_Logic(u32 index);
s32 Forward_SMS(char* targetNumber, char* messageContent, u16 contentLen);
bool Get_SMS_Details(u32 index, ST_RIL_SMS_TextInfo* outputStruct);
s32 Send_Direct_SMS(char* phoneNumber, char* message);
s32 Send_Persian_SMS(char* phoneNumber, char* hexMessage);


#endif