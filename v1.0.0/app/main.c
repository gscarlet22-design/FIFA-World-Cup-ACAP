#define _GNU_SOURCE
/*
 * fifa_wc — FIFA World Cup 2026 Live Ticker
 * AXIS ACAP Native SDK  —  Axis C1720 / C1710
 * v1.0.8  gscarlet22 design  (Sprint 6: per-team audio & display overrides)
 *
 * Dual-API strategy:
 *   Primary:  api-football   (v3.football.api-sports.io)
 *   Fallback: football-data.org (api.football-data.org/v4)
 *   Demo:     local mock JSON files — no live HTTP calls
 *
 * Rate limit: MIN_POLL_SEC (180 s) hard minimum between API refresh cycles.
 * The display thread cycles the message queue continuously from cached data
 * between polls, so the ticker never goes blank.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <errno.h>
#include <ctype.h>
#include <curl/curl.h>
#include "cJSON.h"
#include <axsdk/axparameter.h>

/* minimp3: single-header pure-C MP3 decoder — no new link deps.
   minimp3_ex.h provides mp3dec_file_info_t / mp3dec_load_buf and
   pulls in minimp3.h internally. */
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#include "minimp3_ex.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* ── Constants ────────────────────────────────────────────────── */
#define APP_NAME        "fifa_wc"
#define APP_VER         "1.0.8"
#define HTTP_PORT       2016
#define MIN_POLL_SEC    180           /* 3-minute hard floor on API calls     */
#define STD_POLL_SEC    180
#define MAX_MATCHES     64
#define N_GROUPS        12
#define GPG             4             /* teams per group                      */
#define MAX_SCORERS     10
#define MAX_SEL         48
#define MAX_DISP_MSG    24
#define MSG_SZ          320
#define JBUF            (128*1024)
#define CURL_TO         8L
#define MOCK_DIR        "/usr/local/packages/fifa_wc/mock"
#define DISP_API        "https://127.0.0.1/config/rest/speaker-display-notification/v1/simple"
#define DISP_STOP       "https://127.0.0.1/config/rest/speaker-display-notification/v1/stop"
#define MEDIACLIP_API   "http://127.0.0.1/axis-cgi/mediaclip.cgi?action=play&clip=%d"
#define INSTALL_DIR     "/usr/local/packages/" APP_NAME "/"
#define MAX_URL         512
#define MAX_BUF         (512 * 1024)
#define AF_BASE         "https://v3.football.api-sports.io"
#define FD_BASE         "https://api.football-data.org/v4"

#define LOG(fmt,...) do{ \
    time_t _t=time(NULL);char _b[16]; \
    struct tm *_m=localtime(&_t); \
    strftime(_b,sizeof(_b),"%H:%M:%S",_m); \
    fprintf(stderr,"[wc %s] " fmt "\n",_b,##__VA_ARGS__); \
}while(0)

/* ── FIFA 2026 Team Table ─────────────────────────────────────── */
typedef struct {
    char code[4];   /* FIFA TLA  e.g. "USA"         */
    char iso2[3];   /* ISO 3166 alpha-2 for flag    */
    char name[48];
    char grp;       /* 'A'..'L'                     */
    char bg[10];    /* primary kit background hex   */
    char fg[10];    /* primary kit text hex         */
} FIFATeam;

/*
 * Build a UTF-8 flag emoji (8 bytes + NUL) from an ISO-3166-1 alpha-2 code.
 * Each country flag = two Regional Indicator letters (U+1F1E6..U+1F1FF).
 * UTF-8 encoding: F0 9F 87 (A6+offset)  for each letter.
 */
static void make_flag(const char *iso2, char *buf) {
    buf[0]=(char)0xF0; buf[1]=(char)0x9F; buf[2]=(char)0x87;
    buf[3]=(char)(0xA6 + (unsigned char)(iso2[0]-'A'));
    buf[4]=(char)0xF0; buf[5]=(char)0x9F; buf[6]=(char)0x87;
    buf[7]=(char)(0xA6 + (unsigned char)(iso2[1]-'A'));
    buf[8]='\0';
}

/* 48 teams / 12 groups (A-L), 4 per group */
static const FIFATeam TEAMS[48] = {
/* ── Group A ── */
{"USA","US","United States",   'A',"#002868","#FFFFFF"},
{"ENG","GB","England",         'A',"#FFFFFF","#012169"},
{"FRA","FR","France",          'A',"#003189","#FFFFFF"},
{"POL","PL","Poland",          'A',"#FFFFFF","#DC143C"},
/* ── Group B ── */
{"GER","DE","Germany",         'B',"#FFFFFF","#000000"},
{"ARG","AR","Argentina",       'B',"#74ACDF","#FFFFFF"},
{"NGA","NG","Nigeria",         'B',"#008751","#FFFFFF"},
{"TUN","TN","Tunisia",         'B',"#E70013","#FFFFFF"},
/* ── Group C ── */
{"ESP","ES","Spain",           'C',"#C60B1E","#FFC400"},
{"BRA","BR","Brazil",          'C',"#009C3B","#FEDF00"},
{"MAR","MA","Morocco",         'C',"#C1272D","#006233"},
{"AUS","AU","Australia",       'C',"#FFD700","#006400"},
/* ── Group D ── */
{"POR","PT","Portugal",        'D',"#C8102E","#006600"},
{"NED","NL","Netherlands",     'D',"#E8740C","#FFFFFF"},
{"JPN","JP","Japan",           'D',"#003F7F","#FFFFFF"},
{"URU","UY","Uruguay",         'D',"#75AADB","#FFFFFF"},
/* ── Group E ── */
{"BEL","BE","Belgium",         'E',"#CE1126","#000000"},
{"MEX","MX","Mexico",          'E',"#006847","#FFFFFF"},
{"SEN","SN","Senegal",         'E',"#00853F","#FFFFFF"},
{"KOR","KR","South Korea",     'E',"#003478","#FFFFFF"},
/* ── Group F ── */
{"CRO","HR","Croatia",         'F',"#FF0000","#FFFFFF"},
{"COL","CO","Colombia",        'F',"#FDD116","#003087"},
{"IRN","IR","Iran",            'F',"#239F40","#FFFFFF"},
{"CAN","CA","Canada",          'F',"#FF0000","#FFFFFF"},
/* ── Group G ── */
{"ITA","IT","Italy",           'G',"#0F67B1","#FFFFFF"},
{"SUI","CH","Switzerland",     'G',"#FF0000","#FFFFFF"},
{"ECU","EC","Ecuador",         'G',"#FFD100","#003087"},
{"GHA","GH","Ghana",           'G',"#000000","#FFFFFF"},
/* ── Group H ── */
{"DEN","DK","Denmark",         'H',"#C8102E","#FFFFFF"},
{"KSA","SA","Saudi Arabia",    'H',"#006C35","#FFFFFF"},
{"CMR","CM","Cameroon",        'H',"#007A5E","#FFFFFF"},
{"CRC","CR","Costa Rica",      'H',"#002B7F","#FFFFFF"},
/* ── Group I ── */
{"AUT","AT","Austria",         'I',"#ED2939","#FFFFFF"},
{"TUR","TR","Turkey",          'I',"#E30A17","#FFFFFF"},
{"EGY","EG","Egypt",           'I',"#CE1126","#FFFFFF"},
{"NZL","NZ","New Zealand",     'I',"#000000","#FFFFFF"},
/* ── Group J ── */
{"UKR","UA","Ukraine",         'J',"#005BBB","#FFFFFF"},
{"CHL","CL","Chile",           'J',"#D52B1E","#FFFFFF"},
{"ALG","DZ","Algeria",         'J',"#006233","#FFFFFF"},
{"IDN","ID","Indonesia",       'J',"#CE1126","#FFFFFF"},
/* ── Group K ── */
{"SRB","RS","Serbia",          'K',"#C8102E","#002395"},
{"QAT","QA","Qatar",           'K',"#8D1B3D","#FFFFFF"},
{"UZB","UZ","Uzbekistan",      'K',"#1EB53A","#FFFFFF"},
{"VEN","VE","Venezuela",       'K',"#CF142B","#003087"},
/* ── Group L ── */
{"SCO","GB","Scotland",        'L',"#003087","#FFFFFF"},
{"RSA","ZA","South Africa",    'L',"#007A4D","#FFFFFF"},
{"PAN","PA","Panama",          'L',"#FFFFFF","#DA121A"},
{"JOR","JO","Jordan",          'L',"#007A3D","#FFFFFF"},
};
#define NUM_TEAMS 48

static int team_by_code(const char *c) {
    if (!c||!c[0]) return -1;
    for (int i=0;i<NUM_TEAMS;i++) if (strncasecmp(TEAMS[i].code,c,3)==0) return i;
    return -1;
}
static int team_by_name(const char *n) {
    if (!n||!n[0]) return -1;
    for (int i=0;i<NUM_TEAMS;i++) if (strcasecmp(TEAMS[i].name,n)==0) return i;
    for (int i=0;i<NUM_TEAMS;i++) if (strcasestr(TEAMS[i].name,n)) return i;
    return -1;
}

/* ── Normalized data structures ──────────────────────────────── */

typedef enum { SRC_NONE=0, SRC_AF, SRC_FD, SRC_MOCK } DataSrc;

/* A single match, normalized from either API */
typedef struct {
    int  id;
    int  home_idx, away_idx;   /* index into TEAMS[], -1 = unknown */
    char home_code[4];
    char away_code[4];
    int  home_score, away_score;
    char status[8];            /* "NS","1H","HT","2H","ET","PEN","FT","CANC" */
    int  elapsed;
    int  extra;                /* injury / added time                        */
    char group[12];            /* "Group A"                                  */
    char round[32];            /* "R32","R16","QF","SF","Final",etc or ""    */
    char kickoff_iso[32];      /* ISO-8601 UTC for NS matches                */
    char last_event[128];      /* "⚽ GOAL 58' Pulisic (USA)"               */
} NMatch;

/* A single standings row */
typedef struct {
    int  rank;
    char code[4];
    int  played,won,drawn,lost,gf,ga,gd,pts;
} NStanding;

/* A scorer entry */
typedef struct {
    char player[48];
    char team_code[4];
    int  goals;
} NScorer;

/* Per-team goal-event overrides (clip ID and strobe flash count).
 * clip_id == 0 means "use global"; flashes == 0 means "use global". */
typedef struct {
    char code[4];
    int  clip_id;
    int  flashes;
} TeamOverride;

/* ── Application state ────────────────────────────────────────── */
typedef struct {
    /* Persisted config */
    char sel[MAX_SEL][4];       /* selected TLA codes                    */
    int  nsel;
    char af_key[64];            /* api-football API key                  */
    char fd_key[64];            /* football-data.org API key             */
    char dev_user[64];
    char dev_pass[64];
    int  poll_sec;          /* manual cap: 0 = use adaptive only         */
    int  live_poll_sec;     /* adaptive: tracked team live (1H/2H/ET/PEN)*/
    int  prematch_poll_sec; /* adaptive: tracked team kicks off <20 min  */
    int  idle_poll_sec;     /* adaptive: no tracked teams active         */
    char poll_mode[12];     /* "live" / "prematch" / "idle"              */
    int  effective_poll_sec;/* last computed effective interval          */
    int  enabled;
    int  disp_enabled;
    int  demo_mode;
    int  audio_enabled;     /* 0 = mute all goal sounds                  */
    int  audio_volume;      /* 0–100, applied as perceptual gain curve   */
    int  goal_clip_id;      /* mediaclip store ID for goal sound         */
    int  alert_clip_id;     /* mediaclip store ID for halftime/KO sound  */
    char webhook_url[256];  /* outbound goal event push URL (empty=off)  */
    int  webhook_enabled;   /* 0 = disabled                              */
    int  strobe_enabled;    /* flash team colors on goal (0=off)         */
    int  strobe_flashes;    /* number of alternating flashes (2–10)      */
    TeamOverride team_overrides[MAX_SEL]; /* per-team clip/flash overrides */
    int  n_overrides;
    char text_color[16];
    char bg_color[16];
    char text_size[16];
    int  scroll_speed;
    int  duration_ms;

    /* Live data — guarded by lock */
    NMatch    matches[MAX_MATCHES];
    int       nmatches;
    NStanding standings[N_GROUPS][GPG];
    int       nst[N_GROUPS];
    NScorer   scorers[MAX_SCORERS];
    int       nscorers;
    time_t    last_poll;
    DataSrc   last_src;

    /* Runtime */
    pthread_mutex_t lock;
    AXParameter    *axp;
    int             running;
    int             strobe_active; /* display thread pauses during goal flash */
} AppState;

static AppState g_app;

/* Display message queue */
static char g_dmsgs[MAX_DISP_MSG][MSG_SZ];
static int  g_ddur[MAX_DISP_MSG];
static int  g_dcount;
static int  g_didx;
static pthread_mutex_t g_dlock = PTHREAD_MUTEX_INITIALIZER;

/* ── HTTP / CURL helpers ──────────────────────────────────────── */

typedef struct { char *data; size_t len; size_t cap; } HBuf;

static size_t hbuf_cb(char *ptr, size_t sz, size_t nm, void *ud) {
    HBuf *b = (HBuf*)ud;
    size_t need = b->len + sz*nm + 1;
    if (need > b->cap) {
        b->cap = need*2;
        char *tmp = realloc(b->data, b->cap);
        if (!tmp) return 0;
        b->data = tmp;
    }
    memcpy(b->data+b->len, ptr, sz*nm);
    b->len += sz*nm;
    b->data[b->len] = '\0';
    return sz*nm;
}

/* Two-field buffer used by the audio curl helpers (matches MLB pattern) */
typedef struct { char *data; size_t len; } CurlBuf;

static size_t curl_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    CurlBuf *b = (CurlBuf *)userdata;
    size_t total = size * nmemb;
    char *tmp = realloc(b->data, b->len + total + 1);
    if (!tmp) return 0;
    b->data = tmp;
    memcpy(b->data + b->len, ptr, total);
    b->len += total;
    b->data[b->len] = '\0';
    return total;
}

static int http_get(const char *url,
                    const char *hdr_name, const char *hdr_val,
                    char *out, size_t outsz, long *code_out) {
    CURL *curl = curl_easy_init();
    if (!curl) return -1;

    HBuf b = { malloc(JBUF), 0, JBUF };
    if (!b.data) { curl_easy_cleanup(curl); return -1; }

    struct curl_slist *hdrs = NULL;
    char hbuf[256];
    if (hdr_name && hdr_val && hdr_val[0]) {
        snprintf(hbuf, sizeof(hbuf), "%s: %s", hdr_name, hdr_val);
        hdrs = curl_slist_append(hdrs, hbuf);
    }
    hdrs = curl_slist_append(hdrs, "Accept: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, hbuf_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &b);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, CURL_TO);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    CURLcode rc = curl_easy_perform(curl);
    long code = 0;
    if (rc == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    if (code_out) *code_out = code;

    int ret = -1;
    if (rc == CURLE_OK && code == 200 && b.len > 0) {
        size_t cp = b.len < outsz-1 ? b.len : outsz-1;
        memcpy(out, b.data, cp); out[cp]='\0';
        ret = 0;
    }

    free(b.data);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    return ret;
}

/* Check if an api-football body is a rate-limit / error response */
static int af_is_error(const char *j) {
    if (!j || !*j) return 1;
    cJSON *r = cJSON_Parse(j); if (!r) return 1;
    cJSON *e = cJSON_GetObjectItem(r,"errors");
    int bad = 0;
    if (cJSON_IsArray(e) && cJSON_GetArraySize(e)>0) bad=1;
    if (!bad && cJSON_IsObject(e)) {
        if (cJSON_GetObjectItem(e,"rateLimit") || cJSON_GetObjectItem(e,"requests"))
            bad=1;
    }
    cJSON_Delete(r);
    return bad;
}

/* ── Mock data loader ─────────────────────────────────────────── */
static int load_mock(const char *fname, char *out, size_t outsz) {
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", MOCK_DIR, fname);
    FILE *f = fopen(path, "r");
    if (!f) { LOG("mock open failed: %s", path); return -1; }
    size_t n = fread(out, 1, outsz-1, f);
    fclose(f);
    if (n == 0) return -1;
    out[n]='\0'; return 0;
}

/* ── Unified fetch with failover ──────────────────────────────── */
/*
 * af_path  — suffix appended to AF_BASE
 * fd_path  — suffix appended to FD_BASE
 * mock_af  — filename in MOCK_DIR (api-football format)
 * Returns the source that succeeded, or SRC_NONE on total failure.
 */
static DataSrc fetch_ep(const char *af_path, const char *fd_path,
                        const char *mock_af,
                        char *out, size_t outsz) {
    /* Demo mode: bypass all HTTP */
    if (g_app.demo_mode) {
        if (load_mock(mock_af, out, outsz) == 0) return SRC_MOCK;
        return SRC_NONE;
    }

    /* ─── Primary: api-football ─── */
    if (g_app.af_key[0]) {
        char url[512];
        snprintf(url, sizeof(url), "%s%s", AF_BASE, af_path);
        long code=0;
        int r = http_get(url, "X-RapidAPI-Key", g_app.af_key, out, outsz, &code);
        if (r == 0 && !af_is_error(out)) {
            LOG("af OK  %s", af_path); return SRC_AF;
        }
        LOG("af FAIL ret=%d http=%ld on %s — trying fallback", r, code, af_path);
    }

    /* ─── Fallback: football-data.org ─── */
    if (g_app.fd_key[0]) {
        char url[512];
        snprintf(url, sizeof(url), "%s%s", FD_BASE, fd_path);
        long code=0;
        int r = http_get(url, "X-Auth-Token", g_app.fd_key, out, outsz, &code);
        if (r == 0) {
            LOG("fd  OK  %s", fd_path); return SRC_FD;
        }
        LOG("fd  FAIL ret=%d http=%ld on %s", r, code, fd_path);
    }

    LOG("all sources exhausted for %s", af_path);
    return SRC_NONE;
}

/* ═══════════════ Parsers — api-football format ══════════════════ */

/* Normalise a raw round/stage string from either API into a short label.
 * Group-stage strings produce "" (caller leaves round[0]='\0'). */
static void norm_round(const char *s, char *out) {
    out[0] = '\0';
    if (!s || !s[0]) return;
    /* Convert to uppercase for case-insensitive checks */
    char u[64]; int i;
    for (i=0; s[i] && i<63; i++) u[i]=(char)toupper((unsigned char)s[i]);
    u[i]='\0';
    if      (strstr(u,"ROUND OF 32")  || strstr(u,"R32")) strcpy(out,"R32");
    else if (strstr(u,"ROUND OF 16")  || strstr(u,"R16")) strcpy(out,"R16");
    else if (strstr(u,"QUARTER"))                          strcpy(out,"QF");
    else if (strstr(u,"SEMI"))                             strcpy(out,"SF");
    else if (strstr(u,"THIRD") || strstr(u,"3RD"))         strcpy(out,"3rd Place");
    else if (strstr(u,"FINAL"))                            strcpy(out,"Final");
    /* else: group stage or unknown → leave empty */
}

static void af_norm_status(const char *s, char *out) {
    if (!s) { strcpy(out,"NS"); return; }
    strncpy(out, s, 7); out[7]='\0';
}

static int af_parse_fixtures(const char *json,
                              NMatch *matches, int maxm, int *count) {
    *count=0;
    cJSON *root = cJSON_Parse(json); if (!root) return -1;
    cJSON *resp = cJSON_GetObjectItem(root,"response");
    if (!cJSON_IsArray(resp)) { cJSON_Delete(root); return -1; }

    int n = cJSON_GetArraySize(resp);
    for (int i=0; i<n && *count<maxm; i++) {
        cJSON *item = cJSON_GetArrayItem(resp,i);
        cJSON *fix  = cJSON_GetObjectItem(item,"fixture");
        cJSON *lg   = cJSON_GetObjectItem(item,"league");
        cJSON *tms  = cJSON_GetObjectItem(item,"teams");
        cJSON *gls  = cJSON_GetObjectItem(item,"goals");
        cJSON *evts = cJSON_GetObjectItem(item,"events");
        if (!fix||!tms||!gls) continue;

        NMatch *m = &matches[(*count)++];
        memset(m, 0, sizeof(*m));
        m->home_idx = m->away_idx = -1;

        cJSON *fid = cJSON_GetObjectItem(fix,"id");
        m->id = cJSON_IsNumber(fid) ? (int)fid->valuedouble : 0;

        cJSON *st = cJSON_GetObjectItem(fix,"status");
        cJSON *sh = cJSON_GetObjectItem(st,"short");
        cJSON *el = cJSON_GetObjectItem(st,"elapsed");
        cJSON *ex = cJSON_GetObjectItem(st,"extra");
        af_norm_status(cJSON_IsString(sh)?sh->valuestring:NULL, m->status);
        m->elapsed = cJSON_IsNumber(el) ? (int)el->valuedouble : 0;
        m->extra   = (cJSON_IsNumber(ex)&&!cJSON_IsNull(ex)) ? (int)ex->valuedouble : 0;

        cJSON *dt = cJSON_GetObjectItem(fix,"date");
        if (cJSON_IsString(dt)) strncpy(m->kickoff_iso, dt->valuestring, 31);

        /* group label + round */
        cJSON *grpobj = cJSON_GetObjectItem(lg,"group");
        if (cJSON_IsString(grpobj)) { strncpy(m->group, grpobj->valuestring, 11); m->group[11]='\0'; }
        else { memcpy(m->group,"Group Stage",11); m->group[11]='\0'; }
        cJSON *rndobj = cJSON_GetObjectItem(lg,"round");
        norm_round(cJSON_IsString(rndobj)?rndobj->valuestring:"", m->round);

        /* teams — try tla field first, fall back to name fuzzy-match */
        cJSON *home = cJSON_GetObjectItem(tms,"home");
        cJSON *away = cJSON_GetObjectItem(tms,"away");
        cJSON *htla = cJSON_GetObjectItem(home,"tla");
        cJSON *atla = cJSON_GetObjectItem(away,"tla");
        cJSON *hn   = cJSON_GetObjectItem(home,"name");
        cJSON *an   = cJSON_GetObjectItem(away,"name");
        const char *hns = cJSON_IsString(hn)?hn->valuestring:"";
        const char *ans = cJSON_IsString(an)?an->valuestring:"";

        int hi = cJSON_IsString(htla) ? team_by_code(htla->valuestring) : -1;
        if (hi<0) hi = team_by_name(hns);
        int ai = cJSON_IsString(atla) ? team_by_code(atla->valuestring) : -1;
        if (ai<0) ai = team_by_name(ans);

        m->home_idx=hi; m->away_idx=ai;
        if (hi>=0) strncpy(m->home_code,TEAMS[hi].code,3);
        else { strncpy(m->home_code,hns,3); m->home_code[3]='\0'; }
        if (ai>=0) strncpy(m->away_code,TEAMS[ai].code,3);
        else { strncpy(m->away_code,ans,3); m->away_code[3]='\0'; }

        /* goals */
        cJSON *hg=cJSON_GetObjectItem(gls,"home");
        cJSON *ag=cJSON_GetObjectItem(gls,"away");
        m->home_score=(cJSON_IsNumber(hg)&&!cJSON_IsNull(hg))?(int)hg->valuedouble:0;
        m->away_score=(cJSON_IsNumber(ag)&&!cJSON_IsNull(ag))?(int)ag->valuedouble:0;

        /* last event — scan for most recent goal; fall back to last red card */
        if (cJSON_IsArray(evts)) {
            int ne = cJSON_GetArraySize(evts);
            int best_goal_min = -1;
            cJSON *best_goal = NULL, *last_red = NULL;
            for (int ei = 0; ei < ne; ei++) {
                cJSON *ev   = cJSON_GetArrayItem(evts, ei);
                cJSON *etp  = cJSON_GetObjectItem(ev, "type");
                cJSON *edet = cJSON_GetObjectItem(ev, "detail");
                cJSON *etm  = cJSON_GetObjectItem(ev, "time");
                cJSON *eel  = cJSON_GetObjectItem(etm, "elapsed");
                int emin    = cJSON_IsNumber(eel) ? (int)eel->valuedouble : 0;
                const char *tp  = cJSON_IsString(etp)  ? etp->valuestring  : "";
                const char *det = cJSON_IsString(edet) ? edet->valuestring : "";
                if (strcmp(tp,"Goal")==0 && strcmp(det,"Missed Penalty")!=0) {
                    if (emin >= best_goal_min) { best_goal_min = emin; best_goal = ev; }
                }
                if (strcmp(tp,"Card")==0 && strstr(det,"Red")) last_red = ev;
            }
            if (best_goal) {
                cJSON *edet = cJSON_GetObjectItem(best_goal, "detail");
                cJSON *etm  = cJSON_GetObjectItem(best_goal, "time");
                cJSON *eel  = cJSON_GetObjectItem(etm, "elapsed");
                cJSON *eex  = cJSON_GetObjectItem(etm, "extra");
                cJSON *epln = cJSON_GetObjectItem(cJSON_GetObjectItem(best_goal,"player"),"name");
                cJSON *etmn = cJSON_GetObjectItem(cJSON_GetObjectItem(best_goal,"team"), "name");
                int emin  = cJSON_IsNumber(eel) ? (int)eel->valuedouble : 0;
                int extra = (cJSON_IsNumber(eex)&&!cJSON_IsNull(eex)) ? (int)eex->valuedouble : 0;
                const char *det = cJSON_IsString(edet) ? edet->valuestring : "";
                const char *pn  = cJSON_IsString(epln) ? epln->valuestring : "";
                const char *tn  = cJSON_IsString(etmn) ? etmn->valuestring : "";
                int ti = team_by_name(tn);
                char tc[4]="?"; if(ti>=0) strncpy(tc,TEAMS[ti].code,3);
                const char *pfx = strstr(det,"Own Goal") ? "\xE2\x9A\xBD OG"  :
                                  strstr(det,"Penalty")  ? "\xE2\x9A\xBD PEN" :
                                                           "\xE2\x9A\xBD GOAL";
                if (extra > 0)
                    snprintf(m->last_event,127,"%s %d+%d' %s (%s)",pfx,emin,extra,pn,tc);
                else
                    snprintf(m->last_event,127,"%s %d' %s (%s)",pfx,emin,pn,tc);
            } else if (last_red) {
                cJSON *etm  = cJSON_GetObjectItem(last_red, "time");
                cJSON *eel  = cJSON_GetObjectItem(etm, "elapsed");
                cJSON *epln = cJSON_GetObjectItem(cJSON_GetObjectItem(last_red,"player"),"name");
                cJSON *etmn = cJSON_GetObjectItem(cJSON_GetObjectItem(last_red,"team"), "name");
                int emin    = cJSON_IsNumber(eel) ? (int)eel->valuedouble : 0;
                const char *pn = cJSON_IsString(epln) ? epln->valuestring : "";
                const char *tn = cJSON_IsString(etmn) ? etmn->valuestring : "";
                int ti = team_by_name(tn);
                char tc[4]="?"; if(ti>=0) strncpy(tc,TEAMS[ti].code,3);
                snprintf(m->last_event,127,"\xF0\x9F\x9F\xA5 RED CARD %d' %s (%s)",emin,pn,tc);
            }
        }
    }
    cJSON_Delete(root);
    return 0;
}

static int af_parse_standings(const char *json,
                               NStanding st[][GPG], int *nst) {
    memset(nst,0,N_GROUPS*sizeof(int));
    cJSON *root=cJSON_Parse(json); if(!root) return -1;
    cJSON *resp=cJSON_GetObjectItem(root,"response");
    if (!cJSON_IsArray(resp)||cJSON_GetArraySize(resp)<1) {
        cJSON_Delete(root); return -1; }
    cJSON *lg  = cJSON_GetObjectItem(cJSON_GetArrayItem(resp,0),"league");
    cJSON *sts = cJSON_GetObjectItem(lg,"standings");
    if (!cJSON_IsArray(sts)) { cJSON_Delete(root); return -1; }

    int ng = cJSON_GetArraySize(sts);
    for (int g=0; g<ng && g<N_GROUPS; g++) {
        cJSON *grp = cJSON_GetArrayItem(sts,g);
        if (!cJSON_IsArray(grp)) continue;
        int nt = cJSON_GetArraySize(grp);
        for (int t=0; t<nt && t<GPG; t++) {
            cJSON *e  = cJSON_GetArrayItem(grp,t);
            cJSON *rk = cJSON_GetObjectItem(e,"rank");
            cJSON *tm = cJSON_GetObjectItem(e,"team");
            cJSON *tn = cJSON_GetObjectItem(tm,"name");
            cJSON *ttla=cJSON_GetObjectItem(tm,"tla");
            cJSON *pt = cJSON_GetObjectItem(e,"points");
            cJSON *gd = cJSON_GetObjectItem(e,"goalsDiff");
            cJSON *al = cJSON_GetObjectItem(e,"all");
            cJSON *gls=cJSON_GetObjectItem(al,"goals");
            cJSON *gf = cJSON_GetObjectItem(gls,"for");
            cJSON *ga = cJSON_GetObjectItem(gls,"against");
            cJSON *pl = cJSON_GetObjectItem(al,"played");
            cJSON *wn = cJSON_GetObjectItem(al,"win");
            cJSON *dr = cJSON_GetObjectItem(al,"draw");
            cJSON *ls = cJSON_GetObjectItem(al,"lose");

            NStanding *s = &st[g][t];
            s->rank  =cJSON_IsNumber(rk)?(int)rk->valuedouble:t+1;
            s->pts   =cJSON_IsNumber(pt)?(int)pt->valuedouble:0;
            s->gd    =cJSON_IsNumber(gd)?(int)gd->valuedouble:0;
            s->gf    =cJSON_IsNumber(gf)?(int)gf->valuedouble:0;
            s->ga    =cJSON_IsNumber(ga)?(int)ga->valuedouble:0;
            s->played=cJSON_IsNumber(pl)?(int)pl->valuedouble:0;
            s->won   =cJSON_IsNumber(wn)?(int)wn->valuedouble:0;
            s->drawn =cJSON_IsNumber(dr)?(int)dr->valuedouble:0;
            s->lost  =cJSON_IsNumber(ls)?(int)ls->valuedouble:0;
            const char *tla = cJSON_IsString(ttla)?ttla->valuestring:"";
            const char *nm  = cJSON_IsString(tn)?tn->valuestring:"";
            int idx = team_by_code(tla);
            if (idx<0) idx=team_by_name(nm);
            if (idx>=0) strncpy(s->code,TEAMS[idx].code,3);
            else { strncpy(s->code,tla[0]?tla:nm,3); s->code[3]='\0'; }
            nst[g]++;
        }
    }
    cJSON_Delete(root); return 0;
}

static int af_parse_scorers(const char *json,
                             NScorer *sc, int maxs, int *count) {
    *count=0;
    cJSON *root=cJSON_Parse(json); if(!root) return -1;
    cJSON *resp=cJSON_GetObjectItem(root,"response");
    if (!cJSON_IsArray(resp)) { cJSON_Delete(root); return -1; }
    int n=cJSON_GetArraySize(resp);
    for (int i=0; i<n && *count<maxs; i++) {
        cJSON *item=cJSON_GetArrayItem(resp,i);
        cJSON *pl  =cJSON_GetObjectItem(item,"player");
        cJSON *sts =cJSON_GetObjectItem(item,"statistics");
        cJSON *s0  =cJSON_IsArray(sts)?cJSON_GetArrayItem(sts,0):NULL;
        cJSON *pn  =cJSON_GetObjectItem(pl,"name");
        cJSON *tm  =s0?cJSON_GetObjectItem(s0,"team"):NULL;
        cJSON *tn  =cJSON_GetObjectItem(tm,"name");
        cJSON *gls =s0?cJSON_GetObjectItem(s0,"goals"):NULL;
        cJSON *tot =cJSON_GetObjectItem(gls,"total");
        if (!cJSON_IsString(pn)||!cJSON_IsNumber(tot)) continue;
        NScorer *s=&sc[(*count)++];
        strncpy(s->player,pn->valuestring,47);
        s->goals=(int)tot->valuedouble;
        const char *nm=cJSON_IsString(tn)?tn->valuestring:"";
        int ti=team_by_name(nm);
        if (ti>=0) strncpy(s->team_code,TEAMS[ti].code,3);
        else { strncpy(s->team_code,nm,3); s->team_code[3]='\0'; }
    }
    cJSON_Delete(root); return 0;
}

/* ═══════════════ Parsers — football-data.org format ═════════════ */

static void fd_norm_status(const char *s, char *out) {
    if (!s) { strcpy(out,"NS"); return; }
    if (!strcmp(s,"IN_PLAY"))             strcpy(out,"1H");
    else if (!strcmp(s,"HALF_TIME"))      strcpy(out,"HT");
    else if (!strcmp(s,"FINISHED"))       strcpy(out,"FT");
    else if (!strcmp(s,"SCHEDULED")||
             !strcmp(s,"TIMED"))          strcpy(out,"NS");
    else if (!strcmp(s,"PAUSED"))         strcpy(out,"HT");
    else if (!strcmp(s,"EXTRA_TIME"))     strcpy(out,"ET");
    else if (!strcmp(s,"PENALTY_SHOOTOUT")) strcpy(out,"PEN");
    else if (!strcmp(s,"CANCELLED")||
             !strcmp(s,"POSTPONED"))      strcpy(out,"CANC");
    else { strncpy(out,s,7); out[7]='\0'; }
}

static int fd_parse_matches(const char *json,
                             NMatch *matches, int maxm, int *count) {
    *count=0;
    cJSON *root=cJSON_Parse(json); if(!root) return -1;
    cJSON *arr=cJSON_GetObjectItem(root,"matches");
    if (!cJSON_IsArray(arr)) { cJSON_Delete(root); return -1; }
    int n=cJSON_GetArraySize(arr);
    for (int i=0; i<n && *count<maxm; i++) {
        cJSON *item=cJSON_GetArrayItem(arr,i);
        NMatch *m=&matches[(*count)++];
        memset(m,0,sizeof(*m)); m->home_idx=m->away_idx=-1;

        cJSON *iid=cJSON_GetObjectItem(item,"id");
        m->id=cJSON_IsNumber(iid)?(int)iid->valuedouble:0;

        cJSON *st=cJSON_GetObjectItem(item,"status");
        fd_norm_status(cJSON_IsString(st)?st->valuestring:NULL, m->status);

        cJSON *min=cJSON_GetObjectItem(item,"minute");
        m->elapsed=cJSON_IsNumber(min)?(int)min->valuedouble:0;

        cJSON *dt=cJSON_GetObjectItem(item,"utcDate");
        if (cJSON_IsString(dt)) strncpy(m->kickoff_iso,dt->valuestring,31);

        cJSON *grp=cJSON_GetObjectItem(item,"group");
        if (cJSON_IsString(grp)) strncpy(m->group,grp->valuestring,11);
        else strncpy(m->group,"Group Stage",11);
        cJSON *stg=cJSON_GetObjectItem(item,"stage");
        norm_round(cJSON_IsString(stg)?stg->valuestring:"", m->round);

        cJSON *ht  =cJSON_GetObjectItem(item,"homeTeam");
        cJSON *at  =cJSON_GetObjectItem(item,"awayTeam");
        cJSON *htla=cJSON_GetObjectItem(ht,"tla");
        cJSON *atla=cJSON_GetObjectItem(at,"tla");
        cJSON *hn  =cJSON_GetObjectItem(ht,"name");
        cJSON *an  =cJSON_GetObjectItem(at,"name");
        const char *htlas=cJSON_IsString(htla)?htla->valuestring:"";
        const char *atlas=cJSON_IsString(atla)?atla->valuestring:"";

        int hi=team_by_code(htlas); if(hi<0)hi=team_by_name(cJSON_IsString(hn)?hn->valuestring:"");
        int ai=team_by_code(atlas); if(ai<0)ai=team_by_name(cJSON_IsString(an)?an->valuestring:"");
        m->home_idx=hi; m->away_idx=ai;
        if(hi>=0) strncpy(m->home_code,TEAMS[hi].code,3);
        else { strncpy(m->home_code,htlas[0]?htlas:(cJSON_IsString(hn)?hn->valuestring:"?"),3); m->home_code[3]='\0'; }
        if(ai>=0) strncpy(m->away_code,TEAMS[ai].code,3);
        else { strncpy(m->away_code,atlas[0]?atlas:(cJSON_IsString(an)?an->valuestring:"?"),3); m->away_code[3]='\0'; }

        cJSON *sc=cJSON_GetObjectItem(item,"score");
        cJSON *ft=cJSON_GetObjectItem(sc,"fullTime");
        cJSON *hsc=cJSON_GetObjectItem(ft,"home");
        cJSON *asc=cJSON_GetObjectItem(ft,"away");
        m->home_score=(cJSON_IsNumber(hsc)&&!cJSON_IsNull(hsc))?(int)hsc->valuedouble:0;
        m->away_score=(cJSON_IsNumber(asc)&&!cJSON_IsNull(asc))?(int)asc->valuedouble:0;

        cJSON *goals = cJSON_GetObjectItem(item, "goals");
        if (cJSON_IsArray(goals) && cJSON_GetArraySize(goals) > 0) {
            int ng   = cJSON_GetArraySize(goals);
            cJSON *last = cJSON_GetArrayItem(goals, ng - 1);
            cJSON *gtype = cJSON_GetObjectItem(last, "type");
            cJSON *gmin  = cJSON_GetObjectItem(last, "minute");
            cJSON *gadd  = cJSON_GetObjectItem(last, "additionalMinute");
            cJSON *snm   = cJSON_GetObjectItem(cJSON_GetObjectItem(last,"scorer"), "name");
            cJSON *gtnn  = cJSON_GetObjectItem(cJSON_GetObjectItem(last,"team"),   "name");
            int gm    = cJSON_IsNumber(gmin) ? (int)gmin->valuedouble : 0;
            int gadd_ = (cJSON_IsNumber(gadd)&&!cJSON_IsNull(gadd)) ? (int)gadd->valuedouble : 0;
            const char *sn  = cJSON_IsString(snm)  ? snm->valuestring  : "";
            const char *gtp = cJSON_IsString(gtype) ? gtype->valuestring : "GOAL";
            int ti = team_by_name(cJSON_IsString(gtnn) ? gtnn->valuestring : "");
            char tc[4]="?"; if(ti>=0) strncpy(tc,TEAMS[ti].code,3);
            const char *pfx = strcmp(gtp,"OWN_GOAL")==0 ? "\xE2\x9A\xBD OG"  :
                              strcmp(gtp,"PENALTY") ==0 ? "\xE2\x9A\xBD PEN" :
                                                          "\xE2\x9A\xBD GOAL";
            if (gadd_ > 0)
                snprintf(m->last_event,127,"%s %d+%d' %s (%s)",pfx,gm,gadd_,sn,tc);
            else
                snprintf(m->last_event,127,"%s %d' %s (%s)",pfx,gm,sn,tc);
        }
    }
    cJSON_Delete(root); return 0;
}

static int fd_parse_standings(const char *json,
                               NStanding st[][GPG], int *nst) {
    memset(nst,0,N_GROUPS*sizeof(int));
    cJSON *root=cJSON_Parse(json); if(!root) return -1;
    cJSON *sts=cJSON_GetObjectItem(root,"standings");
    if (!cJSON_IsArray(sts)) { cJSON_Delete(root); return -1; }
    int ng=cJSON_GetArraySize(sts);
    for (int g=0; g<ng&&g<N_GROUPS; g++) {
        cJSON *grp=cJSON_GetArrayItem(sts,g);
        cJSON *tbl=cJSON_GetObjectItem(grp,"table");
        if (!cJSON_IsArray(tbl)) continue;
        int nt=cJSON_GetArraySize(tbl);
        for (int t=0; t<nt&&t<GPG; t++) {
            cJSON *e  =cJSON_GetArrayItem(tbl,t);
            cJSON *rk =cJSON_GetObjectItem(e,"position");
            cJSON *tm =cJSON_GetObjectItem(e,"team");
            cJSON *tla=cJSON_GetObjectItem(tm,"tla");
            cJSON *tn =cJSON_GetObjectItem(tm,"name");
            cJSON *pl =cJSON_GetObjectItem(e,"playedGames");
            cJSON *wn =cJSON_GetObjectItem(e,"won");
            cJSON *dr =cJSON_GetObjectItem(e,"draw");
            cJSON *ls =cJSON_GetObjectItem(e,"lost");
            cJSON *gf =cJSON_GetObjectItem(e,"goalsFor");
            cJSON *ga =cJSON_GetObjectItem(e,"goalsAgainst");
            cJSON *gd =cJSON_GetObjectItem(e,"goalDifference");
            cJSON *pt =cJSON_GetObjectItem(e,"points");
            NStanding *s=&st[g][t];
            s->rank  =cJSON_IsNumber(rk)?(int)rk->valuedouble:t+1;
            s->pts   =cJSON_IsNumber(pt)?(int)pt->valuedouble:0;
            s->gd    =cJSON_IsNumber(gd)?(int)gd->valuedouble:0;
            s->gf    =cJSON_IsNumber(gf)?(int)gf->valuedouble:0;
            s->ga    =cJSON_IsNumber(ga)?(int)ga->valuedouble:0;
            s->played=cJSON_IsNumber(pl)?(int)pl->valuedouble:0;
            s->won   =cJSON_IsNumber(wn)?(int)wn->valuedouble:0;
            s->drawn =cJSON_IsNumber(dr)?(int)dr->valuedouble:0;
            s->lost  =cJSON_IsNumber(ls)?(int)ls->valuedouble:0;
            const char *tlastr=cJSON_IsString(tla)?tla->valuestring:"";
            const char *nm    =cJSON_IsString(tn)?tn->valuestring:"";
            int idx=team_by_code(tlastr);
            if(idx<0) idx=team_by_name(nm);
            if(idx>=0) strncpy(s->code,TEAMS[idx].code,3);
            else { strncpy(s->code,tlastr[0]?tlastr:nm,3); s->code[3]='\0'; }
            nst[g]++;
        }
    }
    cJSON_Delete(root); return 0;
}

static int fd_parse_scorers(const char *json,
                             NScorer *sc, int maxs, int *count) {
    *count=0;
    cJSON *root=cJSON_Parse(json); if(!root) return -1;
    cJSON *arr=cJSON_GetObjectItem(root,"scorers");
    if (!cJSON_IsArray(arr)) { cJSON_Delete(root); return -1; }
    int n=cJSON_GetArraySize(arr);
    for (int i=0; i<n&&*count<maxs; i++) {
        cJSON *item=cJSON_GetArrayItem(arr,i);
        cJSON *pl  =cJSON_GetObjectItem(item,"player");
        cJSON *pn  =cJSON_GetObjectItem(pl,"name");
        cJSON *tm  =cJSON_GetObjectItem(item,"team");
        cJSON *tla =cJSON_GetObjectItem(tm,"tla");
        cJSON *tn  =cJSON_GetObjectItem(tm,"name");
        cJSON *gls =cJSON_GetObjectItem(item,"goals");
        if (!cJSON_IsString(pn)||!cJSON_IsNumber(gls)) continue;
        NScorer *s=&sc[(*count)++];
        strncpy(s->player,pn->valuestring,47);
        s->goals=(int)gls->valuedouble;
        const char *tlastr=cJSON_IsString(tla)?tla->valuestring:"";
        const char *nm    =cJSON_IsString(tn)?tn->valuestring:"";
        int ti=team_by_code(tlastr);
        if(ti<0) ti=team_by_name(nm);
        if(ti>=0) strncpy(s->team_code,TEAMS[ti].code,3);
        else { strncpy(s->team_code,tlastr[0]?tlastr:nm,3); s->team_code[3]='\0'; }
    }
    cJSON_Delete(root); return 0;
}

/* ── Helpers ──────────────────────────────────────────────────── */

/* Per-team override lookups — return override if > 0, else global default. */
static int get_team_clip(const char *code) {
    for (int i = 0; i < g_app.n_overrides; i++)
        if (strncasecmp(g_app.team_overrides[i].code, code, 3) == 0 &&
            g_app.team_overrides[i].clip_id > 0)
            return g_app.team_overrides[i].clip_id;
    return g_app.goal_clip_id;
}

static int get_team_flashes(const char *code) {
    for (int i = 0; i < g_app.n_overrides; i++)
        if (strncasecmp(g_app.team_overrides[i].code, code, 3) == 0 &&
            g_app.team_overrides[i].flashes > 0)
            return g_app.team_overrides[i].flashes;
    return g_app.strobe_flashes;
}

static int is_selected(const char *code) {
    if (g_app.nsel == 0) return 1; /* track all if nothing selected */
    for (int i=0; i<g_app.nsel; i++)
        if (strncasecmp(g_app.sel[i],code,3)==0) return 1;
    return 0;
}

static void flag_for(const char *code, char *buf) {
    int i=team_by_code(code);
    if (i>=0) make_flag(TEAMS[i].iso2,buf); else buf[0]='\0';
}

static void fmt_clock(const NMatch *m, char *buf, size_t sz) {
    if (m->extra>0) snprintf(buf,sz,"%d+%d'",m->elapsed,m->extra);
    else            snprintf(buf,sz,"%d'",m->elapsed);
}

static void fmt_countdown(const char *iso, char *buf, size_t sz) {
    struct tm tm; memset(&tm,0,sizeof(tm));
    if (sscanf(iso,"%d-%d-%dT%d:%d:%d",
               &tm.tm_year,&tm.tm_mon,&tm.tm_mday,
               &tm.tm_hour,&tm.tm_min,&tm.tm_sec) < 6) {
        snprintf(buf,sz,"soon"); return;
    }
    tm.tm_year-=1900; tm.tm_mon-=1;
    time_t ko=timegm(&tm);
    long diff=(long)(ko-time(NULL));
    if (diff<=0) { snprintf(buf,sz,"now"); return; }
    long d=diff/86400, h=(diff%86400)/3600, m=(diff%3600)/60;
    if (d>0) snprintf(buf,sz,"%ldd %ldh %ldm",d,h,m);
    else if (h>0) snprintf(buf,sz,"%ldh %ldm",h,m);
    else snprintf(buf,sz,"%ldm",m);
}

/* ── Display queue builder (called under g_app.lock) ─────────── */
static void rebuild_queue_locked(void) {
    char   msgs[MAX_DISP_MSG][MSG_SZ];
    int    durs[MAX_DISP_MSG];
    int    cnt=0;
    int    dur = g_app.duration_ms>0 ? g_app.duration_ms : 15000;

    /* ── Pass 0: live knockout matches for selected teams ── */
    for (int i=0; i<g_app.nmatches && cnt<MAX_DISP_MSG; i++) {
        NMatch *m=&g_app.matches[i];
        if (!m->round[0]) continue; /* skip group stage */
        if (!is_selected(m->home_code) && !is_selected(m->away_code)) continue;
        int live = !strcmp(m->status,"1H")||!strcmp(m->status,"2H")||
                   !strcmp(m->status,"ET")||!strcmp(m->status,"PEN")||
                   !strcmp(m->status,"HT");
        if (!live) continue;
        char hf[16],af[16],cl[16];
        flag_for(m->home_code,hf); flag_for(m->away_code,af);
        fmt_clock(m,cl,sizeof(cl));
        snprintf(msgs[cnt],MSG_SZ,
            "\xE2\x9A\xBD LIVE %s: %s%s %d\xE2\x80\x93%d %s%s | %s",
            m->round, hf,m->home_code, m->home_score,
            m->away_score, af,m->away_code, cl);
        durs[cnt++]=dur;
        if (m->last_event[0] && cnt<MAX_DISP_MSG) {
            strncpy(msgs[cnt],m->last_event,MSG_SZ-1);
            durs[cnt++]=dur/2;
        }
    }

    /* ── Pass 1: live matches for selected teams (highest priority) ── */
    for (int i=0; i<g_app.nmatches && cnt<MAX_DISP_MSG; i++) {
        NMatch *m=&g_app.matches[i];
        if (!is_selected(m->home_code) && !is_selected(m->away_code)) continue;
        char hf[16],af[16],cl[16];
        flag_for(m->home_code,hf); flag_for(m->away_code,af);

        int live = !strcmp(m->status,"1H")||!strcmp(m->status,"2H")||
                   !strcmp(m->status,"ET")||!strcmp(m->status,"PEN");
        int ht   = !strcmp(m->status,"HT");
        int ft   = !strcmp(m->status,"FT");

        if (live) {
            fmt_clock(m,cl,sizeof(cl));
            snprintf(msgs[cnt],MSG_SZ,
                "\xE2\x9A\xBD LIVE: %s%s %d\xE2\x80\x93%d %s%s | %s | %s",
                hf,m->home_code,m->home_score,
                m->away_score,af,m->away_code,cl,m->group);
            durs[cnt++]=dur;
            if (m->last_event[0] && cnt<MAX_DISP_MSG) {
                strncpy(msgs[cnt],m->last_event,MSG_SZ-1);
                durs[cnt++]=dur/2;
            }
        } else if (ht) {
            snprintf(msgs[cnt],MSG_SZ,
                "HT: %s%s %d\xE2\x80\x93%d %s%s | %s",
                hf,m->home_code,m->home_score,
                m->away_score,af,m->away_code,m->group);
            durs[cnt++]=dur;
        } else if (ft) {
            snprintf(msgs[cnt],MSG_SZ,
                "FT: %s%s %d\xE2\x80\x93%d %s%s | %s",
                hf,m->home_code,m->home_score,
                m->away_score,af,m->away_code,m->group);
            durs[cnt++]=dur;
        }
    }

    /* ── Pass 2: all other live matches ── */
    for (int i=0; i<g_app.nmatches && cnt<MAX_DISP_MSG; i++) {
        NMatch *m=&g_app.matches[i];
        if (is_selected(m->home_code)||is_selected(m->away_code)) continue;
        int live=!strcmp(m->status,"1H")||!strcmp(m->status,"2H")||
                 !strcmp(m->status,"HT")||!strcmp(m->status,"ET")||
                 !strcmp(m->status,"PEN");
        if (!live) continue;
        char hf[16],af[16],cl[16];
        flag_for(m->home_code,hf); flag_for(m->away_code,af);
        fmt_clock(m,cl,sizeof(cl));
        snprintf(msgs[cnt],MSG_SZ,
            "\xE2\x9A\xBD %s%s %d\xE2\x80\x93%d %s%s | %s %s",
            hf,m->home_code,m->home_score,
            m->away_score,af,m->away_code,cl,m->group);
        durs[cnt++]=dur*2/3;
    }

    /* ── Pass 3: group standings for groups with selected teams ── */
    for (int g=0; g<N_GROUPS && cnt<MAX_DISP_MSG; g++) {
        if (g_app.nst[g]<2) continue;
        /* Does this group contain a selected team? */
        int want=0;
        if (g_app.nsel==0) want=1;
        else {
            for (int t=0; t<g_app.nst[g]; t++)
                if (is_selected(g_app.standings[g][t].code)) { want=1; break; }
        }
        if (!want) continue;
        /* derive group letter from first team */
        char gltr='?';
        int ti=team_by_code(g_app.standings[g][0].code);
        if (ti>=0) gltr=TEAMS[ti].grp;
        char line[MSG_SZ];
        int off=snprintf(line,MSG_SZ,"\xF0\x9F\x93\x8A Group %c: ",gltr);
        for (int t=0; t<g_app.nst[g]&&t<GPG; t++) {
            NStanding *s=&g_app.standings[g][t];
            char f[16]; flag_for(s->code,f);
            int rem=(int)MSG_SZ-off-1; if(rem<=0) break;
            off+=snprintf(line+off,rem,"%d.%s%s %dpts ",t+1,f,s->code,s->pts);
        }
        strncpy(msgs[cnt],line,MSG_SZ-1);
        durs[cnt++]=dur;
    }

    /* ── Pass 4: Golden Boot top 3 ── */
    if (g_app.nscorers>0 && cnt<MAX_DISP_MSG) {
        char line[MSG_SZ];
        int off=snprintf(line,MSG_SZ,
            "\xF0\x9F\x8F\x86 Golden Boot: ");
        int top=g_app.nscorers<3?g_app.nscorers:3;
        for (int i=0; i<top; i++) {
            NScorer *sc=&g_app.scorers[i];
            char f[16]; flag_for(sc->team_code,f);
            int rem=(int)MSG_SZ-off-1; if(rem<=0) break;
            const char *last=strrchr(sc->player,' ');
            const char *disp=last?last+1:sc->player;
            off+=snprintf(line+off,rem,"%s%s(%s) %d  ",f,disp,sc->team_code,sc->goals);
        }
        strncpy(msgs[cnt],line,MSG_SZ-1);
        durs[cnt++]=dur;
    }

    /* ── Pass 5: kickoff countdowns for selected teams not playing ── */
    for (int i=0; i<g_app.nmatches && cnt<MAX_DISP_MSG; i++) {
        NMatch *m=&g_app.matches[i];
        if (strcmp(m->status,"NS")!=0) continue;
        if (!is_selected(m->home_code)&&!is_selected(m->away_code)) continue;
        char hf[16],af[16],cd[32];
        flag_for(m->home_code,hf); flag_for(m->away_code,af);
        fmt_countdown(m->kickoff_iso,cd,sizeof(cd));
        snprintf(msgs[cnt],MSG_SZ,
            "\xE2\x8F\xB1 %s%s vs %s%s | KO in %s | %s",
            hf,m->home_code,af,m->away_code,cd,m->group);
        durs[cnt++]=dur;
    }

    /* ── Fallback splash ── */
    if (cnt==0) {
        const char *src=g_app.demo_mode?"Demo Mode":
                        g_app.last_src==SRC_AF?"api-football":
                        g_app.last_src==SRC_FD?"football-data":"--";
        snprintf(msgs[cnt],MSG_SZ,
            "\xF0\x9F\x8F\x86 FIFA World Cup 2026  |  %s",src);
        durs[cnt++]=dur;
    }

    pthread_mutex_lock(&g_dlock);
    g_dcount=cnt;
    for (int i=0;i<cnt;i++) {
        strncpy(g_dmsgs[i],msgs[i],MSG_SZ-1);
        g_ddur[i]=durs[i];
    }
    pthread_mutex_unlock(&g_dlock);
}

/* ── Goal event webhook ──────────────────────────────────────────
 * Called OUTSIDE g_app.lock.  Posts a JSON goal-event payload to
 * the configured webhook URL with one retry on failure.           */
static void fire_webhook(const NMatch *m) {
    if (!g_app.webhook_enabled || !g_app.webhook_url[0]) return;

    /* Build flags for home/away */
    char hf[16] = "", af[16] = "";
    flag_for(m->home_code, hf);
    flag_for(m->away_code, af);

    /* ISO-8601 timestamp */
    char ts[32];
    time_t now = time(NULL);
    struct tm *tm_utc = gmtime(&now);
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", tm_utc);

    /* Build payload with cJSON */
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "event",      "goal");
    cJSON_AddNumberToObject(root, "match_id",   (double)m->id);
    cJSON *home = cJSON_CreateObject();
    cJSON_AddStringToObject(home, "code",  m->home_code);
    cJSON_AddStringToObject(home, "flag",  hf);
    cJSON_AddNumberToObject(home, "score", (double)m->home_score);
    cJSON_AddItemToObject(root, "home", home);
    cJSON *away = cJSON_CreateObject();
    cJSON_AddStringToObject(away, "code",  m->away_code);
    cJSON_AddStringToObject(away, "flag",  af);
    cJSON_AddNumberToObject(away, "score", (double)m->away_score);
    cJSON_AddItemToObject(root, "away", away);
    cJSON_AddNumberToObject(root, "elapsed",    (double)m->elapsed);
    cJSON_AddStringToObject(root, "status",     m->status);
    cJSON_AddStringToObject(root, "group",      m->group);
    cJSON_AddStringToObject(root, "last_event", m->last_event);
    cJSON_AddStringToObject(root, "timestamp",  ts);
    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!payload) return;

    struct curl_slist *hdrs = curl_slist_append(NULL, "Content-Type: application/json");
    for (int attempt = 0; attempt < 2; attempt++) {
        CURL *c = curl_easy_init();
        if (!c) break;
        CurlBuf resp = {NULL, 0};
        curl_easy_setopt(c, CURLOPT_URL,           g_app.webhook_url);
        curl_easy_setopt(c, CURLOPT_POSTFIELDS,    payload);
        curl_easy_setopt(c, CURLOPT_HTTPHEADER,    hdrs);
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_write_cb);
        curl_easy_setopt(c, CURLOPT_WRITEDATA,     &resp);
        curl_easy_setopt(c, CURLOPT_TIMEOUT,       5L);
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER,1L);
        CURLcode rc = curl_easy_perform(c);
        long http_code = -1;
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http_code);
        curl_easy_cleanup(c);
        LOG("fire_webhook attempt=%d http=%ld rc=%d resp=%s",
            attempt+1, http_code, (int)rc, resp.data ? resp.data : "(none)");
        free(resp.data);
        if (rc == CURLE_OK && http_code >= 200 && http_code < 300) break;
    }
    curl_slist_free_all(hdrs);
    free(payload);
}

/* ═══════════════════════ Audio Pipeline ════════════════════════
 * Same approach proven in the MLB ACAP app:
 *   1. download_clip()       — fetch MP3 from device mediaclip store
 *   2. stream_scaled_clip()  — decode MP3 → resample → perceptual
 *                              gain → µ-law → POST transmit.cgi
 *   3. play_clip_ex()        — primary stream path + mediaclip fallback
 *   4. play_clip()           — fire-and-forget wrapper
 * Volume is NEVER set on the device output — it is baked into each
 * PCM sample using gain = (vol/100)^1.7 before µ-law re-encode.
 * ═══════════════════════════════════════════════════════════════*/

static int16_t ulaw_decode(uint8_t u) {
    u = ~u;
    int sign      = (u & 0x80) ? -1 : 1;
    int exponent  = (u >> 4) & 0x07;
    int mantissa  =  u & 0x0F;
    int magnitude = ((mantissa << 1) | 1) << exponent;
    return (int16_t)(sign * (magnitude - 1));
}

static uint8_t ulaw_encode(int16_t s) {
    int sign = (s >= 0) ? 0x80 : 0x00;
    int v    = (s < 0) ? -(int)s : (int)s;
    v += 33;
    if (v > 0x1FFF) v = 0x1FFF;
    int exp = 7;
    for (int mask = 0x1000; (v & mask) == 0 && exp > 0; exp--, mask >>= 1);
    int mant = (v >> (exp + 1)) & 0x0F;
    return (uint8_t)(~(sign | (exp << 4) | mant));
}

/* Returns byte offset to audio data, or -1 on error.
   Sets *encoding (1 = µ-law), *sample_rate, *channels. */
static int au_parse_header(const uint8_t *d, size_t len,
                            uint32_t *encoding, uint32_t *sample_rate,
                            uint32_t *channels) {
    if (len < 24) return -1;
    uint32_t magic  = ((uint32_t)d[0]<<24)|((uint32_t)d[1]<<16)|
                      ((uint32_t)d[2]<<8)|d[3];
    if (magic != 0x2E736E64u) return -1;   /* ".snd" */
    uint32_t offset = ((uint32_t)d[4]<<24)|((uint32_t)d[5]<<16)|
                      ((uint32_t)d[6]<<8)|d[7];
    if (encoding)    *encoding    = ((uint32_t)d[12]<<24)|((uint32_t)d[13]<<16)|
                                    ((uint32_t)d[14]<<8)|d[15];
    if (sample_rate) *sample_rate = ((uint32_t)d[16]<<24)|((uint32_t)d[17]<<16)|
                                    ((uint32_t)d[18]<<8)|d[19];
    if (channels)    *channels    = ((uint32_t)d[20]<<24)|((uint32_t)d[21]<<16)|
                                    ((uint32_t)d[22]<<8)|d[23];
    return (offset < len) ? (int)offset : -1;
}

/* Returns 1 if buffer starts with an MP3 sync word or ID3v2 tag. */
static int is_mp3(const uint8_t *d, size_t len) {
    if (len < 3) return 0;
    if (d[0] == 0x49 && d[1] == 0x44 && d[2] == 0x33) return 1; /* ID3v2 */
    if (d[0] == 0xFF && len >= 2) {
        uint8_t b = d[1];
        if ((b & 0xE0) == 0xE0 && (b & 0x06) == 0x02) return 1; /* MPEG sync */
    }
    return 0;
}

/* Resample interleaved int16 PCM src_rate→dst_rate with stereo→mono downmix.
   Returns a malloc'd mono buffer (*dst_frames_out samples). */
static int16_t *resample_to_mono(const int16_t *src, int channels,
                                  int src_frames, int src_rate, int dst_rate,
                                  int *dst_frames_out) {
    *dst_frames_out = (int)((int64_t)src_frames * dst_rate / src_rate);
    if (*dst_frames_out <= 0) return NULL;
    int16_t *out = malloc((size_t)(*dst_frames_out) * sizeof(int16_t));
    if (!out) return NULL;
    double ratio = (double)src_frames / (double)(*dst_frames_out);
    for (int i = 0; i < *dst_frames_out; i++) {
        double pos  = i * ratio;
        int    lo   = (int)pos;
        int    hi   = lo + 1 < src_frames ? lo + 1 : lo;
        double frac = pos - lo;
        double s_lo = 0.0, s_hi = 0.0;
        for (int ch = 0; ch < channels; ch++) {
            s_lo += src[lo * channels + ch];
            s_hi += src[hi * channels + ch];
        }
        double v = (s_lo + frac * (s_hi - s_lo)) / channels;
        if (v >  32767.0) v =  32767.0;
        if (v < -32768.0) v = -32768.0;
        out[i] = (int16_t)v;
    }
    return out;
}

/* Download a stored clip from the device mediaclip store. */
static int download_clip(int clip_id, uint8_t **out, size_t *out_len) {
    char url[MAX_URL], cred[128];
    snprintf(url,  sizeof(url),
        "http://127.0.0.1/axis-cgi/mediaclip.cgi?action=download&clip=%d", clip_id);
    snprintf(cred, sizeof(cred), "%s:%s", g_app.dev_user, g_app.dev_pass);

    CURL *c = curl_easy_init();
    if (!c) return 0;
    CurlBuf buf = {NULL, 0};
    curl_easy_setopt(c, CURLOPT_URL,           url);
    curl_easy_setopt(c, CURLOPT_USERPWD,       cred);
    curl_easy_setopt(c, CURLOPT_HTTPAUTH,      CURLAUTH_DIGEST);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA,     &buf);
    curl_easy_setopt(c, CURLOPT_TIMEOUT,       10L);
    CURLcode rc = curl_easy_perform(c);
    long http_code = -1;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(c);

    if (rc != CURLE_OK || http_code != 200 || !buf.data || buf.len < 4) {
        LOG("download_clip id=%d failed http=%ld rc=%d len=%zu",
            clip_id, http_code, (int)rc, buf.len);
        free(buf.data);
        return 0;
    }
    *out     = (uint8_t *)buf.data;
    *out_len = buf.len;
    return 1;
}

/* Download, volume-scale, and stream a clip via transmit.cgi.
   Handles both MP3 (minimp3) and µ-law .au.  Returns 1 on success. */
static int stream_scaled_clip(int clip_id, int volume_pct) {
    uint8_t *raw = NULL; size_t raw_len = 0;
    if (!download_clip(clip_id, &raw, &raw_len)) return 0;

    uint8_t *scaled = NULL; size_t n = 0;
    /* Perceptual gain: gain = (vol/100)^1.7
     *   50% → ≈0.31 (−10 dB, perceptually half as loud as max)
     *  100% → 1.0   (full volume) */
    float _v   = (float)volume_pct / 100.0f;
    float gain = (_v <= 0.0f) ? 0.0f : powf(_v, 1.7f);

    if (is_mp3(raw, raw_len)) {
        /* MP3 path: decode → resample → scale → µ-law */
        mp3dec_t           dec;
        mp3dec_file_info_t info;
        mp3dec_init(&dec);
        int err = mp3dec_load_buf(&dec, raw, raw_len, &info, NULL, NULL);
        free(raw);
        if (err || !info.buffer || info.samples == 0) {
            LOG("stream_scaled_clip: mp3 decode failed err=%d", err);
            free(info.buffer);
            return 0;
        }
        int src_frames = (int)(info.samples / (size_t)info.channels);
        int dst_frames = 0;
        int16_t *mono8k = resample_to_mono(info.buffer, info.channels,
                                           src_frames, info.hz, 8000,
                                           &dst_frames);
        free(info.buffer);
        if (!mono8k || dst_frames == 0) { free(mono8k); return 0; }
        n      = (size_t)dst_frames;
        scaled = malloc(n);
        if (!scaled) { free(mono8k); return 0; }
        for (int i = 0; i < dst_frames; i++) {
            int32_t s = (int32_t)((float)mono8k[i] * gain);
            if (s >  32767) s =  32767;
            if (s < -32768) s = -32768;
            scaled[i] = ulaw_encode((int16_t)s);
        }
        free(mono8k);
    } else {
        /* .au µ-law path */
        uint32_t encoding = 0, sample_rate = 8000, channels = 1;
        int data_off = au_parse_header(raw, raw_len,
                                       &encoding, &sample_rate, &channels);
        if (data_off < 0 || encoding != 1) {
            LOG("stream_scaled_clip: unsupported format (not mp3 or ulaw .au)");
            free(raw); return 0;
        }
        const uint8_t *src = raw + data_off;
        n      = raw_len - (size_t)data_off;
        scaled = malloc(n);
        if (!scaled) { free(raw); return 0; }
        for (size_t i = 0; i < n; i++) {
            int32_t s = (int32_t)((float)ulaw_decode(src[i]) * gain);
            if (s >  32767) s =  32767;
            if (s < -32768) s = -32768;
            scaled[i] = ulaw_encode((int16_t)s);
        }
        free(raw);
    }

    /* Stream scaled µ-law to transmit.cgi as audio/basic */
    char cred[128];
    snprintf(cred, sizeof(cred), "%s:%s", g_app.dev_user, g_app.dev_pass);

    struct curl_slist *hdrs = curl_slist_append(NULL, "Content-Type: audio/basic");
    CURL *c = curl_easy_init();
    if (!c) { free(scaled); curl_slist_free_all(hdrs); return 0; }

    CurlBuf resp = {NULL, 0};
    curl_easy_setopt(c, CURLOPT_URL,           "http://127.0.0.1/axis-cgi/audio/transmit.cgi");
    curl_easy_setopt(c, CURLOPT_USERPWD,       cred);
    curl_easy_setopt(c, CURLOPT_HTTPAUTH,      CURLAUTH_DIGEST);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER,    hdrs);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS,    (char *)scaled);
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)n);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA,     &resp);
    curl_easy_setopt(c, CURLOPT_TIMEOUT,       30L);

    CURLcode rc = curl_easy_perform(c);
    long http_code = -1;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http_code);
    LOG("stream_scaled_clip id=%d vol=%d%% n=%zu http=%ld rc=%d resp=%s",
        clip_id, volume_pct, n, http_code, (int)rc,
        resp.data ? resp.data : "(none)");
    free(resp.data);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(c);
    free(scaled);
    return (rc == CURLE_OK && http_code >= 200 && http_code < 300) ? 1 : 0;
}

/* Primary: stream scaled clip via transmit.cgi.
   Fallback: unscaled play via mediaclip.cgi.
   Device output gain is never modified. */
static long play_clip_ex(int clip_id, char *resp_out, size_t resp_sz) {
    int vol = g_app.audio_volume;
    if (vol < 0)   vol = 0;
    if (vol > 100) vol = 100;

    if (stream_scaled_clip(clip_id, vol)) {
        if (resp_out) snprintf(resp_out, resp_sz,
            "streamed clip=%d vol=%d%% via transmit.cgi", clip_id, vol);
        return 200;
    }

    /* Fallback: play via mediaclip.cgi (no volume control) */
    LOG("play_clip_ex: stream failed, falling back to mediaclip play id=%d", clip_id);
    char url[MAX_URL], cred[128];
    snprintf(url,  sizeof(url),  MEDIACLIP_API, clip_id);
    snprintf(cred, sizeof(cred), "%s:%s", g_app.dev_user, g_app.dev_pass);

    CURL *c = curl_easy_init();
    if (!c) {
        if (resp_out) snprintf(resp_out, resp_sz, "curl_easy_init failed");
        return -1;
    }
    CurlBuf buf = {NULL, 0};
    curl_easy_setopt(c, CURLOPT_URL,           url);
    curl_easy_setopt(c, CURLOPT_USERPWD,       cred);
    curl_easy_setopt(c, CURLOPT_HTTPAUTH,      CURLAUTH_DIGEST);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA,     &buf);
    curl_easy_setopt(c, CURLOPT_TIMEOUT,       5L);
    CURLcode rc = curl_easy_perform(c);
    long http_code = -1;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(c);
    if (rc != CURLE_OK) {
        LOG("play_clip fallback error: %s", curl_easy_strerror(rc));
        if (resp_out) snprintf(resp_out, resp_sz,
            "fallback curl error: %s", curl_easy_strerror(rc));
    } else {
        LOG("play_clip fallback id=%d http=%ld", clip_id, http_code);
        if (resp_out) snprintf(resp_out, resp_sz,
            "fallback http=%ld body=%s",
            http_code, buf.data ? buf.data : "(empty)");
    }
    free(buf.data);
    return (rc == CURLE_OK) ? http_code : -1;
}

static void play_clip(int clip_id) { play_clip_ex(clip_id, NULL, 0); }

/* ── VAPIX display ────────────────────────────────────────────── */
/* Returns the HTTP status code (200 = success), -1 on curl error.
 * If resp_out/resp_sz are non-NULL the raw API response is written there. */
static long display_show(const char *msg, char *resp_out, size_t resp_sz) {
    if (!g_app.disp_enabled) return 0;
    CURL *curl=curl_easy_init(); if(!curl) return -1;
    char up[160];
    snprintf(up,sizeof(up),"%s:%s",g_app.dev_user,g_app.dev_pass);

    cJSON *body=cJSON_CreateObject();
    cJSON *data=cJSON_CreateObject();
    cJSON_AddStringToObject(data,"message",msg);
    cJSON_AddStringToObject(data,"textColor",
        g_app.text_color[0]?g_app.text_color:"#FFFFFF");
    cJSON_AddStringToObject(data,"backgroundColor",
        g_app.bg_color[0]?g_app.bg_color:"#003F7F");
    cJSON_AddStringToObject(data,"textSize",
        g_app.text_size[0]?g_app.text_size:"large");
    cJSON_AddNumberToObject(data,"scrollSpeed",
        g_app.scroll_speed>0?g_app.scroll_speed:3);
    cJSON_AddStringToObject(data,"scrollDirection","fromRightToLeft");
    cJSON *dur=cJSON_CreateObject();
    cJSON_AddStringToObject(dur,"type","time");
    cJSON_AddNumberToObject(dur,"value",
        g_app.duration_ms>0?g_app.duration_ms:15000);
    cJSON_AddItemToObject(data,"duration",dur);
    cJSON_AddItemToObject(body,"data",data);
    char *js=cJSON_PrintUnformatted(body); cJSON_Delete(body);

    HBuf resp={malloc(4096),0,4096};
    struct curl_slist *hdrs=curl_slist_append(NULL,"Content-Type: application/json");
    curl_easy_setopt(curl,CURLOPT_URL,DISP_API);
    curl_easy_setopt(curl,CURLOPT_POST,1L);
    curl_easy_setopt(curl,CURLOPT_COPYPOSTFIELDS,js);
    curl_easy_setopt(curl,CURLOPT_USERPWD,up);
    curl_easy_setopt(curl,CURLOPT_HTTPAUTH,CURLAUTH_BASIC);
    curl_easy_setopt(curl,CURLOPT_SSL_VERIFYPEER,0L);
    curl_easy_setopt(curl,CURLOPT_SSL_VERIFYHOST,0L);
    curl_easy_setopt(curl,CURLOPT_HTTPHEADER,hdrs);
    curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,hbuf_cb);
    curl_easy_setopt(curl,CURLOPT_WRITEDATA,&resp);
    curl_easy_setopt(curl,CURLOPT_TIMEOUT,5L);
    CURLcode rc=curl_easy_perform(curl);
    long http_code=-1;
    if(rc==CURLE_OK)
        curl_easy_getinfo(curl,CURLINFO_RESPONSE_CODE,&http_code);
    LOG("display http=%ld curl=%d resp=%s",
        http_code,(int)rc,resp.data?resp.data:"(empty)");
    if(resp_out&&resp_sz>0)
        snprintf(resp_out,resp_sz,"http=%ld resp=%s",
                 http_code,resp.data?resp.data:"(empty)");
    free(js); free(resp.data);
    curl_slist_free_all(hdrs); curl_easy_cleanup(curl);
    return (rc==CURLE_OK)?http_code:-1;
}

/* ── Display flash (explicit colors — used by strobe_goal) ───── */
/* Pushes a single message with caller-supplied colors.  Does NOT
 * read g_app.text_color / bg_color, preventing races with strobe. */
static long display_flash(const char *msg, const char *bg, const char *fg,
                           int dur_ms) {
    if (!g_app.disp_enabled) return 0;
    CURL *curl = curl_easy_init(); if (!curl) return -1;
    char up[160];
    pthread_mutex_lock(&g_app.lock);
    snprintf(up, sizeof(up), "%s:%s", g_app.dev_user, g_app.dev_pass);
    int speed = g_app.scroll_speed > 0 ? g_app.scroll_speed : 3;
    pthread_mutex_unlock(&g_app.lock);

    cJSON *body = cJSON_CreateObject();
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "message",          msg);
    cJSON_AddStringToObject(data, "textColor",        fg[0] ? fg : "#FFFFFF");
    cJSON_AddStringToObject(data, "backgroundColor",  bg[0] ? bg : "#000000");
    cJSON_AddStringToObject(data, "textSize",         "large");
    cJSON_AddNumberToObject(data, "scrollSpeed",      (double)speed);
    cJSON_AddStringToObject(data, "scrollDirection",  "fromRightToLeft");
    cJSON *dur = cJSON_CreateObject();
    cJSON_AddStringToObject(dur,  "type",  "time");
    cJSON_AddNumberToObject(dur,  "value", (double)(dur_ms > 0 ? dur_ms : 2000));
    cJSON_AddItemToObject(data, "duration", dur);
    cJSON_AddItemToObject(body, "data", data);
    char *js = cJSON_PrintUnformatted(body); cJSON_Delete(body);

    HBuf resp = {malloc(4096), 0, 4096};
    struct curl_slist *hdrs = curl_slist_append(NULL, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_URL,           DISP_API);
    curl_easy_setopt(curl, CURLOPT_POST,          1L);
    curl_easy_setopt(curl, CURLOPT_COPYPOSTFIELDS,js);
    curl_easy_setopt(curl, CURLOPT_USERPWD,       up);
    curl_easy_setopt(curl, CURLOPT_HTTPAUTH,      CURLAUTH_BASIC);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER,0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST,0L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,    hdrs);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, hbuf_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,     &resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,       5L);
    CURLcode rc = curl_easy_perform(curl);
    long code = -1;
    if (rc == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    free(js); free(resp.data); curl_slist_free_all(hdrs); curl_easy_cleanup(curl);
    return (rc == CURLE_OK) ? code : -1;
}

/* ── Goal strobe (called OUTSIDE g_app.lock) ──────────────────── */
/* Flashes the display in the scoring team's kit colors.
 * Pauses the display thread via strobe_active to avoid interleave. */
static void strobe_goal(const NMatch *m, int side, int flashes) {
    if (!g_app.disp_enabled) return;
    if (flashes < 2)  flashes = 2;
    if (flashes > 10) flashes = 10;

    /* Scoring team kit colors */
    const char *code = (side == 0) ? m->home_code : m->away_code;
    int ti = team_by_code(code);
    const char *kb = "#FFD700";  /* fallback: gold */
    const char *kf = "#000000";
    if (ti >= 0) { kb = TEAMS[ti].bg; kf = TEAMS[ti].fg; }

    /* Goal flash message */
    char hf[16], af[16];
    flag_for(m->home_code, hf);
    flag_for(m->away_code, af);
    char msg[MSG_SZ];
    snprintf(msg, MSG_SZ,
        "\xE2\x9A\xBD GOAL! %s%s %d\xE2\x80\x93%d %s%s  %s",
        hf, m->home_code, m->home_score,
        m->away_score, af, m->away_code, m->group);

    /* Pause the display thread so it doesn't interleave */
    pthread_mutex_lock(&g_app.lock);
    g_app.strobe_active = 1;
    pthread_mutex_unlock(&g_app.lock);

    /* Alternating primary / inverted kit colors */
    for (int f = 0; f < flashes && g_app.running; f++) {
        const char *b = (f % 2 == 0) ? kb : kf;
        const char *t = (f % 2 == 0) ? kf : kb;
        display_flash(msg, b, t, 1800);
        usleep(1900000);
    }

    pthread_mutex_lock(&g_app.lock);
    g_app.strobe_active = 0;
    pthread_mutex_unlock(&g_app.lock);
    LOG("strobe_goal: %d flashes for %s (side=%d)", flashes, code, side);
}

/* ── Display thread ───────────────────────────────────────────── */
static void *display_thread(void *arg) {
    (void)arg;
    LOG("display thread started");
    while (g_app.running) {
        pthread_mutex_lock(&g_dlock);
        int cnt=g_dcount;
        if (cnt==0) { pthread_mutex_unlock(&g_dlock); sleep(2); continue; }
        int idx=g_didx%cnt;
        char msg[MSG_SZ]; int dur;
        strncpy(msg,g_dmsgs[idx],MSG_SZ-1); msg[MSG_SZ-1]='\0';
        dur=g_ddur[idx]; g_didx++;
        pthread_mutex_unlock(&g_dlock);

        pthread_mutex_lock(&g_app.lock);
        int go=g_app.enabled&&g_app.disp_enabled&&!g_app.strobe_active;
        pthread_mutex_unlock(&g_app.lock);

        if (go) {
            display_show(msg, NULL, 0);
            int w=0;
            while (g_app.running && w<dur) {
                int s=dur-w<500?dur-w:500;
                usleep(s*1000); w+=s;
            }
        } else { sleep(2); }
    }
    return NULL;
}

/* ── Data fetch cycle ─────────────────────────────────────────── */
static void do_fetch(void) {
    char *jbuf=malloc(JBUF); if(!jbuf) return;
    DataSrc src=SRC_NONE;

    /* Fixtures */
    memset(jbuf,0,JBUF);
    src=fetch_ep("/fixtures?league=1&season=2026",
                 "/competitions/WC/matches",
                 "fixtures_live.json", jbuf, JBUF);
    NMatch tm[MAX_MATCHES]; int nm=0;
    if (src==SRC_AF||src==SRC_MOCK) af_parse_fixtures(jbuf,tm,MAX_MATCHES,&nm);
    else if (src==SRC_FD)           fd_parse_matches(jbuf,tm,MAX_MATCHES,&nm);
    LOG("fixtures: %d matches, src=%d",nm,(int)src);

    /* Standings */
    NStanding tst[N_GROUPS][GPG]; int tnst[N_GROUPS];
    memset(tst,0,sizeof(tst)); memset(tnst,0,sizeof(tnst));
    memset(jbuf,0,JBUF);
    DataSrc ss=fetch_ep("/standings?league=1&season=2026",
                        "/competitions/WC/standings",
                        "standings.json", jbuf, JBUF);
    if (ss==SRC_AF||ss==SRC_MOCK) af_parse_standings(jbuf,tst,tnst);
    else if (ss==SRC_FD)          fd_parse_standings(jbuf,tst,tnst);

    /* Scorers */
    NScorer tsc[MAX_SCORERS]; int nsc=0;
    memset(jbuf,0,JBUF);
    DataSrc cs=fetch_ep("/players/topscorers?league=1&season=2026",
                        "/competitions/WC/scorers?limit=10",
                        "topscorers.json", jbuf, JBUF);
    if (cs==SRC_AF||cs==SRC_MOCK) af_parse_scorers(jbuf,tsc,MAX_SCORERS,&nsc);
    else if (cs==SRC_FD)          fd_parse_scorers(jbuf,tsc,MAX_SCORERS,&nsc);

    free(jbuf);

    pthread_mutex_lock(&g_app.lock);

    /* ── Goal detection for audio alerts + webhook ── */
    int goal_scored  = 0;
    int goal_side    = 0; /* 0=home scored, 1=away scored */
    int wh_enabled   = g_app.webhook_enabled;
    int str_enabled  = g_app.strobe_enabled;
    NMatch goal_match; memset(&goal_match, 0, sizeof(goal_match));
    for (int i = 0; i < nm; i++) {
        /* Only tracked teams trigger events */
        if (!is_selected(tm[i].home_code) && !is_selected(tm[i].away_code))
            continue;
        /* Only during live play */
        if (strcmp(tm[i].status,"1H")!=0 && strcmp(tm[i].status,"2H")!=0 &&
            strcmp(tm[i].status,"ET")!=0 && strcmp(tm[i].status,"PEN")!=0)
            continue;
        /* Compare against previous cached score — capture which side scored */
        for (int p = 0; p < g_app.nmatches; p++) {
            if (g_app.matches[p].id == tm[i].id) {
                if (tm[i].home_score > g_app.matches[p].home_score) {
                    goal_scored = 1; goal_side = 0;
                    goal_match  = tm[i]; /* capture by value before unlock */
                } else if (tm[i].away_score > g_app.matches[p].away_score) {
                    goal_scored = 1; goal_side = 1;
                    goal_match  = tm[i];
                }
                break;
            }
        }
    }

    if (nm>0||src!=SRC_NONE) {
        memcpy(g_app.matches,tm,nm*sizeof(NMatch));
        g_app.nmatches=nm;
    }
    if (ss!=SRC_NONE) {
        memcpy(g_app.standings,tst,sizeof(tst));
        memcpy(g_app.nst,tnst,sizeof(tnst));
    }
    if (cs!=SRC_NONE) {
        memcpy(g_app.scorers,tsc,nsc*sizeof(NScorer));
        g_app.nscorers=nsc;
    }
    g_app.last_poll=time(NULL);
    if (src!=SRC_NONE) g_app.last_src=src;
    else if (ss!=SRC_NONE) g_app.last_src=ss;
    else if (cs!=SRC_NONE) g_app.last_src=cs;
    rebuild_queue_locked();
    pthread_mutex_unlock(&g_app.lock);

    /* Fire audio / strobe / webhook outside the lock.
     * Resolve per-team overrides now (after unlock — helpers read g_app
     * without the lock, which is safe for int reads). */
    if (goal_scored) {
        const char *sc = (goal_side == 0) ? goal_match.home_code : goal_match.away_code;
        int goal_clip   = get_team_clip(sc);
        int str_flashes = get_team_flashes(sc);
        LOG("GOAL detected — side=%d team=%s clip=%d flashes=%d strobe=%d",
            goal_side, sc, goal_clip, str_flashes, str_enabled);
        if (g_app.audio_enabled && g_app.audio_volume > 0 && goal_clip > 0)
            play_clip(goal_clip);
        if (str_enabled) strobe_goal(&goal_match, goal_side, str_flashes);
        if (wh_enabled)  fire_webhook(&goal_match);
    }
}

/* ── Adaptive interval computation ───────────────────────────────
 * Inspects current match cache (caller holds g_app.lock) and
 * returns the appropriate poll interval in seconds.              */
static int compute_interval_locked(void) {
    time_t now = time(NULL);
    int mode = 0; /* 0=idle, 1=prematch, 2=live */

    for (int i = 0; i < g_app.nmatches; i++) {
        NMatch *m = &g_app.matches[i];
        if (!is_selected(m->home_code) && !is_selected(m->away_code)) continue;

        int live = !strcmp(m->status,"1H")||!strcmp(m->status,"2H")||
                   !strcmp(m->status,"ET")||!strcmp(m->status,"PEN");
        if (live)        { mode = 2; break; } /* highest priority */

        if (!strcmp(m->status,"NS") && mode < 1) {
            /* Check if kickoff is within 20 minutes */
            struct tm tm; memset(&tm,0,sizeof(tm));
            if (sscanf(m->kickoff_iso,"%d-%d-%dT%d:%d:%d",
                       &tm.tm_year,&tm.tm_mon,&tm.tm_mday,
                       &tm.tm_hour,&tm.tm_min,&tm.tm_sec) == 6) {
                tm.tm_year -= 1900; tm.tm_mon -= 1;
                time_t ko = timegm(&tm);
                if (ko > now && (ko - now) <= 1200) mode = 1;
            }
        }
    }

    int iv;
    if      (mode == 2) { iv = g_app.live_poll_sec;     strcpy(g_app.poll_mode,"live");     }
    else if (mode == 1) { iv = g_app.prematch_poll_sec;  strcpy(g_app.poll_mode,"prematch"); }
    else                { iv = g_app.idle_poll_sec;      strcpy(g_app.poll_mode,"idle");     }

    /* Hard floor: never faster than 30 s */
    if (iv < 30) iv = 30;

    /* Optional manual cap: if poll_sec > 0 and slower than adaptive, use it */
    if (g_app.poll_sec > 0 && g_app.poll_sec > iv) iv = g_app.poll_sec;

    g_app.effective_poll_sec = iv;
    return iv;
}

/* ── Poll thread ──────────────────────────────────────────────── */
static void *poll_thread(void *arg) {
    (void)arg;
    LOG("poll thread started (adaptive: live=%ds prematch=%ds idle=%ds)",
        g_app.live_poll_sec, g_app.prematch_poll_sec, g_app.idle_poll_sec);
    do_fetch(); /* immediate first fetch */
    while (g_app.running) {
        sleep(10);
        pthread_mutex_lock(&g_app.lock);
        int en = g_app.enabled;
        int iv = compute_interval_locked();
        time_t lp = g_app.last_poll;
        pthread_mutex_unlock(&g_app.lock);
        if (!en) continue;
        if (time(NULL) - lp >= iv) do_fetch();
        else {
            pthread_mutex_lock(&g_app.lock);
            rebuild_queue_locked();
            pthread_mutex_unlock(&g_app.lock);
        }
    }
    return NULL;
}

/* ═══════════════════════ HTTP Server ═══════════════════════════ */

static void send_json(int fd, int code, const char *body) {
    char hdr[256];
    snprintf(hdr,sizeof(hdr),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n\r\n",
        code, code==200?"OK":code==400?"Bad Request":"Error",
        strlen(body));
    send(fd,hdr,strlen(hdr),0);
    send(fd,body,strlen(body),0);
}

static void read_body(int fd, const char *hdrs, char *body, int bsz) {
    const char *cl=strcasestr(hdrs,"Content-Length:");
    int len=0; if(cl) len=atoi(cl+15);
    if(len<=0||len>=bsz) { body[0]='\0'; return; }
    int got=0;
    while (got<len) { int n=recv(fd,body+got,len-got,0); if(n<=0) break; got+=n; }
    body[got]='\0';
}

static void handle_client(int fd) {
    char req[4096]; int rlen=0, state=0; char c;
    while (rlen<(int)sizeof(req)-1) {
        if(recv(fd,&c,1,0)<=0) break;
        req[rlen++]=c;
        if(c=='\n') state++; else if(c!='\r') state=0;
        if(state>=2) break;
    }
    req[rlen]='\0';
    char method[8]="", path[256]="";
    sscanf(req,"%7s %255s",method,path);
    char *q=strchr(path,'?'); if(q)*q='\0';

    /* ROUTE: substring match so the VAPIX reverse-proxy path prefix
     * (e.g. "/api/status") resolves correctly regardless of what the
     * proxy strips.  Identical pattern to the working MLB app. */
    #define ROUTE(r) (strstr(path,(r))!=NULL)

    /* ── GET /status ── */
    if (!strcmp(method,"GET") && ROUTE("/status")) {
        pthread_mutex_lock(&g_app.lock);
        cJSON *root=cJSON_CreateObject();
        cJSON_AddBoolToObject(root,"enabled",g_app.enabled);
        cJSON_AddBoolToObject(root,"demo_mode",g_app.demo_mode);
        cJSON_AddStringToObject(root,"data_source",
            g_app.last_src==SRC_AF?"api-football":
            g_app.last_src==SRC_FD?"football-data":
            g_app.last_src==SRC_MOCK?"mock":"none");
        char ts[32]; strftime(ts,sizeof(ts),"%Y-%m-%dT%H:%M:%S",localtime(&g_app.last_poll));
        cJSON_AddStringToObject(root,"last_poll",ts);
        cJSON_AddStringToObject(root,"poll_mode",
            g_app.poll_mode[0] ? g_app.poll_mode : "idle");
        cJSON_AddNumberToObject(root,"effective_poll_sec",
            (double)(g_app.effective_poll_sec > 0 ? g_app.effective_poll_sec : g_app.idle_poll_sec));

        cJSON *marr=cJSON_CreateArray();
        for (int i=0;i<g_app.nmatches;i++) {
            NMatch *m=&g_app.matches[i];
            cJSON *o=cJSON_CreateObject();
            char hf[16],af[16]; flag_for(m->home_code,hf); flag_for(m->away_code,af);
            cJSON_AddNumberToObject(o,"id",m->id);
            cJSON_AddStringToObject(o,"home_code",m->home_code);
            cJSON_AddStringToObject(o,"home_flag",hf);
            cJSON_AddNumberToObject(o,"home_score",m->home_score);
            cJSON_AddStringToObject(o,"away_code",m->away_code);
            cJSON_AddStringToObject(o,"away_flag",af);
            cJSON_AddNumberToObject(o,"away_score",m->away_score);
            cJSON_AddStringToObject(o,"status",m->status);
            cJSON_AddNumberToObject(o,"elapsed",m->elapsed);
            cJSON_AddStringToObject(o,"group",m->group);
            cJSON_AddStringToObject(o,"kickoff",m->kickoff_iso);
            cJSON_AddStringToObject(o,"last_event",m->last_event);
            cJSON_AddStringToObject(o,"round",m->round);
            cJSON_AddBoolToObject(o,"tracked",
                is_selected(m->home_code)||is_selected(m->away_code));
            cJSON_AddItemToArray(marr,o);
        }
        cJSON_AddItemToObject(root,"matches",marr);

        cJSON *sarr=cJSON_CreateArray();
        int top=g_app.nscorers<5?g_app.nscorers:5;
        for (int i=0;i<top;i++) {
            cJSON *o=cJSON_CreateObject();
            char f[16]; flag_for(g_app.scorers[i].team_code,f);
            cJSON_AddNumberToObject(o,"rank",i+1);
            cJSON_AddStringToObject(o,"player",g_app.scorers[i].player);
            cJSON_AddStringToObject(o,"team",g_app.scorers[i].team_code);
            cJSON_AddStringToObject(o,"flag",f);
            cJSON_AddNumberToObject(o,"goals",g_app.scorers[i].goals);
            cJSON_AddItemToArray(sarr,o);
        }
        cJSON_AddItemToObject(root,"top_scorers",sarr);
        pthread_mutex_unlock(&g_app.lock);
        char *js=cJSON_PrintUnformatted(root); cJSON_Delete(root);
        send_json(fd,200,js); free(js); return;
    }

    /* ── GET /teams ── */
    if (!strcmp(method,"GET") && ROUTE("/teams")) {
        cJSON *root=cJSON_CreateObject();
        cJSON *arr=cJSON_CreateArray();
        for (int i=0;i<NUM_TEAMS;i++) {
            cJSON *o=cJSON_CreateObject();
            char f[16]; make_flag(TEAMS[i].iso2,f);
            cJSON_AddStringToObject(o,"code",TEAMS[i].code);
            cJSON_AddStringToObject(o,"name",TEAMS[i].name);
            cJSON_AddStringToObject(o,"flag",f);
            char g[3]={TEAMS[i].grp,'\0','\0'};
            cJSON_AddStringToObject(o,"group",g);
            cJSON_AddStringToObject(o,"bg",TEAMS[i].bg);
            cJSON_AddStringToObject(o,"fg",TEAMS[i].fg);
            cJSON_AddBoolToObject(o,"selected",is_selected(TEAMS[i].code));
            cJSON_AddItemToArray(arr,o);
        }
        cJSON_AddItemToObject(root,"teams",arr);
        char *js=cJSON_PrintUnformatted(root); cJSON_Delete(root);
        send_json(fd,200,js); free(js); return;
    }

    /* ── GET /standings ── */
    if (!strcmp(method,"GET") && ROUTE("/standings")) {
        pthread_mutex_lock(&g_app.lock);
        cJSON *root=cJSON_CreateObject();
        cJSON *garr=cJSON_CreateArray();
        for (int g=0;g<N_GROUPS;g++) {
            if (g_app.nst[g]<1) continue;
            char gl='?';
            int ti=team_by_code(g_app.standings[g][0].code);
            if(ti>=0) gl=TEAMS[ti].grp;
            cJSON *go=cJSON_CreateObject();
            char gs[3]={gl,'\0','\0'};
            cJSON_AddStringToObject(go,"group",gs);
            cJSON *tarr=cJSON_CreateArray();
            for (int t=0;t<g_app.nst[g];t++) {
                NStanding *s=&g_app.standings[g][t];
                cJSON *o=cJSON_CreateObject();
                char f[16]; flag_for(s->code,f);
                cJSON_AddNumberToObject(o,"rank",s->rank);
                cJSON_AddStringToObject(o,"code",s->code);
                cJSON_AddStringToObject(o,"flag",f);
                cJSON_AddNumberToObject(o,"played",s->played);
                cJSON_AddNumberToObject(o,"won",s->won);
                cJSON_AddNumberToObject(o,"drawn",s->drawn);
                cJSON_AddNumberToObject(o,"lost",s->lost);
                cJSON_AddNumberToObject(o,"gf",s->gf);
                cJSON_AddNumberToObject(o,"ga",s->ga);
                cJSON_AddNumberToObject(o,"gd",s->gd);
                cJSON_AddNumberToObject(o,"pts",s->pts);
                cJSON_AddBoolToObject(o,"tracked",is_selected(s->code));
                cJSON_AddItemToArray(tarr,o);
            }
            cJSON_AddItemToObject(go,"table",tarr);
            cJSON_AddItemToArray(garr,go);
        }
        cJSON_AddItemToObject(root,"groups",garr);
        pthread_mutex_unlock(&g_app.lock);
        char *js=cJSON_PrintUnformatted(root); cJSON_Delete(root);
        send_json(fd,200,js); free(js); return;
    }

    /* ── GET /bracket ── */
    if (!strcmp(method,"GET") && ROUTE("/bracket")) {
        /* Round display order */
        static const char *ROUND_ORDER[] =
            { "R32","R16","QF","SF","3rd Place","Final" };
        static const int N_ROUNDS = 6;

        pthread_mutex_lock(&g_app.lock);
        cJSON *root = cJSON_CreateObject();
        cJSON *rarr = cJSON_CreateArray();

        for (int ri = 0; ri < N_ROUNDS; ri++) {
            const char *rname = ROUND_ORDER[ri];
            cJSON *marr = cJSON_CreateArray();
            for (int i = 0; i < g_app.nmatches; i++) {
                NMatch *m = &g_app.matches[i];
                if (strcmp(m->round, rname) != 0) continue;
                cJSON *o = cJSON_CreateObject();
                char hf[16], af[16];
                flag_for(m->home_code, hf); flag_for(m->away_code, af);
                cJSON_AddNumberToObject(o,"id",           (double)m->id);
                cJSON_AddStringToObject(o,"home_code",    m->home_code);
                cJSON_AddStringToObject(o,"home_flag",    hf);
                cJSON_AddNumberToObject(o,"home_score",   (double)m->home_score);
                cJSON_AddStringToObject(o,"away_code",    m->away_code);
                cJSON_AddStringToObject(o,"away_flag",    af);
                cJSON_AddNumberToObject(o,"away_score",   (double)m->away_score);
                cJSON_AddStringToObject(o,"status",       m->status);
                cJSON_AddNumberToObject(o,"elapsed",      (double)m->elapsed);
                cJSON_AddStringToObject(o,"round",        m->round);
                cJSON_AddStringToObject(o,"kickoff",      m->kickoff_iso);
                cJSON_AddStringToObject(o,"last_event",   m->last_event);
                cJSON_AddBoolToObject(o,"tracked",
                    is_selected(m->home_code)||is_selected(m->away_code));
                cJSON_AddItemToArray(marr, o);
            }
            if (cJSON_GetArraySize(marr) > 0) {
                cJSON *ro = cJSON_CreateObject();
                cJSON_AddStringToObject(ro, "name",    rname);
                cJSON_AddItemToObject(ro,   "matches", marr);
                cJSON_AddItemToArray(rarr, ro);
            } else {
                cJSON_Delete(marr);
            }
        }
        cJSON_AddItemToObject(root, "rounds", rarr);
        pthread_mutex_unlock(&g_app.lock);
        char *js = cJSON_PrintUnformatted(root); cJSON_Delete(root);
        send_json(fd, 200, js ? js : "{}"); free(js); return;
    }

    /* ── GET /scorers ── */
    if (!strcmp(method,"GET") && ROUTE("/scorers")) {
        pthread_mutex_lock(&g_app.lock);
        cJSON *root=cJSON_CreateObject();
        cJSON *arr=cJSON_CreateArray();
        for (int i=0;i<g_app.nscorers;i++) {
            cJSON *o=cJSON_CreateObject();
            char f[16]; flag_for(g_app.scorers[i].team_code,f);
            cJSON_AddNumberToObject(o,"rank",i+1);
            cJSON_AddStringToObject(o,"player",g_app.scorers[i].player);
            cJSON_AddStringToObject(o,"team",g_app.scorers[i].team_code);
            cJSON_AddStringToObject(o,"flag",f);
            cJSON_AddNumberToObject(o,"goals",g_app.scorers[i].goals);
            cJSON_AddItemToArray(arr,o);
        }
        cJSON_AddItemToObject(root,"scorers",arr);
        pthread_mutex_unlock(&g_app.lock);
        char *js=cJSON_PrintUnformatted(root); cJSON_Delete(root);
        send_json(fd,200,js); free(js); return;
    }

    /* ── GET /config ── */
    if (!strcmp(method,"GET") && ROUTE("/config")) {
        pthread_mutex_lock(&g_app.lock);
        cJSON *root=cJSON_CreateObject();
        cJSON *sarr=cJSON_CreateArray();
        for (int i=0;i<g_app.nsel;i++)
            cJSON_AddItemToArray(sarr,cJSON_CreateString(g_app.sel[i]));
        cJSON_AddItemToObject(root,"selected_teams",sarr);
        cJSON_AddStringToObject(root,"af_key",g_app.af_key[0]?"***":"");
        cJSON_AddStringToObject(root,"fd_key",g_app.fd_key[0]?"***":"");
        cJSON_AddStringToObject(root,"device_user",g_app.dev_user);
        cJSON_AddNumberToObject(root,"poll_interval_sec",g_app.poll_sec);
        cJSON_AddNumberToObject(root,"live_poll_sec",g_app.live_poll_sec);
        cJSON_AddNumberToObject(root,"prematch_poll_sec",g_app.prematch_poll_sec);
        cJSON_AddNumberToObject(root,"idle_poll_sec",g_app.idle_poll_sec);
        cJSON_AddBoolToObject(root,"webhook_enabled",g_app.webhook_enabled);
        cJSON_AddStringToObject(root,"webhook_url",g_app.webhook_url[0]?"***":"");
        cJSON_AddBoolToObject(root,"strobe_enabled",g_app.strobe_enabled);
        cJSON_AddNumberToObject(root,"strobe_flashes",(double)g_app.strobe_flashes);
        cJSON *ovarr = cJSON_CreateArray();
        for (int i = 0; i < g_app.n_overrides; i++) {
            cJSON *ov = cJSON_CreateObject();
            cJSON_AddStringToObject(ov, "code",    g_app.team_overrides[i].code);
            cJSON_AddNumberToObject(ov, "clip_id", (double)g_app.team_overrides[i].clip_id);
            cJSON_AddNumberToObject(ov, "flashes", (double)g_app.team_overrides[i].flashes);
            cJSON_AddItemToArray(ovarr, ov);
        }
        cJSON_AddItemToObject(root, "team_overrides", ovarr);
        cJSON_AddBoolToObject(root,"enabled",g_app.enabled);
        cJSON_AddBoolToObject(root,"display_enabled",g_app.disp_enabled);
        cJSON_AddBoolToObject(root,"demo_mode",g_app.demo_mode);
        cJSON_AddBoolToObject(root,"audio_enabled",g_app.audio_enabled);
        cJSON_AddNumberToObject(root,"audio_volume",g_app.audio_volume);
        cJSON_AddNumberToObject(root,"goal_clip_id",g_app.goal_clip_id);
        cJSON_AddNumberToObject(root,"alert_clip_id",g_app.alert_clip_id);
        cJSON_AddStringToObject(root,"text_color",g_app.text_color);
        cJSON_AddStringToObject(root,"bg_color",g_app.bg_color);
        cJSON_AddStringToObject(root,"text_size",g_app.text_size);
        cJSON_AddNumberToObject(root,"scroll_speed",g_app.scroll_speed);
        cJSON_AddNumberToObject(root,"duration_ms",g_app.duration_ms);
        pthread_mutex_unlock(&g_app.lock);
        char *js=cJSON_PrintUnformatted(root); cJSON_Delete(root);
        send_json(fd,200,js); free(js); return;
    }

    /* ── POST /config ── */
    if (!strcmp(method,"POST") && ROUTE("/config")) {
        char body[8192]; read_body(fd,req,body,sizeof(body));
        cJSON *j=cJSON_Parse(body);
        if(!j) { send_json(fd,400,"{\"message\":\"bad json\"}"); return; }

        pthread_mutex_lock(&g_app.lock);

        cJSON *sel=cJSON_GetObjectItem(j,"selected_teams");
        if (cJSON_IsArray(sel)) {
            g_app.nsel=0;
            int n=cJSON_GetArraySize(sel);
            for (int i=0;i<n&&g_app.nsel<MAX_SEL;i++) {
                cJSON *c=cJSON_GetArrayItem(sel,i);
                if(cJSON_IsString(c)) {
                    strncpy(g_app.sel[g_app.nsel],c->valuestring,3);
                    g_app.sel[g_app.nsel][3]='\0'; g_app.nsel++;
                }
            }
        }

#define STR_FIELD(key,dst,sz) do { \
    cJSON *_f=cJSON_GetObjectItem(j,key); \
    if(cJSON_IsString(_f)&&strcmp(_f->valuestring,"***")!=0) \
        strncpy(dst,_f->valuestring,sz-1); } while(0)
#define BOOL_FIELD(key,dst) do { \
    cJSON *_f=cJSON_GetObjectItem(j,key); \
    if(cJSON_IsBool(_f)) dst=cJSON_IsTrue(_f); } while(0)
#define NUM_FIELD(key,dst) do { \
    cJSON *_f=cJSON_GetObjectItem(j,key); \
    if(cJSON_IsNumber(_f)) dst=(int)_f->valuedouble; } while(0)

        STR_FIELD("af_key",     g_app.af_key,    64);
        STR_FIELD("fd_key",     g_app.fd_key,    64);
        STR_FIELD("device_user",g_app.dev_user,  64);
        STR_FIELD("device_pass",g_app.dev_pass,  64);
        STR_FIELD("text_color", g_app.text_color,16);
        STR_FIELD("bg_color",   g_app.bg_color,  16);
        STR_FIELD("text_size",  g_app.text_size, 16);
        BOOL_FIELD("enabled",          g_app.enabled);
        BOOL_FIELD("display_enabled",  g_app.disp_enabled);
        BOOL_FIELD("demo_mode",        g_app.demo_mode);
        BOOL_FIELD("audio_enabled",    g_app.audio_enabled);
        BOOL_FIELD("webhook_enabled",  g_app.webhook_enabled);
        BOOL_FIELD("strobe_enabled",   g_app.strobe_enabled);
        NUM_FIELD("strobe_flashes",    g_app.strobe_flashes);
        if (g_app.strobe_flashes < 2)  g_app.strobe_flashes = 2;
        if (g_app.strobe_flashes > 10) g_app.strobe_flashes = 10;
        {   /* team_overrides array */
            cJSON *ovs = cJSON_GetObjectItem(j, "team_overrides");
            if (cJSON_IsArray(ovs)) {
                g_app.n_overrides = 0;
                int nov = cJSON_GetArraySize(ovs);
                for (int i = 0; i < nov && g_app.n_overrides < MAX_SEL; i++) {
                    cJSON *ov  = cJSON_GetArrayItem(ovs, i);
                    cJSON *ovc = cJSON_GetObjectItem(ov, "code");
                    cJSON *ovl = cJSON_GetObjectItem(ov, "clip_id");
                    cJSON *ovf = cJSON_GetObjectItem(ov, "flashes");
                    if (!cJSON_IsString(ovc)) continue;
                    TeamOverride *t = &g_app.team_overrides[g_app.n_overrides++];
                    strncpy(t->code, ovc->valuestring, 3); t->code[3] = '\0';
                    t->clip_id = cJSON_IsNumber(ovl) ? (int)ovl->valuedouble : 0;
                    t->flashes = cJSON_IsNumber(ovf) ? (int)ovf->valuedouble : 0;
                    if (t->clip_id < 0 || t->clip_id > 9)  t->clip_id = 0;
                    if (t->flashes != 0 && (t->flashes < 2 || t->flashes > 10)) t->flashes = 0;
                }
            }
        }
        {   /* webhook_url: only update if provided and not "***" */
            cJSON *_f = cJSON_GetObjectItem(j, "webhook_url");
            if (cJSON_IsString(_f) && strcmp(_f->valuestring,"***")!=0 && _f->valuestring[0])
                strncpy(g_app.webhook_url, _f->valuestring, sizeof(g_app.webhook_url)-1);
        }
        NUM_FIELD("poll_interval_sec",  g_app.poll_sec);
        NUM_FIELD("live_poll_sec",       g_app.live_poll_sec);
        NUM_FIELD("prematch_poll_sec",   g_app.prematch_poll_sec);
        NUM_FIELD("idle_poll_sec",       g_app.idle_poll_sec);
        if (g_app.live_poll_sec < 30)    g_app.live_poll_sec = 30;
        if (g_app.prematch_poll_sec < 30) g_app.prematch_poll_sec = 30;
        if (g_app.idle_poll_sec < 30)    g_app.idle_poll_sec = 30;
        NUM_FIELD("scroll_speed",    g_app.scroll_speed);
        NUM_FIELD("duration_ms",     g_app.duration_ms);
        NUM_FIELD("audio_volume",    g_app.audio_volume);
        NUM_FIELD("goal_clip_id",    g_app.goal_clip_id);
        NUM_FIELD("alert_clip_id",   g_app.alert_clip_id);
        if (g_app.audio_volume < 0)   g_app.audio_volume = 0;
        if (g_app.audio_volume > 100) g_app.audio_volume = 100;
#undef STR_FIELD
#undef BOOL_FIELD
#undef NUM_FIELD

        /* Persist */
        cJSON *cfg=cJSON_CreateObject();
        cJSON *sa2=cJSON_CreateArray();
        for(int i=0;i<g_app.nsel;i++) cJSON_AddItemToArray(sa2,cJSON_CreateString(g_app.sel[i]));
        cJSON_AddItemToObject(cfg,"selected_teams",sa2);
        cJSON_AddStringToObject(cfg,"af_key",     g_app.af_key);
        cJSON_AddStringToObject(cfg,"fd_key",     g_app.fd_key);
        cJSON_AddStringToObject(cfg,"device_user",g_app.dev_user);
        cJSON_AddStringToObject(cfg,"device_pass",g_app.dev_pass);
        cJSON_AddNumberToObject(cfg,"poll_interval_sec",g_app.poll_sec);
        cJSON_AddNumberToObject(cfg,"live_poll_sec",    g_app.live_poll_sec);
        cJSON_AddNumberToObject(cfg,"prematch_poll_sec",g_app.prematch_poll_sec);
        cJSON_AddNumberToObject(cfg,"idle_poll_sec",    g_app.idle_poll_sec);
        cJSON_AddBoolToObject(cfg,"webhook_enabled",    g_app.webhook_enabled);
        cJSON_AddStringToObject(cfg,"webhook_url",      g_app.webhook_url);
        cJSON_AddBoolToObject(cfg,"strobe_enabled",     g_app.strobe_enabled);
        cJSON_AddNumberToObject(cfg,"strobe_flashes",   (double)g_app.strobe_flashes);
        cJSON *ovarr2 = cJSON_CreateArray();
        for (int i = 0; i < g_app.n_overrides; i++) {
            cJSON *ov = cJSON_CreateObject();
            cJSON_AddStringToObject(ov, "code",    g_app.team_overrides[i].code);
            cJSON_AddNumberToObject(ov, "clip_id", (double)g_app.team_overrides[i].clip_id);
            cJSON_AddNumberToObject(ov, "flashes", (double)g_app.team_overrides[i].flashes);
            cJSON_AddItemToArray(ovarr2, ov);
        }
        cJSON_AddItemToObject(cfg, "team_overrides", ovarr2);
        cJSON_AddBoolToObject(cfg,"enabled",        g_app.enabled);
        cJSON_AddBoolToObject(cfg,"display_enabled",g_app.disp_enabled);
        cJSON_AddBoolToObject(cfg,"demo_mode",      g_app.demo_mode);
        cJSON_AddBoolToObject(cfg,"audio_enabled",  g_app.audio_enabled);
        cJSON_AddNumberToObject(cfg,"audio_volume",  g_app.audio_volume);
        cJSON_AddNumberToObject(cfg,"goal_clip_id",  g_app.goal_clip_id);
        cJSON_AddNumberToObject(cfg,"alert_clip_id", g_app.alert_clip_id);
        cJSON_AddStringToObject(cfg,"text_color",   g_app.text_color);
        cJSON_AddStringToObject(cfg,"bg_color",     g_app.bg_color);
        cJSON_AddStringToObject(cfg,"text_size",    g_app.text_size);
        cJSON_AddNumberToObject(cfg,"scroll_speed", g_app.scroll_speed);
        cJSON_AddNumberToObject(cfg,"duration_ms",  g_app.duration_ms);
        char *cs=cJSON_PrintUnformatted(cfg); cJSON_Delete(cfg);
        int save_ok=0;
        if(g_app.axp) {
            GError *err=NULL;
            save_ok=ax_parameter_set(g_app.axp,"Config",cs,TRUE,&err);
            if(err){
                LOG("ax_parameter_set error: %s",err->message);
                g_error_free(err);
            } else {
                LOG("config saved (%zu bytes)",strlen(cs));
            }
        } else {
            LOG("axp is NULL — config not persisted");
        }
        free(cs);
        g_app.last_poll=0; /* force refresh */
        pthread_mutex_unlock(&g_app.lock);
        cJSON_Delete(j);
        send_json(fd,200,save_ok?"{\"message\":\"Saved\"}":"{\"message\":\"Saved (runtime only — persist failed, check syslog)\"}");
        return;
    }

    /* ── POST /test_display ── */
    if (!strcmp(method,"POST") && ROUTE("/test_display")) {
        const char *tmsg =
            "\xF0\x9F\x8F\x86 FIFA World Cup 2026  "
            "\xE2\x9A\xBD USA \xF0\x9F\x87\xBA\xF0\x9F\x87\xB8"
            " 1\xE2\x80\x93" "0 ENG \xF0\x9F\x87\xAC\xF0\x9F\x87\xA7"
            " | 67' | Group A";
        char disp_resp[512]="";
        long http_code=display_show(tmsg,disp_resp,sizeof(disp_resp));
        cJSON *root=cJSON_CreateObject();
        cJSON_AddStringToObject(root,"message","ok");
        cJSON_AddNumberToObject(root,"display_http",(double)http_code);
        cJSON_AddStringToObject(root,"display_resp",disp_resp);
        char *js=cJSON_PrintUnformatted(root); cJSON_Delete(root);
        send_json(fd,200,js); free(js); return;
    }

    /* ── POST /refresh ── */
    if (!strcmp(method,"POST") && ROUTE("/refresh")) {
        pthread_mutex_lock(&g_app.lock); g_app.last_poll=0; pthread_mutex_unlock(&g_app.lock);
        send_json(fd,200,"{\"message\":\"Refresh queued\"}"); return;
    }

    /* ── POST /upload_clips ── */
    /* Uploads the bundled default audio clips to the device mediaclip store.
       Files land at INSTALL_DIR "audio/" after ACAP install. */
    if (!strcmp(method,"POST") && ROUTE("/upload_clips")) {
        static const struct { const char *file; const char *name; } BUNDLED[] = {
            { INSTALL_DIR "audio/goal_cheer.mp3", "FIFA WC Goal Cheer"  },
            { INSTALL_DIR "audio/goal_horn.mp3",  "FIFA WC Goal Horn"   },
        };
        int nb = (int)(sizeof(BUNDLED) / sizeof(BUNDLED[0]));

        pthread_mutex_lock(&g_app.lock);
        char user[64], pass[64];
        strncpy(user, g_app.dev_user, sizeof(user)-1); user[63]='\0';
        strncpy(pass, g_app.dev_pass, sizeof(pass)-1); pass[63]='\0';
        pthread_mutex_unlock(&g_app.lock);

        char cred[128];
        snprintf(cred, sizeof(cred), "%s:%s", user, pass);

        cJSON *clips_out = cJSON_CreateArray();
        for (int ci = 0; ci < nb; ci++) {
            int file_ok = (access(BUNDLED[ci].file, R_OK) == 0);
            LOG("upload_clips: file=%s exists=%s", BUNDLED[ci].file, file_ok?"yes":"no");

            cJSON *r = cJSON_CreateObject();
            cJSON_AddStringToObject(r, "name",      BUNDLED[ci].name);
            cJSON_AddStringToObject(r, "file_path", BUNDLED[ci].file);
            cJSON_AddStringToObject(r, "file_exists", file_ok ? "yes" : "no");

            if (!file_ok) {
                cJSON_AddNumberToObject(r, "http_code", -1);
                cJSON_AddStringToObject(r, "response",  "file not found on device");
                cJSON_AddItemToArray(clips_out, r);
                continue;
            }

            CURL *uc = curl_easy_init();
            char upload_url[512];
            char *enc_name = curl_easy_escape(uc, BUNDLED[ci].name, 0);
            snprintf(upload_url, sizeof(upload_url),
                     "http://127.0.0.1/axis-cgi/mediaclip.cgi?action=upload&name=%s",
                     enc_name ? enc_name : "");
            curl_free(enc_name);

            curl_mime     *mime = curl_mime_init(uc);
            curl_mimepart *part = curl_mime_addpart(mime);
            curl_mime_name(part, "clip");
            curl_mime_filedata(part, BUNDLED[ci].file);
            curl_mime_type(part, "audio/mpeg");

            CurlBuf resp = {NULL, 0};
            curl_easy_setopt(uc, CURLOPT_URL,           upload_url);
            curl_easy_setopt(uc, CURLOPT_USERPWD,       cred);
            curl_easy_setopt(uc, CURLOPT_HTTPAUTH,      CURLAUTH_DIGEST);
            curl_easy_setopt(uc, CURLOPT_MIMEPOST,      mime);
            curl_easy_setopt(uc, CURLOPT_WRITEFUNCTION, curl_write_cb);
            curl_easy_setopt(uc, CURLOPT_WRITEDATA,     &resp);
            curl_easy_setopt(uc, CURLOPT_TIMEOUT,       30L);

            CURLcode rc = curl_easy_perform(uc);
            long http_code = -1;
            curl_easy_getinfo(uc, CURLINFO_RESPONSE_CODE, &http_code);
            curl_easy_cleanup(uc);
            curl_mime_free(mime);

            LOG("upload_clips: %s http=%ld rc=%d resp=%s",
                BUNDLED[ci].name, http_code, (int)rc,
                resp.data ? resp.data : "(none)");

            cJSON_AddNumberToObject(r, "http_code",  (double)http_code);
            cJSON_AddNumberToObject(r, "curl_code",  (double)rc);
            cJSON_AddStringToObject(r, "curl_error", curl_easy_strerror(rc));
            cJSON_AddStringToObject(r, "response",   resp.data ? resp.data : "(none)");
            cJSON_AddItemToArray(clips_out, r);
            free(resp.data);
        }

        cJSON *root = cJSON_CreateObject();
        cJSON_AddItemToObject(root, "clips", clips_out);
        char *out_str = cJSON_PrintUnformatted(root);
        send_json(fd, 200, out_str ? out_str : "{}");
        free(out_str);
        cJSON_Delete(root);
        return;
    }

    /* ── GET /clips ── */
    /* Lists clips from device param.cgi → { clips: [{id, name}] }
       Parses "root.MediaClip.M{N}.Name={name}" key-value pairs from
       param.cgi (same approach as MLB ACAP app). */
    if (!strcmp(method,"GET") && ROUTE("/clips")) {
        pthread_mutex_lock(&g_app.lock);
        char user[64], pass[64];
        strncpy(user, g_app.dev_user, sizeof(user)-1); user[63]='\0';
        strncpy(pass, g_app.dev_pass, sizeof(pass)-1); pass[63]='\0';
        pthread_mutex_unlock(&g_app.lock);

        char cred[128];
        snprintf(cred, sizeof(cred), "%s:%s", user, pass);

        CURL *c = curl_easy_init();
        CurlBuf buf = {NULL, 0};
        curl_easy_setopt(c, CURLOPT_URL,
            "http://127.0.0.1/axis-cgi/param.cgi?action=list&group=root.MediaClip");
        curl_easy_setopt(c, CURLOPT_USERPWD,       cred);
        curl_easy_setopt(c, CURLOPT_HTTPAUTH,      CURLAUTH_DIGEST);
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_write_cb);
        curl_easy_setopt(c, CURLOPT_WRITEDATA,     &buf);
        curl_easy_setopt(c, CURLOPT_TIMEOUT,       8L);
        curl_easy_perform(c);
        curl_easy_cleanup(c);

        cJSON *root  = cJSON_CreateObject();
        cJSON *clips = cJSON_CreateArray();
        cJSON_AddItemToObject(root, "clips", clips);

        if (buf.data) {
            char *line = buf.data;
            while (*line) {
                char *nl = strpbrk(line, "\r\n");
                if (nl) *nl = '\0';
                int idx = 0; char name[128] = {0};
                if (sscanf(line, "root.MediaClip.M%d.Name=%127[^\r\n]",
                           &idx, name) == 2) {
                    cJSON *clip = cJSON_CreateObject();
                    cJSON_AddNumberToObject(clip, "id",   (double)idx);
                    cJSON_AddStringToObject(clip, "name", name);
                    cJSON_AddItemToArray(clips, clip);
                }
                if (!nl) break;
                line = nl + 1;
                while (*line == '\r' || *line == '\n') line++;
            }
        }

        /* Fallback: return a placeholder when no clips are found */
        if (cJSON_GetArraySize(clips) == 0) {
            cJSON *clip = cJSON_CreateObject();
            cJSON_AddNumberToObject(clip, "id",   1.0);
            cJSON_AddStringToObject(clip, "name", "Default Notification");
            cJSON_AddItemToArray(clips, clip);
        }

        free(buf.data);
        char *js = cJSON_PrintUnformatted(root); cJSON_Delete(root);
        send_json(fd, 200, js ? js : "{}"); free(js); return;
    }

    /* ── POST /test_audio ── */
    /* Plays the goal clip at the configured volume.  Body (optional):
       {"clip_id": N, "volume": V}  — overrides clip and/or volume for the test. */
    if (!strcmp(method,"POST") && ROUTE("/test_audio")) {
        char body[512]; read_body(fd, req, body, sizeof(body));
        cJSON *j = cJSON_Parse(body);

        pthread_mutex_lock(&g_app.lock);
        int clip_id = g_app.goal_clip_id;
        int vol     = g_app.audio_volume;
        pthread_mutex_unlock(&g_app.lock);

        if (j) {
            cJSON *ci = cJSON_GetObjectItem(j, "clip_id");
            cJSON *vl = cJSON_GetObjectItem(j, "volume");
            if (cJSON_IsNumber(ci)) clip_id = (int)ci->valuedouble;
            if (cJSON_IsNumber(vl)) {
                vol = (int)vl->valuedouble;
                if (vol < 0)   vol = 0;
                if (vol > 100) vol = 100;
            }
            cJSON_Delete(j);
        }

        if (clip_id <= 0) {
            send_json(fd, 400,
                "{\"message\":\"No clip configured — run Upload Clips first\"}");
            return;
        }

        char clip_resp[512] = "";
        long clip_http = play_clip_ex(clip_id, clip_resp, sizeof(clip_resp));

        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "message",   "ok");
        cJSON_AddNumberToObject(root, "clip_id",   (double)clip_id);
        cJSON_AddNumberToObject(root, "volume",    (double)vol);
        cJSON_AddNumberToObject(root, "clip_http", (double)clip_http);
        cJSON_AddStringToObject(root, "clip_resp", clip_resp);
        char *js = cJSON_PrintUnformatted(root); cJSON_Delete(root);
        send_json(fd, 200, js ? js : "{}"); free(js); return;
    }

    /* ── POST /test_webhook ── */
    if (!strcmp(method,"POST") && ROUTE("/test_webhook")) {
        pthread_mutex_lock(&g_app.lock);
        char wurl[256]; strncpy(wurl, g_app.webhook_url, sizeof(wurl)-1); wurl[255]='\0';
        pthread_mutex_unlock(&g_app.lock);

        if (!wurl[0]) {
            send_json(fd,400,"{\"message\":\"No webhook URL configured\"}");
            return;
        }

        /* Build a dummy goal payload */
        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root,"event","goal_test");
        cJSON_AddNumberToObject(root,"match_id",999);
        cJSON *home=cJSON_CreateObject();
        cJSON_AddStringToObject(home,"code","USA"); cJSON_AddStringToObject(home,"flag","\xF0\x9F\x87\xBA\xF0\x9F\x87\xB8");
        cJSON_AddNumberToObject(home,"score",1); cJSON_AddItemToObject(root,"home",home);
        cJSON *away=cJSON_CreateObject();
        cJSON_AddStringToObject(away,"code","ENG"); cJSON_AddStringToObject(away,"flag","\xF0\x9F\x87\xAC\xF0\x9F\x87\xA7");
        cJSON_AddNumberToObject(away,"score",0); cJSON_AddItemToObject(root,"away",away);
        cJSON_AddNumberToObject(root,"elapsed",45);
        cJSON_AddStringToObject(root,"status","1H");
        cJSON_AddStringToObject(root,"group","Group A");
        cJSON_AddStringToObject(root,"last_event","\xE2\x9A\xBD GOAL 45' Pulisic (USA)");
        char ts[32]; time_t now=time(NULL);
        strftime(ts,sizeof(ts),"%Y-%m-%dT%H:%M:%SZ",gmtime(&now));
        cJSON_AddStringToObject(root,"timestamp",ts);
        char *payload=cJSON_PrintUnformatted(root); cJSON_Delete(root);

        struct curl_slist *hdrs=curl_slist_append(NULL,"Content-Type: application/json");
        CURL *c=curl_easy_init();
        CurlBuf resp={NULL,0};
        curl_easy_setopt(c,CURLOPT_URL,           wurl);
        curl_easy_setopt(c,CURLOPT_POSTFIELDS,    payload);
        curl_easy_setopt(c,CURLOPT_HTTPHEADER,    hdrs);
        curl_easy_setopt(c,CURLOPT_WRITEFUNCTION, curl_write_cb);
        curl_easy_setopt(c,CURLOPT_WRITEDATA,     &resp);
        curl_easy_setopt(c,CURLOPT_TIMEOUT,       5L);
        curl_easy_setopt(c,CURLOPT_SSL_VERIFYPEER,1L);
        CURLcode rc=curl_easy_perform(c);
        long http_code=-1;
        curl_easy_getinfo(c,CURLINFO_RESPONSE_CODE,&http_code);
        curl_easy_cleanup(c); curl_slist_free_all(hdrs); free(payload);
        LOG("test_webhook http=%ld rc=%d", http_code, (int)rc);

        cJSON *out=cJSON_CreateObject();
        cJSON_AddNumberToObject(out,"http_code",(double)http_code);
        cJSON_AddNumberToObject(out,"curl_code",(double)rc);
        cJSON_AddStringToObject(out,"curl_error",curl_easy_strerror(rc));
        cJSON_AddStringToObject(out,"response",resp.data?resp.data:"(none)");
        char *js=cJSON_PrintUnformatted(out); cJSON_Delete(out);
        free(resp.data);
        send_json(fd,200,js?js:"{}"); free(js); return;
    }

    LOG("404 method=%s path=%s",method,path);
    send_json(fd,404,"{\"message\":\"Not found\"}");
}

static void *http_thread(void *arg) {
    (void)arg;
    int srv=socket(AF_INET,SOCK_STREAM,0); if(srv<0) return NULL;
    int opt=1; setsockopt(srv,SOL_SOCKET,SO_REUSEADDR,&opt,sizeof(opt));
    struct sockaddr_in addr; memset(&addr,0,sizeof(addr));
    addr.sin_family=AF_INET; addr.sin_addr.s_addr=INADDR_ANY;
    addr.sin_port=htons(HTTP_PORT);
    if(bind(srv,(struct sockaddr*)&addr,sizeof(addr))<0) { close(srv); return NULL; }
    listen(srv,8);
    LOG("HTTP server port %d",HTTP_PORT);
    while (g_app.running) {
        int fd=accept(srv,NULL,NULL); if(fd<0) continue;
        handle_client(fd); close(fd);
    }
    close(srv); return NULL;
}

/* ═══════════════════ Config persistence ════════════════════════ */

static void load_config(void) {
    /* defaults */
    strcpy(g_app.dev_user,"root");
    g_app.poll_sec=STD_POLL_SEC; g_app.enabled=1;
    g_app.disp_enabled=1; g_app.demo_mode=1;
    strcpy(g_app.text_color,"#FFFFFF"); strcpy(g_app.bg_color,"#003F7F");
    strcpy(g_app.text_size,"large"); g_app.scroll_speed=3; g_app.duration_ms=15000;
    g_app.audio_enabled=1; g_app.audio_volume=75;
    g_app.goal_clip_id=1;  g_app.alert_clip_id=2;
    g_app.live_poll_sec=30; g_app.prematch_poll_sec=90; g_app.idle_poll_sec=300;
    strcpy(g_app.poll_mode,"idle"); g_app.effective_poll_sec=300;
    g_app.webhook_enabled=0;
    g_app.strobe_enabled=1; g_app.strobe_flashes=5;

    if (!g_app.axp) return;
    GError *err=NULL; gchar *val=NULL;
    ax_parameter_get(g_app.axp,"Config",&val,&err);
    if (err||!val) { if(err)g_error_free(err); return; }

    cJSON *j=cJSON_Parse(val); g_free(val);
    if (!j) return;

    cJSON *sel=cJSON_GetObjectItem(j,"selected_teams");
    if (cJSON_IsArray(sel)) {
        g_app.nsel=0;
        int n=cJSON_GetArraySize(sel);
        for (int i=0;i<n&&g_app.nsel<MAX_SEL;i++) {
            cJSON *c=cJSON_GetArrayItem(sel,i);
            if(cJSON_IsString(c)){strncpy(g_app.sel[g_app.nsel],c->valuestring,3);g_app.sel[g_app.nsel][3]='\0';g_app.nsel++;}
        }
    }

#define LS(k,d,s) do{cJSON *_f=cJSON_GetObjectItem(j,k);if(cJSON_IsString(_f))strncpy(d,_f->valuestring,s-1);}while(0)
#define LB(k,d)   do{cJSON *_f=cJSON_GetObjectItem(j,k);if(cJSON_IsBool(_f))d=cJSON_IsTrue(_f);}while(0)
#define LN(k,d)   do{cJSON *_f=cJSON_GetObjectItem(j,k);if(cJSON_IsNumber(_f))d=(int)_f->valuedouble;}while(0)
    LS("af_key",     g_app.af_key,    64);
    LS("fd_key",     g_app.fd_key,    64);
    LS("device_user",g_app.dev_user,  64);
    LS("device_pass",g_app.dev_pass,  64);
    LS("text_color", g_app.text_color,16);
    LS("bg_color",   g_app.bg_color,  16);
    LS("text_size",  g_app.text_size, 16);
    LB("enabled",        g_app.enabled);
    LB("display_enabled",g_app.disp_enabled);
    LB("demo_mode",      g_app.demo_mode);
    LB("audio_enabled",      g_app.audio_enabled);
    LB("webhook_enabled",    g_app.webhook_enabled);
    LS("webhook_url",        g_app.webhook_url,   256);
    LB("strobe_enabled",     g_app.strobe_enabled);
    LN("strobe_flashes",     g_app.strobe_flashes);
    {
        cJSON *ovs = cJSON_GetObjectItem(j, "team_overrides");
        if (cJSON_IsArray(ovs)) {
            g_app.n_overrides = 0;
            int nov = cJSON_GetArraySize(ovs);
            for (int i = 0; i < nov && g_app.n_overrides < MAX_SEL; i++) {
                cJSON *ov  = cJSON_GetArrayItem(ovs, i);
                cJSON *ovc = cJSON_GetObjectItem(ov, "code");
                cJSON *ovl = cJSON_GetObjectItem(ov, "clip_id");
                cJSON *ovf = cJSON_GetObjectItem(ov, "flashes");
                if (!cJSON_IsString(ovc)) continue;
                TeamOverride *t = &g_app.team_overrides[g_app.n_overrides++];
                strncpy(t->code, ovc->valuestring, 3); t->code[3] = '\0';
                t->clip_id = cJSON_IsNumber(ovl) ? (int)ovl->valuedouble : 0;
                t->flashes = cJSON_IsNumber(ovf) ? (int)ovf->valuedouble : 0;
            }
        }
    }
    LN("poll_interval_sec",  g_app.poll_sec);
    LN("live_poll_sec",      g_app.live_poll_sec);
    LN("prematch_poll_sec",  g_app.prematch_poll_sec);
    LN("idle_poll_sec",      g_app.idle_poll_sec);
    LN("scroll_speed",       g_app.scroll_speed);
    LN("duration_ms",        g_app.duration_ms);
    LN("audio_volume",       g_app.audio_volume);
    LN("goal_clip_id",       g_app.goal_clip_id);
    LN("alert_clip_id",      g_app.alert_clip_id);
#undef LS
#undef LB
#undef LN
    cJSON_Delete(j);
    LOG("config loaded: nsel=%d demo=%d",g_app.nsel,g_app.demo_mode);
}

/* ═══════════════════════════ Main ══════════════════════════════ */

static volatile int g_quit=0;
static void on_sig(int s) { (void)s; g_quit=1; g_app.running=0; }

int main(void) {
    LOG("FIFA World Cup 2026 Ticker v%s",APP_VER);
    signal(SIGTERM,on_sig); signal(SIGINT,on_sig);
    curl_global_init(CURL_GLOBAL_DEFAULT);
    memset(&g_app,0,sizeof(g_app));
    pthread_mutex_init(&g_app.lock,NULL);
    g_app.running=1;

    GError *err=NULL;
    g_app.axp=ax_parameter_new(APP_NAME,&err);
    if(err){LOG("axparam: %s",err->message);g_error_free(err);}

    load_config();
    LOG("demo=%d nsel=%d",g_app.demo_mode,g_app.nsel);

    pthread_t ht,pt,dt;
    pthread_create(&ht,NULL,http_thread,   NULL);
    pthread_create(&pt,NULL,poll_thread,   NULL);
    pthread_create(&dt,NULL,display_thread,NULL);

    while (g_app.running) sleep(1);

    pthread_join(pt,NULL);
    pthread_join(dt,NULL);
    pthread_join(ht,NULL);

    if(g_app.axp) ax_parameter_free(g_app.axp);
    curl_global_cleanup();
    pthread_mutex_destroy(&g_app.lock);
    LOG("exit");
    return 0;
}
