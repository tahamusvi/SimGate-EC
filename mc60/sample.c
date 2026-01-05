#ifdef __CUSTOMER_CODE__

#include "ql_type.h"
#include "ql_stdlib.h"
#include "ql_system.h"
#include "ql_timer.h"
#include "ql_gpio.h"
#include "ql_uart.h"

#include "ril.h"
#include "ril_network.h"

/* ================== CONFIG ================== */
#define DBG_UART_PORT   UART_PORT1
#define DBG_BAUD        115200
#define DBG_FLOW        FC_NONE

#define STATUS_LED      PINNAME_RI

static char g_apn[]  = "mcinet";
static char g_user[] = "";
static char g_pass[] = "";

#define MQTT_HOST       "78.39.182.223"
#define MQTT_PORT       1883
#define CLIENT_ID       "mc60_client_A"

#define TOPIC_SMS_RX    "device/MC60/sms_rx" /* module -> server */
#define TOPIC_SMS_TX    "device/MC60/sms_tx" /* server -> module */

#define HB_TOPIC        "mc60/test"

/* Publish: QoS=1 برای msgid معتبر */
#define MQTT_QOS        1
#define MQTT_RETAIN     0

/* ================== TIMER ================== */
#define TMR_ID  1
#define TMR_MS  100

/* ================== MQTT RX URC (باید مقدار واقعی را جایگزین کنی) ================== */
/* وقتی سرور publish می‌کند و URC جدید می‌بینی، p1 را اینجا بگذار */
#define URC_MQTT_RX_P1   0xFFFF

/* ================== DEBUG ================== */
static void DBG_Write(const char *s)
{
    if (!s) return;
    Ql_UART_Write(DBG_UART_PORT, (u8*)s, (u32)Ql_strlen(s));
}
static void DBG_Line(const char *s)
{
    DBG_Write(s);
    DBG_Write("\r\n");
}
static void DBG_AT(const char *tag, char *line, u32 len)
{
    if (tag) { DBG_Write(tag); DBG_Write(" "); }
    if (line && len) Ql_UART_Write(DBG_UART_PORT, (u8*)line, len);
    DBG_Write("\r\n");
}

/* ================== UART echo (اختیاری) ================== */
static void UartCb(Enum_SerialPort port, Enum_UARTEventType event, bool pinLevel, void *customizePara)
{
    (void)pinLevel; (void)customizePara;
    if (port != DBG_UART_PORT) return;

    if (event == EVENT_UART_READY_TO_READ)
    {
        u8 buf[128];
        s32 rd;
        while (1)
        {
            rd = Ql_UART_Read(port, buf, sizeof(buf));
            if (rd <= 0) break;
            Ql_UART_Write(port, buf, (u32)rd);
        }
    }
}

/* ================== small helpers ================== */
static int mem_contains(const char *buf, u32 len, const char *pat)
{
    u32 i, j, plen;
    if (!buf || !pat) return 0;
    plen = (u32)Ql_strlen(pat);
    if (plen == 0 || plen > len) return 0;

    for (i = 0; i + plen <= len; i++)
    {
        for (j = 0; j < plen; j++)
            if (buf[i + j] != pat[j]) break;
        if (j == plen) return 1;
    }
    return 0;
}

static void copy_line_trim(char *dst, u32 dstsz, char *line, u32 len)
{
    u32 i = 0, n = 0;
    if (!dst || dstsz == 0) return;
    dst[0] = '\0';
    if (!line || len == 0) return;

    while (i < len && (line[i] == '\r' || line[i] == '\n')) i++;

    for (; i < len && n < dstsz - 1; i++)
    {
        char c = line[i];
        if (c == '\r' || c == '\n') break;
        dst[n++] = c;
    }
    dst[n] = '\0';
}

static int is_hex_char(char c)
{
    return ((c >= '0' && c <= '9') ||
            (c >= 'a' && c <= 'f') ||
            (c >= 'A' && c <= 'F'));
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return (c - '0');
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return 0;
}

static int looks_like_ucs2_hex(const char *s)
{
    u32 i, L;
    if (!s) return 0;
    L = (u32)Ql_strlen(s);
    if (L == 0 || (L % 4) != 0) return 0;
    for (i = 0; i < L; i++) if (!is_hex_char(s[i])) return 0;
    return 1;
}

static void ucs2_hex_to_ascii(const char *ucs2hex, char *out, u32 outsz, int *had_non_ascii)
{
    u32 i = 0, o = 0, L;
    if (had_non_ascii) *had_non_ascii = 0;
    if (!out || outsz == 0) return;
    out[0] = '\0';
    if (!ucs2hex) return;

    L = (u32)Ql_strlen(ucs2hex);
    while (i + 3 < L && o + 1 < outsz)
    {
        int v = (hex_val(ucs2hex[i]) << 12) |
                (hex_val(ucs2hex[i+1]) << 8) |
                (hex_val(ucs2hex[i+2]) << 4) |
                (hex_val(ucs2hex[i+3]) << 0);

        if (v <= 0x7F) out[o++] = (char)v;
        else { if (had_non_ascii) *had_non_ascii = 1; }
        i += 4;
    }
    out[o] = '\0';
}

static void ascii_to_ucs2_hex(const char *in, char *out, u32 outsz)
{
    static const char *hx = "0123456789ABCDEF";
    u32 i = 0, o = 0;
    if (!in || !out || outsz < 5) return;
    out[0] = '\0';

    while (in[i] && (o + 4) < outsz)
    {
        u8 ch = (u8)in[i++];
        out[o++] = '0';
        out[o++] = '0';
        out[o++] = hx[(ch >> 4) & 0x0F];
        out[o++] = hx[ch & 0x0F];
    }
    out[o] = '\0';
}

/* ================== Global AT busy ================== */
static volatile u8 g_at_busy = 0;

/* ================== STATE ================== */
typedef enum {
    ST_IDLE = 0,
    ST_WAIT_REG,
    ST_PDP,
    ST_MQTT_CFG,
    ST_MQTT_OPEN,
    ST_MQTT_CONN,
    ST_MQTT_SUB,
    ST_READY
} APP_STATE;

static volatile APP_STATE g_state = ST_IDLE;
static APP_STATE g_last_state = (APP_STATE)0xFF;

static u32 g_tick100 = 0;
static u32 g_sec = 0;

static u8  g_mqtt_connected = 0;
static u8  g_mqtt_subscribed = 0;

static u16 g_mqtt_msgid = 1;
static u8  g_hb_due = 0;

/* ================== Queues ================== */
#define SMS_IDX_Q_MAX  8
static u16 g_sms_idx_q[SMS_IDX_Q_MAX];
static u8  g_sms_idx_w = 0, g_sms_idx_r = 0;

static void sms_idx_push(u16 idx)
{
    u8 n = (u8)((g_sms_idx_w + 1) % SMS_IDX_Q_MAX);
    if (n == g_sms_idx_r) return;
    g_sms_idx_q[g_sms_idx_w] = idx;
    g_sms_idx_w = n;
}
static int sms_idx_pop(u16 *out)
{
    if (g_sms_idx_r == g_sms_idx_w) return 0;
    *out = g_sms_idx_q[g_sms_idx_r];
    g_sms_idx_r = (u8)((g_sms_idx_r + 1) % SMS_IDX_Q_MAX);
    return 1;
}

#define PUB_Q_MAX  6
#define PUB_STR_MAX  360
static char g_pub_q[PUB_Q_MAX][PUB_STR_MAX];
static u8 g_pub_w = 0, g_pub_r = 0;

static void pubq_push(const char *s)
{
    u8 n = (u8)((g_pub_w + 1) % PUB_Q_MAX);
    if (n == g_pub_r) { DBG_Line("[PUBQ] full drop"); return; }
    Ql_memset(g_pub_q[g_pub_w], 0, PUB_STR_MAX);
    Ql_strncpy(g_pub_q[g_pub_w], s, PUB_STR_MAX - 1);
    g_pub_w = n;
}
static const char* pubq_peek(void)
{
    if (g_pub_r == g_pub_w) return NULL;
    return g_pub_q[g_pub_r];
}
static void pubq_pop(void)
{
    if (g_pub_r == g_pub_w) return;
    g_pub_r = (u8)((g_pub_r + 1) % PUB_Q_MAX);
}

#define SMS_TX_Q_MAX  5
typedef struct {
    char phone[32];
    char msg[256];
    u8  msg_is_ucs2_hex;
} SMS_TX_ITEM;

static SMS_TX_ITEM g_sms_tx_q[SMS_TX_Q_MAX];
static u8 g_sms_tx_w = 0, g_sms_tx_r = 0;

static void sms_tx_push(const char *phone, const char *msg, u8 is_hex)
{
    u8 n = (u8)((g_sms_tx_w + 1) % SMS_TX_Q_MAX);
    if (n == g_sms_tx_r) { DBG_Line("[SMS_TX] full drop"); return; }
    Ql_memset(&g_sms_tx_q[g_sms_tx_w], 0, sizeof(SMS_TX_ITEM));
    Ql_strncpy(g_sms_tx_q[g_sms_tx_w].phone, phone, sizeof(g_sms_tx_q[g_sms_tx_w].phone)-1);
    Ql_strncpy(g_sms_tx_q[g_sms_tx_w].msg, msg, sizeof(g_sms_tx_q[g_sms_tx_w].msg)-1);
    g_sms_tx_q[g_sms_tx_w].msg_is_ucs2_hex = is_hex;
    g_sms_tx_w = n;
}
static int sms_tx_pop(SMS_TX_ITEM *out)
{
    if (g_sms_tx_r == g_sms_tx_w) return 0;
    *out = g_sms_tx_q[g_sms_tx_r];
    return 1;
}
static void sms_tx_pop_commit(void)
{
    if (g_sms_tx_r == g_sms_tx_w) return;
    g_sms_tx_r = (u8)((g_sms_tx_r + 1) % SMS_TX_Q_MAX);
}

/* ================== MQTT handlers ================== */
typedef struct { const char *data; u32 len; } PUB_CTX;
static PUB_CTX g_pub_ctx;

static s32 AT_QMTPUB_Handler(char* line, u32 len, void* userdata)
{
    PUB_CTX *ctx = (PUB_CTX*)userdata;
    DBG_AT("[QMTPUB]", line, len);

    if (mem_contains(line, len, ">"))
    {
        Ql_RIL_WriteDataToCore((u8*)ctx->data, ctx->len);
        return RIL_ATRSP_CONTINUE;
    }

    if (mem_contains(line, len, "OK")) { g_at_busy = 0; return RIL_ATRSP_SUCCESS; }
    if (mem_contains(line, len, "ERROR") || mem_contains(line, len, "+CME ERROR") || mem_contains(line, len, "+CMS ERROR"))
    { g_at_busy = 0; return RIL_ATRSP_FAILED; }

    return RIL_ATRSP_CONTINUE;
}

static s32 MQTT_PUB_FIXEDLEN(int cid, u16 msgid, const char *topic, const char *payload)
{
    char at[280];
    u32 plen = (u32)Ql_strlen(payload);
    if (g_at_busy) return RIL_AT_BUSY;

    Ql_memset(at, 0, sizeof(at));
    Ql_sprintf(at, "AT+QMTPUB=%d,%d,%d,%d,\"%s\",%d\r\n",
               cid, (int)msgid, MQTT_QOS, MQTT_RETAIN, topic, (int)plen);

    g_pub_ctx.data = payload;
    g_pub_ctx.len = plen;

    DBG_Line("==> SEND QMTPUB");
    g_at_busy = 1;
    return Ql_RIL_SendATCmd(at, Ql_strlen(at), AT_QMTPUB_Handler, &g_pub_ctx, 0);
}

/* open/conn/sub need to read +QMTxxx */
static volatile u8 g_open_ok = 0, g_conn_ok = 0, g_sub_ok = 0;

static s32 AT_QMTOPEN_Handler(char* line, u32 len, void* userdata)
{
    (void)userdata;
    DBG_AT("[QMTOPEN]", line, len);

    if (mem_contains(line, len, "+QMTOPEN:"))
    {
        char tmp[120];
        int cid=-1, res=-1;
        copy_line_trim(tmp, sizeof(tmp), line, len);
        if (2 == Ql_sscanf(tmp, "+QMTOPEN: %d,%d", &cid, &res)) g_open_ok = (res==0);
        g_at_busy = 0;
        return g_open_ok ? RIL_ATRSP_SUCCESS : RIL_ATRSP_FAILED;
    }
    if (mem_contains(line, len, "ERROR") || mem_contains(line, len, "+CME ERROR") || mem_contains(line, len, "+CMS ERROR"))
    { g_open_ok = 0; g_at_busy = 0; return RIL_ATRSP_FAILED; }
    return RIL_ATRSP_CONTINUE;
}

static s32 AT_QMTCONN_Handler(char* line, u32 len, void* userdata)
{
    (void)userdata;
    DBG_AT("[QMTCONN]", line, len);

    if (mem_contains(line, len, "+QMTCONN:"))
    {
        char tmp[140];
        int cid=-1, res=-1, ack=-1;
        copy_line_trim(tmp, sizeof(tmp), line, len);
        if (3 == Ql_sscanf(tmp, "+QMTCONN: %d,%d,%d", &cid, &res, &ack))
            g_conn_ok = (res==0 && ack==0);
        g_at_busy = 0;
        return g_conn_ok ? RIL_ATRSP_SUCCESS : RIL_ATRSP_FAILED;
    }
    if (mem_contains(line, len, "ERROR") || mem_contains(line, len, "+CME ERROR") || mem_contains(line, len, "+CMS ERROR"))
    { g_conn_ok = 0; g_at_busy = 0; return RIL_ATRSP_FAILED; }
    return RIL_ATRSP_CONTINUE;
}

static s32 AT_QMTSUB_Handler(char* line, u32 len, void* userdata)
{
    (void)userdata;
    DBG_AT("[QMTSUB]", line, len);

    if (mem_contains(line, len, "+QMTSUB:"))
    {
        char tmp[140];
        int cid=-1, mid=-1, res=-1, qos=-1;
        copy_line_trim(tmp, sizeof(tmp), line, len);
        if (4 == Ql_sscanf(tmp, "+QMTSUB: %d,%d,%d,%d", &cid, &mid, &res, &qos)) g_sub_ok = (res==0);
        g_at_busy = 0;
        return g_sub_ok ? RIL_ATRSP_SUCCESS : RIL_ATRSP_FAILED;
    }
    if (mem_contains(line, len, "ERROR") || mem_contains(line, len, "+CME ERROR") || mem_contains(line, len, "+CMS ERROR"))
    { g_sub_ok = 0; g_at_busy = 0; return RIL_ATRSP_FAILED; }
    return RIL_ATRSP_CONTINUE;
}

static s32 MQTT_OPEN(int cid)
{
    char at[200];
    if (g_at_busy) return RIL_AT_BUSY;
    Ql_memset(at, 0, sizeof(at));
    Ql_sprintf(at, "AT+QMTOPEN=%d,\"%s\",%d\r\n", cid, MQTT_HOST, MQTT_PORT);
    DBG_Line("==> SEND QMTOPEN");
    g_at_busy = 1;
    return Ql_RIL_SendATCmd(at, Ql_strlen(at), AT_QMTOPEN_Handler, NULL, 0);
}

static s32 MQTT_CONN(int cid)
{
    char at[200];
    if (g_at_busy) return RIL_AT_BUSY;
    Ql_memset(at, 0, sizeof(at));
    Ql_sprintf(at, "AT+QMTCONN=%d,\"%s\"\r\n", cid, CLIENT_ID);
    DBG_Line("==> SEND QMTCONN");
    g_at_busy = 1;
    return Ql_RIL_SendATCmd(at, Ql_strlen(at), AT_QMTCONN_Handler, NULL, 0);
}

static s32 MQTT_SUB(int cid, u16 msgid, const char *topic, int qos)
{
    char at[240];
    if (g_at_busy) return RIL_AT_BUSY;
    Ql_memset(at, 0, sizeof(at));
    Ql_sprintf(at, "AT+QMTSUB=%d,%d,\"%s\",%d\r\n", cid, (int)msgid, topic, qos);
    DBG_Line("==> SEND QMTSUB");
    g_at_busy = 1;
    return Ql_RIL_SendATCmd(at, Ql_strlen(at), AT_QMTSUB_Handler, NULL, 0);
}

static void MQTT_CFG_Once(void)
{
    Ql_RIL_SendATCmd("AT+QMTCFG=\"KEEPALIVE\",0,60\r\n", Ql_strlen("AT+QMTCFG=\"KEEPALIVE\",0,60\r\n"), NULL, NULL, 0);
    Ql_RIL_SendATCmd("AT+QMTCFG=\"VERSION\",0,4\r\n",     Ql_strlen("AT+QMTCFG=\"VERSION\",0,4\r\n"),     NULL, NULL, 0);
    Ql_RIL_SendATCmd("AT+QMTCFG=\"SHOWRECVLEN\",0,1\r\n", Ql_strlen("AT+QMTCFG=\"SHOWRECVLEN\",0,1\r\n"), NULL, NULL, 0);
}

/* ================== QMTRECV READ ================== */
typedef struct {
    char topic[128];
    char payload[256];
    u8 got;
} QMTRECV_CTX;

static QMTRECV_CTX g_qmtr;

static void Handle_ServerCmd_ToSMS(const char *payload)
{
    /* payload format: phone:msg */
    char phone[40];
    char msg[256];
    char *c;

    Ql_memset(phone, 0, sizeof(phone));
    Ql_memset(msg, 0, sizeof(msg));

    if (!payload) return;
    c = Ql_strchr((char*)payload, ':');
    if (!c) return;

    Ql_strncpy(phone, payload, (u32)(c - payload));
    Ql_strncpy(msg, c + 1, sizeof(msg) - 1);

    if (phone[0] == 0 || msg[0] == 0) return;

    /* اگر UCS2 hex بود: طول %4==0 و همه hex */
    if (looks_like_ucs2_hex(msg))
        sms_tx_push(phone, msg, 1);
    else
        sms_tx_push(phone, msg, 0);

    DBG_Line("[MQTT] sms_tx enqueued");
}

static s32 AT_QMTRECV_Handler(char* line, u32 len, void* userdata)
{
    QMTRECV_CTX *ctx = (QMTRECV_CTX*)userdata;
    DBG_AT("[QMTRECV]", line, len);

    if (mem_contains(line, len, "+QMTRECV:"))
    {
        /* +QMTRECV: 0,1,"topic","payload"
           یا با SHOWRECVLEN ممکنه یک عدد length هم وسط باشد، ما ساده‌ترین حالت را parse می‌کنیم */
        char tmp[420];
        char *q1, *q2, *q3, *q4;

        copy_line_trim(tmp, sizeof(tmp), line, len);

        q1 = Ql_strchr(tmp, '"'); if (!q1) return RIL_ATRSP_CONTINUE;
        q2 = Ql_strchr(q1 + 1, '"'); if (!q2) return RIL_ATRSP_CONTINUE;

        q3 = Ql_strchr(q2 + 1, '"'); if (!q3) return RIL_ATRSP_CONTINUE;
        q4 = Ql_strchr(q3 + 1, '"'); if (!q4) return RIL_ATRSP_CONTINUE;

        Ql_memset(ctx->topic, 0, sizeof(ctx->topic));
        Ql_memset(ctx->payload, 0, sizeof(ctx->payload));

        Ql_strncpy(ctx->topic, q1 + 1, (u32)(q2 - (q1 + 1)));
        Ql_strncpy(ctx->payload, q3 + 1, (u32)(q4 - (q3 + 1)));
        ctx->got = 1;

        return RIL_ATRSP_CONTINUE;
    }

    if (mem_contains(line, len, "OK")) { g_at_busy = 0; return RIL_ATRSP_SUCCESS; }
    if (mem_contains(line, len, "ERROR") || mem_contains(line, len, "+CME ERROR") || mem_contains(line, len, "+CMS ERROR"))
    { g_at_busy = 0; return RIL_ATRSP_FAILED; }

    return RIL_ATRSP_CONTINUE;
}

static s32 MQTT_ReadOneMsg(u16 msgid)
{
    char at[80];
    if (g_at_busy) return RIL_AT_BUSY;

    Ql_memset(&g_qmtr, 0, sizeof(g_qmtr));
    Ql_memset(at, 0, sizeof(at));
    Ql_sprintf(at, "AT+QMTRECV=0,%d\r\n", (int)msgid);

    DBG_Line("==> SEND QMTRECV(read)");
    g_at_busy = 1;
    return Ql_RIL_SendATCmd(at, Ql_strlen(at), AT_QMTRECV_Handler, &g_qmtr, 0);
}

/* ================== SMS: CMGR/CMGD/CMGS ================== */
static volatile u16 g_sms_delete_idx = 0;

typedef struct {
    u16 idx;
    u8  got_hdr;
    u8  got_msg;
    char sender_raw[96];
    char msg_raw[280];
} CMGR_CTX;

static CMGR_CTX g_cmgr;

static int parse_second_quoted(const char *s, char *out, u32 outsz)
{
    const char *p = s;
    int q = 0;
    const char *start = NULL;
    if (!s || !out || outsz == 0) return -1;
    out[0] = '\0';

    while (*p)
    {
        if (*p == '"')
        {
            q++;
            if (q == 3) start = p + 1;
            else if (q == 4 && start)
            {
                u32 n = (u32)(p - start);
                if (n >= outsz) n = outsz - 1;
                Ql_memcpy(out, start, n);
                out[n] = '\0';
                return 0;
            }
        }
        p++;
    }
    return -1;
}

static s32 AT_CMGR_Handler(char* line, u32 len, void* userdata)
{
    CMGR_CTX *ctx = (CMGR_CTX*)userdata;
    DBG_AT("[CMGR]", line, len);

    if (mem_contains(line, len, "+CMGR:"))
    {
        char tmp[220];
        copy_line_trim(tmp, sizeof(tmp), line, len);

        Ql_memset(ctx->sender_raw, 0, sizeof(ctx->sender_raw));
        if (parse_second_quoted(tmp, ctx->sender_raw, sizeof(ctx->sender_raw)) == 0)
            ctx->got_hdr = 1;

        return RIL_ATRSP_CONTINUE;
    }

    if (ctx->got_hdr && !ctx->got_msg)
    {
        char msgline[300];
        copy_line_trim(msgline, sizeof(msgline), line, len);

        if (msgline[0] != '\0' &&
            !Ql_strstr(msgline, "OK") &&
            !Ql_strstr(msgline, "ERROR") &&
            !Ql_strstr(msgline, "+CME ERROR") &&
            !Ql_strstr(msgline, "+CMS ERROR"))
        {
            Ql_memset(ctx->msg_raw, 0, sizeof(ctx->msg_raw));
            Ql_strncpy(ctx->msg_raw, msgline, sizeof(ctx->msg_raw)-1);
            ctx->got_msg = 1;
        }
    }

    if (mem_contains(line, len, "OK"))
    {
        g_at_busy = 0;
        return RIL_ATRSP_SUCCESS;
    }
    if (mem_contains(line, len, "ERROR") || mem_contains(line, len, "+CME ERROR") || mem_contains(line, len, "+CMS ERROR"))
    {
        g_at_busy = 0;
        return RIL_ATRSP_FAILED;
    }

    return RIL_ATRSP_CONTINUE;
}

static s32 SMS_CMGR(u16 idx)
{
    char at[40];
    if (g_at_busy) return RIL_AT_BUSY;

    Ql_memset(&g_cmgr, 0, sizeof(g_cmgr));
    g_cmgr.idx = idx;

    Ql_memset(at, 0, sizeof(at));
    Ql_sprintf(at, "AT+CMGR=%d\r\n", (int)idx);

    DBG_Line("==> SEND CMGR");
    g_at_busy = 1;
    return Ql_RIL_SendATCmd(at, Ql_strlen(at), AT_CMGR_Handler, &g_cmgr, 0);
}

static s32 AT_CMGD_Handler(char* line, u32 len, void* userdata)
{
    (void)userdata;
    DBG_AT("[CMGD]", line, len);

    if (mem_contains(line, len, "OK")) { g_at_busy = 0; return RIL_ATRSP_SUCCESS; }
    if (mem_contains(line, len, "ERROR") || mem_contains(line, len, "+CME ERROR") || mem_contains(line, len, "+CMS ERROR"))
    { g_at_busy = 0; return RIL_ATRSP_FAILED; }

    return RIL_ATRSP_CONTINUE;
}

static s32 SMS_CMGD(u16 idx)
{
    char at[40];
    if (g_at_busy) return RIL_AT_BUSY;

    Ql_memset(at, 0, sizeof(at));
    Ql_sprintf(at, "AT+CMGD=%d\r\n", (int)idx);

    DBG_Line("==> SEND CMGD");
    g_at_busy = 1;
    return Ql_RIL_SendATCmd(at, Ql_strlen(at), AT_CMGD_Handler, NULL, 0);
}

/* ---- CMGS (always UCS2) ---- */
typedef struct {
    char phone_ucs2[96];
    char msg_ucs2[520];
    u32  msg_len;
} CMGS_CTX;

static CMGS_CTX g_cmgs;

static s32 AT_CMGS_Handler(char* line, u32 len, void* userdata)
{
    CMGS_CTX *ctx = (CMGS_CTX*)userdata;
    DBG_AT("[CMGS]", line, len);

    if (mem_contains(line, len, ">"))
    {
        Ql_RIL_WriteDataToCore((u8*)ctx->msg_ucs2, ctx->msg_len);
        { u8 cz = 0x1A; Ql_RIL_WriteDataToCore(&cz, 1); }
        return RIL_ATRSP_CONTINUE;
    }

    if (mem_contains(line, len, "OK")) { g_at_busy = 0; return RIL_ATRSP_SUCCESS; }
    if (mem_contains(line, len, "ERROR") || mem_contains(line, len, "+CME ERROR") || mem_contains(line, len, "+CMS ERROR"))
    { g_at_busy = 0; return RIL_ATRSP_FAILED; }

    return RIL_ATRSP_CONTINUE;
}

static s32 SMS_Send_UCS2(const char *phone_ascii, const char *msg_ascii_or_hex, u8 msg_is_hex_ucs2)
{
    char at[140];
    if (g_at_busy) return RIL_AT_BUSY;
    if (!phone_ascii || !msg_ascii_or_hex) return RIL_AT_INVALID_PARAM;

    /* ensure SMS settings */
    Ql_RIL_SendATCmd("AT+CMGF=1\r\n", Ql_strlen("AT+CMGF=1\r\n"), NULL, NULL, 0);
    Ql_RIL_SendATCmd("AT+CSCS=\"UCS2\"\r\n", Ql_strlen("AT+CSCS=\"UCS2\"\r\n"), NULL, NULL, 0);
    Ql_RIL_SendATCmd("AT+CSMP=17,167,0,8\r\n", Ql_strlen("AT+CSMP=17,167,0,8\r\n"), NULL, NULL, 0);

    Ql_memset(&g_cmgs, 0, sizeof(g_cmgs));
    ascii_to_ucs2_hex(phone_ascii, g_cmgs.phone_ucs2, sizeof(g_cmgs.phone_ucs2));

    if (msg_is_hex_ucs2)
        Ql_strncpy(g_cmgs.msg_ucs2, msg_ascii_or_hex, sizeof(g_cmgs.msg_ucs2)-1);
    else
        ascii_to_ucs2_hex(msg_ascii_or_hex, g_cmgs.msg_ucs2, sizeof(g_cmgs.msg_ucs2));

    g_cmgs.msg_len = (u32)Ql_strlen(g_cmgs.msg_ucs2);

    Ql_memset(at, 0, sizeof(at));
    Ql_sprintf(at, "AT+CMGS=\"%s\"\r\n", g_cmgs.phone_ucs2);

    DBG_Line("==> SEND CMGS");
    g_at_busy = 1;
    return Ql_RIL_SendATCmd(at, Ql_strlen(at), AT_CMGS_Handler, &g_cmgs, 0);
}

/* ================== Build SMS->MQTT payload ================== */
static void sms_prepare_payload_and_enqueue(const char *sender_raw, const char *msg_raw)
{
    char sender_dec[64];
    char msg_dec[280];
    char out[360];
    int non_ascii_sender = 0;
    int non_ascii_msg = 0;

    Ql_memset(sender_dec, 0, sizeof(sender_dec));
    Ql_memset(msg_dec, 0, sizeof(msg_dec));
    Ql_memset(out, 0, sizeof(out));

    if (looks_like_ucs2_hex(sender_raw))
    {
        ucs2_hex_to_ascii(sender_raw, sender_dec, sizeof(sender_dec), &non_ascii_sender);
        if (non_ascii_sender || sender_dec[0] == '\0')
            Ql_strncpy(sender_dec, sender_raw, sizeof(sender_dec)-1);
    }
    else
    {
        Ql_strncpy(sender_dec, sender_raw, sizeof(sender_dec)-1);
    }

    if (looks_like_ucs2_hex(msg_raw))
    {
        ucs2_hex_to_ascii(msg_raw, msg_dec, sizeof(msg_dec), &non_ascii_msg);
        if (non_ascii_msg || msg_dec[0] == '\0')
            Ql_strncpy(msg_dec, msg_raw, sizeof(msg_dec)-1);
    }
    else
    {
        Ql_strncpy(msg_dec, msg_raw, sizeof(msg_dec)-1);
    }

    Ql_sprintf(out, "%s:%s", sender_dec, msg_dec);
    pubq_push(out);
}

/* ================== READY pump ================== */
static void Pump_Ready(void)
{
    int cid = 0;
    SMS_TX_ITEM it;
    u16 idx;

    if (!g_mqtt_connected) return;

    /* 1) send SMS from server */
    if (!g_at_busy && sms_tx_pop(&it))
    {
        (void)SMS_Send_UCS2(it.phone, it.msg, it.msg_is_ucs2_hex);
        sms_tx_pop_commit();
        return;
    }

    /* 2) delete processed SMS */
    if (!g_at_busy && g_sms_delete_idx != 0)
    {
        (void)SMS_CMGD(g_sms_delete_idx);
        g_sms_delete_idx = 0;
        return;
    }

    /* 3) read incoming SMS */
    if (!g_at_busy && sms_idx_pop(&idx))
    {
        (void)SMS_CMGR(idx);
        return;
    }

    /* 4) publish queued SMS payloads */
    if (!g_at_busy)
    {
        const char *p = pubq_peek();
        if (p)
        {
            (void)MQTT_PUB_FIXEDLEN(cid, g_mqtt_msgid++, TOPIC_SMS_RX, p);
            if (g_mqtt_msgid == 0) g_mqtt_msgid = 1;
            pubq_pop();
            return;
        }
    }

    /* 5) heartbeat */
    if (!g_at_busy && g_hb_due)
    {
        static char hb[64];
        Ql_memset(hb, 0, sizeof(hb));
        Ql_sprintf(hb, "hb_%lu", (unsigned long)g_sec);
        (void)MQTT_PUB_FIXEDLEN(cid, g_mqtt_msgid++, HB_TOPIC, hb);
        if (g_mqtt_msgid == 0) g_mqtt_msgid = 1;
        g_hb_due = 0;
    }
}

/* ================== State machine ================== */
static void StateMachine_1s(void)
{
    s32 reg = 0;
    s32 ret;

    g_sec++;
    g_hb_due = 1;

    if (g_state != g_last_state)
    {
        g_last_state = g_state;
        DBG_Write("\r\n====================\r\nSTATE -> ");
        switch (g_state)
        {
        case ST_WAIT_REG:  DBG_Line("WAIT_REG"); break;
        case ST_PDP:       DBG_Line("PDP"); break;
        case ST_MQTT_CFG:  DBG_Line("MQTT_CFG"); break;
        case ST_MQTT_OPEN: DBG_Line("MQTT_OPEN"); break;
        case ST_MQTT_CONN: DBG_Line("MQTT_CONN"); break;
        case ST_MQTT_SUB:  DBG_Line("MQTT_SUB"); break;
        case ST_READY:     DBG_Line("READY"); break;
        default:           DBG_Line("IDLE"); break;
        }
    }

    switch (g_state)
    {
    case ST_WAIT_REG:
        RIL_NW_GetGPRSState(&reg);
        if (reg == NW_STAT_REGISTERED || reg == NW_STAT_REGISTERED_ROAMING)
        {
            DBG_Line("[NET] REGISTERED OK");
            g_state = ST_PDP;
        }
        else DBG_Line("[NET] not registered yet...");
        break;

    case ST_PDP:
        if (!g_at_busy)
        {
            RIL_NW_SetGPRSContext(0);
            RIL_NW_SetAPN(1, g_apn, g_user, g_pass);
            ret = RIL_NW_OpenPDPContext();
            if (ret == RIL_AT_SUCCESS) { DBG_Line("[PDP] OK"); g_state = ST_MQTT_CFG; }
        }
        break;

    case ST_MQTT_CFG:
        MQTT_CFG_Once();
        g_open_ok = g_conn_ok = g_sub_ok = 0;
        g_mqtt_connected = 0;
        g_mqtt_subscribed = 0;
        g_state = ST_MQTT_OPEN;
        break;

    case ST_MQTT_OPEN:
        if (!g_at_busy)
        {
            if (!g_open_ok) (void)MQTT_OPEN(0);
            else g_state = ST_MQTT_CONN;
        }
        break;

    case ST_MQTT_CONN:
        if (!g_at_busy)
        {
            if (!g_conn_ok) (void)MQTT_CONN(0);
            else
            {
                g_mqtt_connected = 1;
                Ql_GPIO_SetLevel(STATUS_LED, PINLEVEL_HIGH);
                DBG_Line("[MQTT] CONNECTED");
                g_state = ST_MQTT_SUB;
            }
        }
        break;

    case ST_MQTT_SUB:
        if (!g_at_busy)
        {
            if (!g_sub_ok) (void)MQTT_SUB(0, 100, TOPIC_SMS_TX, 1);
            else
            {
                g_mqtt_subscribed = 1;
                DBG_Line("[MQTT] SUBSCRIBED sms_tx");
                g_state = ST_READY;
            }
        }
        break;

    default:
        break;
    }
}

/* ================== Timer ================== */
static void TimerCb(u32 id, void *param)
{
    (void)param;
    if (id != TMR_ID) return;

    g_tick100++;

    if ((g_tick100 % 10) == 0)
        StateMachine_1s();

    if (g_state == ST_READY)
        Pump_Ready();
}

/* ================== MAIN ================== */
void proc_main_task(s32 taskId)
{
    ST_MSG msg;
    (void)taskId;

    Ql_GPIO_Init(STATUS_LED, PINDIRECTION_OUT, PINLEVEL_LOW, PINPULLSEL_DISABLE);
    Ql_GPIO_SetLevel(STATUS_LED, PINLEVEL_LOW);

    Ql_UART_Register(DBG_UART_PORT, UartCb, NULL);
    Ql_UART_Open(DBG_UART_PORT, DBG_BAUD, DBG_FLOW);
    DBG_Line("=== MC60 APP (NO RegisterURC) ===");

    Ql_Timer_Register(TMR_ID, TimerCb, NULL);

    while (1)
    {
        Ql_OS_GetMessage(&msg);

        if (msg.message == MSG_ID_RIL_READY)
        {
            DBG_Line("[SYS] RIL READY -> Initialize");
            Ql_RIL_Initialize();
        }
        else if (msg.message == MSG_ID_URC_INDICATION)
        {
            /* log URC */
            {
                char b[64];
                Ql_memset(b, 0, sizeof(b));
                Ql_sprintf(b, "[URC] p1=%lu p2=%lu\r\n",
                           (unsigned long)msg.param1, (unsigned long)msg.param2);
                DBG_Write(b);
            }

            /* SIM ready */
            if (msg.param1 == URC_SIM_CARD_STATE_IND && msg.param2 == SIM_STAT_READY)
            {
                DBG_Line("[SYS] SIM READY");

                /* SMS config */
                Ql_RIL_SendATCmd("AT+CMEE=2\r\n", Ql_strlen("AT+CMEE=2\r\n"), NULL, NULL, 0);
                Ql_RIL_SendATCmd("AT+CMGF=1\r\n", Ql_strlen("AT+CMGF=1\r\n"), NULL, NULL, 0);
                Ql_RIL_SendATCmd("AT+CSCS=\"UCS2\"\r\n", Ql_strlen("AT+CSCS=\"UCS2\"\r\n"), NULL, NULL, 0);
                Ql_RIL_SendATCmd("AT+CSMP=17,167,0,8\r\n", Ql_strlen("AT+CSMP=17,167,0,8\r\n"), NULL, NULL, 0);
                Ql_RIL_SendATCmd("AT+CNMI=2,1,0,0,0\r\n", Ql_strlen("AT+CNMI=2,1,0,0,0\r\n"), NULL, NULL, 0);

                g_state = ST_WAIT_REG;
                g_last_state = (APP_STATE)0xFF;

                g_tick100 = 0;
                g_sec = 0;
                g_mqtt_msgid = 1;
                g_hb_due = 0;

                g_open_ok = g_conn_ok = g_sub_ok = 0;
                g_mqtt_connected = 0;
                g_mqtt_subscribed = 0;

                Ql_Timer_Start(TMR_ID, TMR_MS, TRUE);
            }

            /* new sms */
            if (msg.param1 == URC_NEW_SMS_IND)
            {
                DBG_Line("[SMS] NEW SMS IND");
                sms_idx_push((u16)msg.param2);
            }

            /* mqtt rx urc -> read message */
            if (msg.param1 == URC_MQTT_RX_P1)
            {
                DBG_Line("[MQTT] RX URC -> read QMTRECV");
                (void)MQTT_ReadOneMsg((u16)msg.param2);

                if (!g_at_busy && g_qmtr.got)
                {
                    if (0 == Ql_strcmp(g_qmtr.topic, TOPIC_SMS_TX))
                        Handle_ServerCmd_ToSMS(g_qmtr.payload);
                }
            }
        }

        /* after CMGR done: build payload + delete */
        if (!g_at_busy && g_cmgr.idx != 0 && g_cmgr.got_hdr)
        {
            if (g_cmgr.got_msg)
                sms_prepare_payload_and_enqueue(g_cmgr.sender_raw, g_cmgr.msg_raw);

            g_sms_delete_idx = g_cmgr.idx;
            g_cmgr.idx = 0;
        }
    }
}

#endif /* __CUSTOMER_CODE__ */