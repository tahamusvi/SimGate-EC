#ifdef __CUSTOMER_CODE__

#include "ql_type.h"
#include "ql_stdlib.h"
#include "ql_system.h"
#include "ql_timer.h"
#include "ql_gpio.h"

#include "ril.h"
#include "ril_network.h"
#include "ril_mqtt.h"

/* ================== CONFIG ================== */
#define APN       ((u8*)"mcinet")
#define USERID    ((u8*)"")
#define PASSWD    ((u8*)"")

#define MQTT_HOST ((u8*)"78.39.182.223")
#define MQTT_PORT 1883

/* یکتا کن تا برخورد client_id نداشته باشی */
#define CLIENT_ID ((u8*)"mc60_client_A")

#define PUB_TOPIC "mc60/test"

/* ================== LED ================== */
#define STATUS_LED PINNAME_RI  /* اگر LED واقعی نیست باید عوض شود */

/* ---------- Non-blocking Visual_Feedback ----------
   همان تابع شما، ولی بدون Sleep تا سیستم block نشود.
   یک صف چشمک را تنظیم می‌کند و در Blink_Tick_100ms اجرا می‌شود.
*/
static volatile u8 g_blink_steps = 0; /* counts*2 */
static volatile u8 g_blink_pos   = 0;

void Visual_Feedback(u8 counts)
{
    g_blink_steps = (u8)(counts * 2);
    g_blink_pos = 0;
}

static void Blink_Tick_100ms(void)
{
    if (g_blink_steps == 0) return;

    if ((g_blink_pos % 2) == 0)
        Ql_GPIO_SetLevel(STATUS_LED, PINLEVEL_HIGH);
    else
        Ql_GPIO_SetLevel(STATUS_LED, PINLEVEL_LOW);

    g_blink_pos++;

    if (g_blink_pos >= g_blink_steps)
    {
        g_blink_steps = 0;
        g_blink_pos = 0;
        Ql_GPIO_SetLevel(STATUS_LED, PINLEVEL_LOW);
    }
}

/* ================== AT handlers ================== */
static s32 AtRsp_OKERR(char* line, u32 len, void* userdata)
{
    if (Ql_RIL_FindLine(line, len, "OK"))    return RIL_ATRSP_SUCCESS;
    if (Ql_RIL_FindLine(line, len, "ERROR")) return RIL_ATRSP_FAILED;
    if (Ql_RIL_FindString(line, len, "+CME ERROR")) return RIL_ATRSP_FAILED;
    if (Ql_RIL_FindString(line, len, "+CMS ERROR")) return RIL_ATRSP_FAILED;
    return RIL_ATRSP_CONTINUE;
}

static void MQTT_SetKeepAlive(Enum_ConnectID id, u32 sec)
{
    char at[80];
    Ql_memset(at, 0, sizeof(at));
    Ql_sprintf(at, "AT+QMTCFG=\"KEEPALIVE\",%d,%d\r\n", (int)id, (int)sec);
    Ql_RIL_SendATCmd(at, Ql_strlen(at), AtRsp_OKERR, NULL, 0);
}

/* ================== RAW PUBLISH (بدون RIL_MQTT_QMTPUB) ================== */
typedef struct {
    const char *data; /* pointer به payload */
    u32 len;
} PUB_CTX;

static PUB_CTX g_pub_ctx;

static s32 AT_Pub_Handler(char* line, u32 len, void* userdata)
{
    PUB_CTX *ctx = (PUB_CTX*)userdata;

    /* prompt ممکنه شکل‌های مختلف داشته باشد */
    if (Ql_RIL_FindString(line, len, ">"))
    {
        /* payload را به core ارسال کن */
        Ql_RIL_WriteDataToCore((u8*)ctx->data, ctx->len);
        return RIL_ATRSP_CONTINUE;
    }

    if (Ql_RIL_FindLine(line, len, "OK"))
        return RIL_ATRSP_SUCCESS;

    if (Ql_RIL_FindLine(line, len, "ERROR") ||
        Ql_RIL_FindString(line, len, "+CME ERROR") ||
        Ql_RIL_FindString(line, len, "+CMS ERROR"))
        return RIL_ATRSP_FAILED;

    return RIL_ATRSP_CONTINUE;
}

/* qos=0, retain=1 */
static s32 MQTT_PUB_RAW(Enum_ConnectID id, u16 msgid, const char* topic, const char* payload)
{
    static char at[256];
    u32 plen = (u32)Ql_strlen(payload);

    Ql_memset(at, 0, sizeof(at));
    Ql_sprintf(at, "AT+QMTPUB=%d,%d,0,1,\"%s\",%d\r\n", (int)id, (int)msgid, topic, (int)plen);

    g_pub_ctx.data = payload;
    g_pub_ctx.len  = plen;

    return Ql_RIL_SendATCmd(at, Ql_strlen(at), AT_Pub_Handler, &g_pub_ctx, 0);
}

/* ================== STATE ================== */
static Enum_ConnectID connect_id = ConnectID_0;

enum {
    ST_IDLE = 0,
    ST_WAIT_REG,
    ST_PDP,
    ST_MQTT_CFG,
    ST_MQTT_OPEN,
    ST_MQTT_CONN,
    ST_READY
};

static volatile u8 st = ST_IDLE;
static u8 last_st = 0xFF;

static u32 tick100 = 0;
static u32 sec_tick = 0;

static u16 pub_id = 1;
static u8  pub_fail = 0;

/* ================== TIMER ================== */
#define TMR_ID  0x01
#define TMR_MS  100

static void Publish_1s(void)
{
    s32 ret;
    static char payload[64]; /* static تا تا پایان callback معتبر بماند */

    Ql_memset(payload, 0, sizeof(payload));
    Ql_sprintf(payload, "hb_%lu", (unsigned long)sec_tick);

    ret = MQTT_PUB_RAW(connect_id, pub_id, PUB_TOPIC, payload);

    pub_id++;
    if (pub_id == 0) pub_id = 1;

    if (ret == RIL_AT_SUCCESS)
    {
        pub_fail = 0;
        Visual_Feedback(1); /* publish ok */
    }
    else
    {
        pub_fail++;
        Visual_Feedback(2); /* publish fail */

        /* چند بار fail -> reconnect */
        if (pub_fail >= 5)
        {
            pub_fail = 0;
            st = ST_MQTT_OPEN;
        }
    }
}

static void StateMachine_1s(void)
{
    s32 reg = 0;
    s32 ret;

    sec_tick++;

    if (st != last_st)
    {
        last_st = st;
        switch (st)
        {
        case ST_WAIT_REG:  Visual_Feedback(1); break;
        case ST_PDP:       Visual_Feedback(2); break;
        case ST_MQTT_CFG:  Visual_Feedback(3); break;
        case ST_MQTT_OPEN: Visual_Feedback(4); break;
        case ST_MQTT_CONN: Visual_Feedback(5); break;
        case ST_READY:
            /* READY: روشن ثابت (اگر blink فعال نباشد) */
            if (g_blink_steps == 0)
                Ql_GPIO_SetLevel(STATUS_LED, PINLEVEL_HIGH);
            break;
        default:
            break;
        }
    }

    switch (st)
    {
    case ST_WAIT_REG:
        RIL_NW_GetGPRSState(&reg);
        if (reg == NW_STAT_REGISTERED || reg == NW_STAT_REGISTERED_ROAMING)
            st = ST_PDP;
        break;

    case ST_PDP:
        RIL_NW_SetGPRSContext(0);
        RIL_NW_SetAPN(1, APN, USERID, PASSWD);
        ret = RIL_NW_OpenPDPContext();
        if (ret == RIL_AT_SUCCESS)
            st = ST_MQTT_CFG;
        break;

    case ST_MQTT_CFG:
        MQTT_SetKeepAlive(connect_id, 60);
        RIL_MQTT_QMTCFG_Version_Select(connect_id, Version_3_1_1);
        st = ST_MQTT_OPEN;
        break;

    case ST_MQTT_OPEN:
        ret = RIL_MQTT_QMTOPEN(connect_id, MQTT_HOST, MQTT_PORT);
        if (ret == RIL_AT_SUCCESS)
            st = ST_MQTT_CONN;
        break;

    case ST_MQTT_CONN:
        ret = RIL_MQTT_QMTCONN(connect_id, CLIENT_ID, (u8*)"", (u8*)"");
        if (ret == RIL_AT_SUCCESS)
            st = ST_READY;
        break;

    case ST_READY:
        /* هر 1 ثانیه publish */
        Publish_1s();
        break;

    default:
        break;
    }
}

static void TimerCb(u32 id, void *param)
{
    if (id != TMR_ID) return;

    tick100++;

    /* blink هر 100ms */
    Blink_Tick_100ms();

    /* هر 1 ثانیه state machine */
    if ((tick100 % 10) == 0)
    {
        /* اگر در READY هستیم و blink نداریم، LED ثابت بماند */
        if (st == ST_READY && g_blink_steps == 0)
            Ql_GPIO_SetLevel(STATUS_LED, PINLEVEL_HIGH);

        StateMachine_1s();
    }
}

/* ================== MAIN TASK ================== */
void proc_main_task(s32 taskId)
{
    ST_MSG msg;

    Ql_GPIO_Init(STATUS_LED, PINDIRECTION_OUT, PINLEVEL_LOW, PINPULLSEL_PULLUP);
    Ql_GPIO_SetLevel(STATUS_LED, PINLEVEL_LOW);

    Ql_Timer_Register(TMR_ID, TimerCb, NULL);

    while (1)
    {
        Ql_OS_GetMessage(&msg);

        if (msg.message == MSG_ID_RIL_READY)
        {
            Ql_RIL_Initialize();
        }
        else if (msg.message == MSG_ID_URC_INDICATION)
        {
            if (msg.param1 == URC_SIM_CARD_STATE_IND && msg.param2 == SIM_STAT_READY)
            {
                st = ST_WAIT_REG;
                last_st = 0xFF;

                tick100 = 0;
                sec_tick = 0;
                pub_id = 1;
                pub_fail = 0;

                Visual_Feedback(1); /* SIM READY */

                Ql_Timer_Start(TMR_ID, TMR_MS, TRUE);
            }
        }
    }
}

#endif
