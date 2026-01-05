/*****************************************************************************
*  Copyright Statement:
*  --------------------
*  This software is protected by Copyright and the information contained
*  herein is confidential. The software may not be copied and the information
*  contained herein may not be used or disclosed except with the written
*  permission of Quectel Co., Ltd. 2013/2019
*****************************************************************************/
/*****************************************************************************
 *
 * Filename:
 * ---------
 *   ril_urc.c
 *
 * Description:
 * ------------
 *   The module handles URC in RIL.
 *
 ****************************************************************************/

#include "custom_feature_def.h"
#include "ql_stdlib.h"
#include "ril.h"
#include "ril_util.h"
#include "ril_sms.h"
#include "ril_telephony.h"
#include "ql_power.h"
#include "ql_system.h"
#include "ql_uart.h"
#include "ril_audio.h"
#include "ril_ftp.h"
#include "ril_http.h"

#ifdef __OCPU_RIL_SUPPORT__

/************************************************************************/
/* Definition for URC receive task id.                                  */
/************************************************************************/
#define URC_RCV_TASK_ID  main_task_id

/************************************************************************/
/* Custom URC id for MQTT receive indication                            */
/************************************************************************/
#define URC_MQTT_RECV_IND   2001

/************************************************************************/
/* Custom URC id for USSD receive indication (+CUSD)                     */
/************************************************************************/
#define URC_USSD_RECV_IND   2002

/************************************************************************/
/* Declarations for URC handler.                                        */
/************************************************************************/
static void OnURCHandler_Call(const char* strURC, void* reserved);
static void OnURCHandler_SMS(const char* strURC, void* reserved);
static void OnURCHandler_Network(const char* strURC, void* reserved);
static void OnURCHandler_SIM(const char* strURC, void* reserved);
static void OnURCHandler_CFUN(const char* strURC, void* reserved);
static void OnURCHandler_Voltage(const char* strURC, void* reserved);
static void OnURCHandler_InitStat(const char* strURC, void* reserved);
static void OnURCHandler_HTTP(const char* strURC, void* reserved);
static void OnURCHandler_FTP(const char* strURC, void* reserved);
static void OnURCHandler_AlarmRing(const char* strURC, void* reserved);
static void OnURCHandler_AudPlayInd(const char* strURC, void* reserved);
static void OnURCHandler_Undefined(const char* strURC, void* reserved);

/* MQTT URC handler */
static void OnURCHandler_MQTT(const char* strURC, void* reserved);

/* USSD URC handler (+CUSD) */
static void OnURCHandler_USSD(const char* strURC, void* reserved);

/* optional external handlers */
extern void OnURCHandler_QCELLLocation(const char* strURC, void* reserved);
extern void OnURCHandler_QToneDet(const char* strURC, void* reserved);
extern void OnURCHandler_QWDTMF(const char* strURC, void* reserved);
extern void OnURCHandler_GPSCMD(const char* strURC, void* reserved);
extern void OnURCHandler_NTPCMD(const char* strURC, void* reserved);

#ifdef __OCPU_RIL_BT_SUPPORT__
extern void OnURCHandler_BTScan(const char* strURC, void* reserved);
extern void OnURCHandler_BTPair(const char* strURC, void* reserved);
extern void OnURCHandler_BTPairCnf(const char* strURC, void* reserved);
extern void OnURCHandler_BTConn(const char* strURC, void* reserved);
extern void OnURCHandler_BTConnCnf(const char* strURC, void* reserved);
extern void OnURCHandler_BTDisconn(const char* strURC, void* reserved);
extern void OnURCHandler_BTIndication(const char* strURC, void* reserved);
extern void OnURCHandler_BTVisible(const char* strURC, void* reserved);
#endif

/************************************************************************/
/* Customer ATC URC callback                                            */
/************************************************************************/
CallBack_Ftp_Upload   FtpPut_IND_CB = NULL;
CallBack_Ftp_Download FtpGet_IND_CB = NULL;

/************************************************************************/
/* MQTT topic/payload cache (exposed to app)                             */
/************************************************************************/
static char g_mqtt_topic[256];
static char g_mqtt_payload[1024];

const char* RIL_MQTT_GetTopic(void)   { return g_mqtt_topic; }
const char* RIL_MQTT_GetPayload(void) { return g_mqtt_payload; }

/************************************************************************/
/* USSD cache (exposed to app)                                           */
/************************************************************************/
static char g_ussd_str[800];   // قبلا 300 بود
static int  g_ussd_dcs = -1;

const char* RIL_USSD_GetStr(void) { return g_ussd_str; }
int         RIL_USSD_GetDcs(void) { return g_ussd_dcs; }

/************************************************************************/
/* Small debug helper: direct UART print                                */
/************************************************************************/
static void URC_DBG(const char *s)
{
    if (!s) return;
    /* اگر UART1 برای دیباگ شماست، همین خوبه */
    Ql_UART_Write(UART_PORT1, (u8*)s, (u32)Ql_strlen(s));
}

/************************************************************************/
/* Helpers for parsing                                                   */
/************************************************************************/
static int parse_first_quoted_urc(const char *s, char *out, u32 outsz)
{
    const char *q1, *q2;
    u32 n;

    if (!s || !out || outsz == 0) return -1;
    out[0] = '\0';

    q1 = Ql_strchr(s, '"');
    if (!q1) return -1;
    q2 = Ql_strchr(q1 + 1, '"');
    if (!q2) return -1;

    n = (u32)(q2 - (q1 + 1));
    if (n >= outsz) n = outsz - 1;
    Ql_memcpy(out, q1 + 1, n);
    out[n] = '\0';
    return 0;
}

static char* find_last_comma_urc(char *s)
{
    char *p = s, *last = NULL;
    if (!s) return NULL;
    while (*p)
    {
        if (*p == ',') last = p;
        p++;
    }
    return last;
}

/****************************************************/
/* Definitions for system URCs and the handler      */
/****************************************************/
const static ST_URC_HDLENTRY m_SysURCHdlEntry[] = {

    //Telephony unsolicited response
    {"\r\n+CRING: VOICE\r\n",                     OnURCHandler_Call},
    {"\r\nRING\r\n",                              OnURCHandler_Call},
    {"\r\nBUSY\r\n",                              OnURCHandler_Call},
    {"\r\nNO ANSWER\r\n",                         OnURCHandler_Call},
    {"\r\nNO CARRIER\r\n",                        OnURCHandler_Call},
    {"\r\nNO DIALTONE\r\n",                       OnURCHandler_Call},
    {"\r\n+CLIP:",                                OnURCHandler_Call},

    //SMS unsolicited response
    {"\r\n+CMTI:",                                OnURCHandler_SMS},

    //Network status unsolicited response
    {"\r\n+CREG:",                                OnURCHandler_Network},
    {"\r\n+CGREG:",                               OnURCHandler_Network},

    //SIM card unsolicited response
    {"\r\n+CPIN:",                                OnURCHandler_SIM},

    //CFUN unsolicited response
    {"\r\n+CFUN:",                                OnURCHandler_CFUN},

    //Voltage indication
    {"\r\nUNDER_VOLTAGE WARNING \r\n",            OnURCHandler_Voltage},
    {"\r\nUNDER_VOLTAGE POWER DOWN \r\n",         OnURCHandler_Voltage},
    {"\r\nOVER_VOLTAGE WARNING \r\n",             OnURCHandler_Voltage},
    {"\r\nOVER_VOLTAGE POWER DOWN \r\n",          OnURCHandler_Voltage},

    //Init status unsolicited response
    {"\r\nCall Ready\r\n",                        OnURCHandler_InitStat},
    {"\r\nSMS Ready\r\n",                         OnURCHandler_InitStat},

    // Clock alarm ring indication
    {"\r\nALARM RING\r\n",                        OnURCHandler_AlarmRing},
    {"\r\nALARM MODE\r\n",                        OnURCHandler_AlarmRing},

    // Location indication
    {"\r\n+QCELLLOC:",                            OnURCHandler_QCELLLocation},
};

/****************************************************/
/* Definitions for AT URCs and the handler          */
/****************************************************/
const static ST_URC_HDLENTRY m_AtURCHdlEntry[] = {
    //HTTP unsolicited response
    {"\r\n+QHTTPDL:",                             OnURCHandler_HTTP},

    //FTP unsolicited response
    {"\r\n+QFTPGET:",                             OnURCHandler_FTP},
    {"\r\n+QFTPPUT:",                             OnURCHandler_FTP},

    //Audio playing indication
    {"\r\n+QAUDPIND:",                            OnURCHandler_AudPlayInd},
    {"\r\n+QPLAYRES:",                            OnURCHandler_AudPlayInd},
    {"\r\n+QPRESBG:",                             OnURCHandler_AudPlayInd},

    /* USSD receive URC */
    {"+CUSD:",                                    OnURCHandler_USSD},

    /* MQTT receive URC
       نکته مهم: بدون \r\n هم match می‌کنیم تا اگر فریمور format فرق داشت، گیر نکنه.
    */
    {"+QMTRECV:",                                 OnURCHandler_MQTT},

#ifdef __OCPU_RIL_BT_SUPPORT__
    {"\r\n+QBTSCAN:",                             OnURCHandler_BTScan},
    {"\r\n+QBTPAIR:",                             OnURCHandler_BTPair},
    {"\r\n+QBTPAIRCNF:",                          OnURCHandler_BTPairCnf},
    {"\r\n+QBTCONN:",                             OnURCHandler_BTConn},
    {"\r\n+QBTACPT:",                             OnURCHandler_BTConnCnf},
    {"\r\n+QBTDISC:",                             OnURCHandler_BTConnCnf},
    {"\r\n+QBTDISCONN:",                          OnURCHandler_BTDisconn},
    {"\r\n+QBTIND:",                              OnURCHandler_BTIndication},
    {"\r\n+QBTVISB:",                             OnURCHandler_BTVisible},
#endif

    // DTMF unsolicited response
    {"\r\n+QTONEDET:",                            OnURCHandler_QToneDet},
    {"\r\n+QWDTMF:",                              OnURCHandler_QWDTMF},

    // GNSS unsolicited response
    {"\r\n+QGNSSCMD:",                            OnURCHandler_GPSCMD},

    // NTP unsolicited response
    {"\r\n+QNTP:",                                OnURCHandler_NTPCMD},
};

/************************************************************************/
/* URC handlers (mostly stock)                                          */
/************************************************************************/
static void OnURCHandler_SIM(const char* strURC, void* reserved)
{
    char* p1 = NULL;
    char* p2 = NULL;
    char strTmp[20];
    s32 len;
    extern s32 RIL_SIM_GetSimStateByName(char* simStat, u32 len);

    Ql_memset(strTmp, 0x0, sizeof(strTmp));
    len = Ql_sprintf(strTmp, "\r\n+CPIN: ");
    if (Ql_StrPrefixMatch(strURC, strTmp))
    {
        p1 = Ql_strstr(strURC, "\r\n+CPIN: ");
        p1 += len;
        p2 = Ql_strstr(p1, "\r\n");
        if (p1 && p2)
        {
            u32 cpinStat;
            Ql_memset(strTmp, 0x0, sizeof(strTmp));
            Ql_memcpy(strTmp, p1, p2 - p1);
            cpinStat = (u32)RIL_SIM_GetSimStateByName(strTmp, p2 - p1);
            Ql_OS_SendMessage(URC_RCV_TASK_ID, MSG_ID_URC_INDICATION, URC_SIM_CARD_STATE_IND, cpinStat);
        }
    }
}

static void OnURCHandler_Network(const char* strURC, void* reserved)
{
    char* p1 = NULL;
    char* p2 = NULL;
    char strTmp[10];

    if (Ql_StrPrefixMatch(strURC, "\r\n+CREG: "))
    {
        u32 nwStat;
        p1 = Ql_strstr(strURC, "\r\n+CREG: ");
        p1 += Ql_strlen("\r\n+CREG: ");
        p2 = Ql_strstr(p1, "\r\n");
        if (p1 && p2)
        {
            Ql_memset(strTmp, 0x0, sizeof(strTmp));
            Ql_memcpy(strTmp, p1, p2 - p1);
            nwStat = Ql_atoi(strTmp);
            Ql_OS_SendMessage(URC_RCV_TASK_ID, MSG_ID_URC_INDICATION, URC_GSM_NW_STATE_IND, nwStat);
        }
    }
    else if (Ql_StrPrefixMatch(strURC, "\r\n+CGREG: "))
    {
        u32 nwStat;
        p1 = Ql_strstr(strURC, "\r\n+CGREG: ");
        p1 += Ql_strlen("\r\n+CGREG: ");
        p2 = Ql_strstr(p1, "\r\n");
        if (p1 && p2)
        {
            Ql_memset(strTmp, 0x0, sizeof(strTmp));
            Ql_memcpy(strTmp, p1, p2 - p1);
            nwStat = Ql_atoi(strTmp);
            Ql_OS_SendMessage(URC_RCV_TASK_ID, MSG_ID_URC_INDICATION, URC_GPRS_NW_STATE_IND, nwStat);
        }
    }
}

static void OnURCHandler_Call(const char* strURC, void* reserved)
{
    char* p1 = NULL;
    char* p2 = NULL;
    char strTmp[10];

    if (Ql_StrPrefixMatch(strURC, "\r\nRING\r\n") ||
        Ql_StrPrefixMatch(strURC, "\r\n+CLIP:") ||
        Ql_StrPrefixMatch(strURC, "\r\n+CRING: VOICE\r\n"))
    {
        extern ST_ComingCallInfo  g_comingCall;
        u16 len;

        p1 = Ql_strstr(strURC, "\r\n+CLIP:");
        if (!p1) return;

        g_comingCall.ringCnt++;
        if ((g_comingCall.ringCnt / 6) > 0) g_comingCall.ringCnt %= 6;

        p1 += Ql_strlen("\r\n+CLIP:");
        p2 = Ql_strstr(p1 + 1, ",");
        len = p2 - (p1 + 2) - 1;
        Ql_memcpy(g_comingCall.comingCall[g_comingCall.ringCnt].phoneNumber, p1 + 2, len);
        g_comingCall.comingCall[g_comingCall.ringCnt].phoneNumber[len] = '\0';

        p1 = p2;
        p2 = Ql_strstr(p1 + 1, ",");
        Ql_memset(strTmp, 0x0, sizeof(strTmp));
        Ql_memcpy(strTmp, p1 + 1, p2 - p1 - 1);
        g_comingCall.comingCall[g_comingCall.ringCnt].type = Ql_atoi(strTmp);

        Ql_OS_SendMessage(URC_RCV_TASK_ID, MSG_ID_URC_INDICATION,
                          URC_COMING_CALL_IND,
                          (u32)(&(g_comingCall.comingCall[g_comingCall.ringCnt])));
    }
    else if (Ql_StrPrefixMatch(strURC, "\r\nBUSY\r\n") ||
             Ql_StrPrefixMatch(strURC, "\r\nNO ANSWER\r\n") ||
             Ql_StrPrefixMatch(strURC, "\r\nNO CARRIER\r\n") ||
             Ql_StrPrefixMatch(strURC, "\r\nNO DIALTONE\r\n"))
    {
        u32 callStat;

        if (Ql_StrPrefixMatch(strURC, "\r\nBUSY\r\n")) callStat = CALL_STATE_BUSY;
        else if (Ql_StrPrefixMatch(strURC, "\r\nNO ANSWER\r\n")) callStat = CALL_STATE_NO_ANSWER;
        else if (Ql_StrPrefixMatch(strURC, "\r\nNO CARRIER\r\n")) callStat = CALL_STATE_NO_CARRIER;
        else if (Ql_StrPrefixMatch(strURC, "\r\nNO DIALTONE\r\n")) callStat = CALL_STATE_NO_DIALTONE;
        else return;

        Ql_OS_SendMessage(URC_RCV_TASK_ID, MSG_ID_URC_INDICATION, URC_CALL_STATE_IND, callStat);
    }
}

static void OnURCHandler_SMS(const char* strURC, void* reserved)
{
    char* p1 = NULL;
    char* p2 = NULL;

    if (Ql_StrPrefixMatch(strURC, "\r\n+CMTI:"))
    {
        u32 smsIndex;
        char mem[SMS_MEM_CHAR_LEN];

        p1 = Ql_strstr(strURC, ":");
        p1 += 3;
        p2 = Ql_strstr(p1, ",");
        if (p1 && p2)
        {
            Ql_memset(mem, 0x0, sizeof(mem));
            Ql_strncpy(mem, p1, (p2 - p1 - 1));
        }

        p1 = p2;
        p2 = Ql_strstr(p1, "\r\n");
        if (p1 && p2)
        {
            char strIndex[10];
            Ql_memset(strIndex, 0x0, sizeof(strIndex));
            Ql_strncpy(strIndex, p1 + 1, p2 - p1 - 1);
            smsIndex = Ql_atoi(strIndex);
            Ql_OS_SendMessage(URC_RCV_TASK_ID, MSG_ID_URC_INDICATION, URC_NEW_SMS_IND, smsIndex);
        }
    }
}

static void OnURCHandler_Voltage(const char* strURC, void* reserved)
{
    u32 volState = VBATT_UNDER_WRN;

    if (Ql_StrPrefixMatch(strURC, "\r\nUNDER_VOLTAGE WARNING \r\n")) volState = VBATT_UNDER_WRN;
    else if (Ql_StrPrefixMatch(strURC, "\r\nUNDER_VOLTAGE POWER DOWN \r\n")) volState = VBATT_UNDER_PDN;
    else if (Ql_StrPrefixMatch(strURC, "\r\nOVER_VOLTAGE WARNING \r\n")) volState = VBATT_OVER_WRN;
    else if (Ql_StrPrefixMatch(strURC, "\r\nOVER_VOLTAGE POWER DOWN \r\n")) volState = VBATT_OVER_PDN;
    else return;

    Ql_OS_SendMessage(URC_RCV_TASK_ID, MSG_ID_URC_INDICATION, URC_MODULE_VOLTAGE_IND, volState);
}

static void OnURCHandler_InitStat(const char* strURC, void* reserved)
{
    u32 sysInitStat = SYS_STATE_START;

    if (Ql_strstr(strURC, "\r\nCall Ready\r\n") != NULL) sysInitStat = SYS_STATE_PHBOK;
    else if (Ql_strstr(strURC, "\r\nSMS Ready\r\n") != NULL) sysInitStat = SYS_STATE_SMSOK;

    Ql_OS_SendMessage(URC_RCV_TASK_ID, MSG_ID_URC_INDICATION, URC_SYS_INIT_STATE_IND, sysInitStat);
}

static void OnURCHandler_CFUN(const char* strURC, void* reserved)
{
    char* p1 = NULL;
    char* p2 = NULL;
    char strTmp[10];
    s32 len;
    u32 cfun;

    len = Ql_strlen("\r\n+CFUN: ");
    p1 = Ql_strstr(strURC, "\r\n+CFUN: ");
    p1 += len;
    p2 = Ql_strstr(p1, "\r\n");
    if (p1 && p2)
    {
        Ql_memset(strTmp, 0x0, sizeof(strTmp));
        Ql_memcpy(strTmp, p1, 1);
        cfun = Ql_atoi(strTmp);
        Ql_OS_SendMessage(URC_RCV_TASK_ID, MSG_ID_URC_INDICATION, URC_CFUN_STATE_IND, cfun);
    }
}

static void OnURCHandler_HTTP(const char* strURC, void* reserved)
{
    u32 dwnLoadedSize = 0;
    u32 contentLen = 0;
    s32 errCode = 0;
    extern CB_HTTP_DwnldFile callback_http_dwnld;

    Ql_sscanf(strURC, "%*[^: ]: %d,%d,%d[^\r\n]", &dwnLoadedSize, &contentLen, &errCode);
    if (callback_http_dwnld)
    {
        callback_http_dwnld(dwnLoadedSize, contentLen, errCode);
        callback_http_dwnld = NULL;
    }
}

static void OnURCHandler_FTP(const char* strURC, void* reserved)
{
    char* p1 = NULL;
    char* p2 = NULL;
    s32 nFtpDlLen = 0;
    char strTmp[10];

    p1 = Ql_strstr(strURC, "\r\n+QFTPGET:");
    if (p1)
    {
        p1 += Ql_strlen("\r\n+QFTPGET:");
        p2 = Ql_strstr(p1, "\r\n");
        if (p2)
        {
            Ql_memset(strTmp, 0x0, sizeof(strTmp));
            Ql_memcpy(strTmp, p1, p2 - p1);
            nFtpDlLen = Ql_atoi(strTmp);
            if (NULL != FtpGet_IND_CB)
            {
                FtpGet_IND_CB((nFtpDlLen < 0) ? 0 : 1, nFtpDlLen);
                FtpGet_IND_CB = NULL;
                return;
            }
        }
    }

    p1 = Ql_strstr(strURC, "\r\n+QFTPPUT:");
    if (p1)
    {
        p1 += Ql_strlen("\r\n+QFTPPUT:");
        p2 = Ql_strstr(p1, "\r\n");
        if (p2)
        {
            Ql_memset(strTmp, 0x0, sizeof(strTmp));
            Ql_memcpy(strTmp, p1, p2 - p1);
            nFtpDlLen = Ql_atoi(strTmp);
            if (NULL != FtpPut_IND_CB)
            {
                FtpPut_IND_CB((nFtpDlLen < 0) ? 0 : 1, nFtpDlLen);
                FtpPut_IND_CB = NULL;
                return;
            }
        }
    }
}

static void OnURCHandler_AlarmRing(const char* strURC, void* reserved)
{
    Ql_OS_SendMessage(URC_RCV_TASK_ID, MSG_ID_URC_INDICATION, URC_ALARM_RING_IND, 0);
}

static void OnURCHandler_Undefined(const char* strURC, void* reserved)
{
    Ql_OS_SendMessage(URC_RCV_TASK_ID, MSG_ID_URC_INDICATION, URC_END, 0);
}

static void OnURCHandler_AudPlayInd(const char* strURC, void* reserved)
{
    s32 errCode1 = 0;
    s32 errCode2 = 0;
    extern RIL_AUD_PLAY_IND cb_aud_play;

    Ql_sscanf(strURC, "%*[^: ]: %d,%d[^\r\n]", &errCode1, &errCode2);
    if (cb_aud_play) cb_aud_play(errCode2);
}

/************************************************************************/
/* USSD URC handler (+CUSD)                                             */
/************************************************************************/
static void OnURCHandler_USSD(const char* strURC, void* reserved)
{
    (void)reserved;

    char tmp[360];
    char ussd[300];
    int  dcs = -1;

    if (!strURC) return;

    URC_DBG("[ril_urc] OnURCHandler_USSD fired\r\n");

    Ql_memset(tmp, 0, sizeof(tmp));
    Ql_memset(ussd, 0, sizeof(ussd));

    Ql_strncpy(tmp, strURC, sizeof(tmp) - 1);

    /* استخراج اولین متن داخل کوتیشن: +CUSD: m,"<str>",dcs */
    if (0 == parse_first_quoted_urc(tmp, ussd, sizeof(ussd)))
    {
        /* dcs معمولاً آخرین فیلد بعد از آخرین ',' است */
        {
            char *lc = find_last_comma_urc(tmp);
            if (lc) dcs = Ql_atoi(lc + 1);
        }

        Ql_memset(g_ussd_str, 0, sizeof(g_ussd_str));
        Ql_strncpy(g_ussd_str, ussd, sizeof(g_ussd_str) - 1);
        g_ussd_dcs = dcs;

        Ql_OS_SendMessage(URC_RCV_TASK_ID, MSG_ID_URC_INDICATION, URC_USSD_RECV_IND, 0);
    }
    else
    {
        /* اگر کوتیشن نداشت، برای اینکه خالی نمونه، کل خط رو کوتاه ذخیره کن */
        Ql_memset(g_ussd_str, 0, sizeof(g_ussd_str));
        Ql_strncpy(g_ussd_str, tmp, sizeof(g_ussd_str) - 1);
        g_ussd_dcs = -1;

        Ql_OS_SendMessage(URC_RCV_TASK_ID, MSG_ID_URC_INDICATION, URC_USSD_RECV_IND, 0);
    }
}

/************************************************************************/
/* MQTT URC handler (robust + debug)                                    */
/************************************************************************/
static void OnURCHandler_MQTT(const char* strURC, void* reserved)
{
    (void)reserved;

    const char *q1, *q2, *q3, *q4, *qlast;
    u32 n;

    if (!strURC) return;

    /* direct proof that handler fires */
    URC_DBG("[ril_urc] OnURCHandler_MQTT fired\r\n");

    /* Find first quoted (topic start/end) */
    q1 = Ql_strchr(strURC, '"'); if (!q1) return;
    q2 = Ql_strchr(q1 + 1, '"'); if (!q2) return;

    /* Find first quote after topic -> start of payload (or maybe length then payload) */
    q3 = Ql_strchr(q2 + 1, '"'); if (!q3) return;

    /* Find last quote in the line -> end of payload */
    qlast = NULL;
    {
        const char *p = q3;
        while (p && *p)
        {
            const char *nx = Ql_strchr(p, '"');
            if (!nx) break;
            qlast = nx;
            p = nx + 1;
        }
    }
    if (!qlast || qlast <= q3) return;

    Ql_memset(g_mqtt_topic, 0, sizeof(g_mqtt_topic));
    Ql_memset(g_mqtt_payload, 0, sizeof(g_mqtt_payload));

    /* topic */
    n = (u32)(q2 - (q1 + 1));
    if (n >= sizeof(g_mqtt_topic)) n = sizeof(g_mqtt_topic) - 1;
    Ql_memcpy(g_mqtt_topic, q1 + 1, n);
    g_mqtt_topic[n] = '\0';

    /* payload: between q3 and qlast (excluding quotes) */
    q4 = qlast;
    n = (u32)(q4 - (q3 + 1));
    if (n >= sizeof(g_mqtt_payload)) n = sizeof(g_mqtt_payload) - 1;
    Ql_memcpy(g_mqtt_payload, q3 + 1, n);
    g_mqtt_payload[n] = '\0';

    /* notify main task */
    Ql_OS_SendMessage(URC_RCV_TASK_ID, MSG_ID_URC_INDICATION, URC_MQTT_RECV_IND, 0);
}

/*****************************************************************
* URC entrance
*****************************************************************/
void OnURCHandler(const char* strURC, void* reserved)
{
    s32 i;

    if (NULL == strURC) return;

    // For system URCs
    for (i = 0; i < NUM_ELEMS(m_SysURCHdlEntry); i++)
    {
        if (Ql_strstr(strURC, m_SysURCHdlEntry[i].keyword))
        {
            m_SysURCHdlEntry[i].handler(strURC, reserved);
            return;
        }
    }

    // For AT URCs
    for (i = 0; i < NUM_ELEMS(m_AtURCHdlEntry); i++)
    {
        if (Ql_strstr(strURC, m_AtURCHdlEntry[i].keyword))
        {
            m_AtURCHdlEntry[i].handler(strURC, reserved);
            return;
        }
    }

    OnURCHandler_Undefined(strURC, reserved);
}

s32 Ql_RIL_IsURCStr(const char* strRsp)
{
    s32 i;
    for (i = 0; i < NUM_ELEMS(m_SysURCHdlEntry); i++)
        if (Ql_strstr(strRsp, m_SysURCHdlEntry[i].keyword)) return 1;

    for (i = 0; i < NUM_ELEMS(m_AtURCHdlEntry); i++)
        if (Ql_strstr(strRsp, m_AtURCHdlEntry[i].keyword)) return 1;

    return 0;
}

#endif  // __OCPU_RIL_SUPPORT__
