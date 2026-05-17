/*
 *    ██╗  █████╗ ██████╗ ██╗   ██╗██╗███████╗
 *    ██║ ██╔══██╗██╔══██╗██║   ██║██║██╔════╝
 *    ██║ ███████║██████╔╝██║   ██║██║███████╗
 * ██ ██║ ██╔══██║██╔══██╗╚██╗ ██╔╝██║╚════██║
 * █████║ ██║  ██║██║  ██║ ╚████╔╝ ██║███████║
 * ╚════╝ ╚═╝  ╚═╝╚═╝  ╚═╝  ╚═══╝  ╚═╝╚══════╝
 *
 *  AI-powered terminal assistant for Linux
 *  https://github.com/Tehns/Jarvis
 *
 *  Usage:
 *    jarvis                   interactive mode
 *    jarvis --version         print version
 *    jarvis --help            print help
 *    jarvis explain           read stderr from stdin and explain it
 *    jarvis alias             scan history and suggest aliases
 *    jarvis update            update jarvis to the latest version
 *    jarvis watch <cmd>       run a command, explain errors live
 *    jarvis "find big files"  one-shot command suggestion
 *
 *  Pipe usage:
 *    make 2>&1 | jarvis explain
 *    cargo build 2>&1 | jarvis explain
 *    gcc foo.c 2>&1 | jarvis explain
 */

#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <curl/curl.h>

/* ─── ANSI colors ─────────────────────────────────────────────────────────── */
#define RED     "\033[31m"
#define GREEN   "\033[32m"
#define BLUE    "\033[34m"
#define YELLOW  "\033[33m"
#define CYAN    "\033[36m"
#define MAGENTA "\033[35m"
#define BOLD    "\033[1m"
#define DIM     "\033[2m"
#define RESET   "\033[0m"

/* ─── Constants ───────────────────────────────────────────────────────────── */
#define VERSION         "2.2.0"
#define MAX_HISTORY     400
#define MAX_LINE        512
#define MAX_PATH        1024
#define MAX_CONFIG_VAL  256
#define ALIAS_TOP       15
#define EXPLAIN_MAX     16384
#define GEMINI_URL      "https://generativelanguage.googleapis.com/v1beta/models/gemini-2.5-flash:generateContent?key=%s"

/* ─── Structs ─────────────────────────────────────────────────────────────── */
typedef struct { char api_key[MAX_CONFIG_VAL]; char city[MAX_CONFIG_VAL]; char history_path[MAX_PATH]; } Config;
typedef struct { char *data; size_t size; } Response;
typedef struct { char cmds[MAX_HISTORY][MAX_LINE]; int count; } History;
typedef struct { char cmd[MAX_LINE]; int count; } CmdFreq;

/* ─── Forward declarations ────────────────────────────────────────────────── */
static int split_args(char *working_buf, char **args, int max_args);

/* ══════════════════════════════════════════════════════════════════════════════
 * HTTP
 * ══════════════════════════════════════════════════════════════════════════════ */

static size_t http_cb(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t total = size * nmemb;
    Response *r  = (Response *)userp;
    char *ptr    = realloc(r->data, r->size + total + 1);
    if (!ptr) return 0;
    r->data = ptr;
    memcpy(r->data + r->size, contents, total);
    r->size += total;
    r->data[r->size] = '\0';
    return total;
}

static Response *http_post(const char *url, const char *body) {
    Response *r = malloc(sizeof(Response));
    r->data = malloc(1); r->size = 0;
    CURL *curl = curl_easy_init();
    if (!curl) { free(r->data); free(r); return NULL; }
    struct curl_slist *hdrs = curl_slist_append(NULL, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_URL,           url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,    hdrs);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS,    body);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, http_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,     r);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,       15L);
    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(hdrs); curl_easy_cleanup(curl);
    if (res != CURLE_OK) {
        fprintf(stderr, RED "curl error: %s\n" RESET, curl_easy_strerror(res));
        free(r->data); free(r); return NULL;
    }
    return r;
}

static Response *http_get(const char *url) {
    Response *r = malloc(sizeof(Response));
    r->data = malloc(1); r->size = 0;
    CURL *curl = curl_easy_init();
    if (!curl) { free(r->data); free(r); return NULL; }
    curl_easy_setopt(curl, CURLOPT_URL,            url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  http_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,      r);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        10L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    if (res != CURLE_OK) {
        fprintf(stderr, RED "curl error: %s\n" RESET, curl_easy_strerror(res));
        free(r->data); free(r); return NULL;
    }
    return r;
}

/* ══════════════════════════════════════════════════════════════════════════════
 * CONFIG
 * ══════════════════════════════════════════════════════════════════════════════ */

static const char *safe_home(void) {
    const char *home = getenv("HOME");
    if (home && home[0]) return home;
    struct passwd *pw = getpwuid(getuid());
    if (pw && pw->pw_dir) return pw->pw_dir;
    return "/tmp";
}

static void config_path(char *buf, size_t len) {
    snprintf(buf, len, "%s/.config/jarvis/config", safe_home());
}

static void detect_history(char *buf, size_t len) {
    const char *home = safe_home();
    const char *hf = getenv("HISTFILE");
    if (hf && access(hf, R_OK) == 0) { strncpy(buf, hf, len-1); return; }
    const char *sh = getenv("SHELL");
    char c[4][MAX_PATH];
    snprintf(c[0], MAX_PATH, "%s/.zsh_history",  home);
    snprintf(c[1], MAX_PATH, "%s/.bash_history", home);
    snprintf(c[2], MAX_PATH, "%s/.local/share/fish/fish_history", home);
    snprintf(c[3], MAX_PATH, "%s/.history",      home);
    if (sh) {
        if (strstr(sh,"zsh")  && access(c[0],R_OK)==0) { strncpy(buf,c[0],len-1); return; }
        if (strstr(sh,"bash") && access(c[1],R_OK)==0) { strncpy(buf,c[1],len-1); return; }
        if (strstr(sh,"fish") && access(c[2],R_OK)==0) { strncpy(buf,c[2],len-1); return; }
    }
    for (int i=0; i<4; i++) if (access(c[i],R_OK)==0) { strncpy(buf,c[i],len-1); return; }
    strncpy(buf, c[1], len-1);
}

static int config_load(Config *cfg) {
    char path[MAX_PATH]; config_path(path, sizeof(path));
    FILE *f = fopen(path, "r"); if (!f) return 0;
    char line[MAX_PATH];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line,"\n")] = '\0';
        if (line[0]=='#'||line[0]=='\0') continue;
        char k[64], v[MAX_CONFIG_VAL];
        if (sscanf(line, "%63[^=]=%255[^\n]", k, v) == 2) {
            if (!strcmp(k,"api_key"))      strncpy(cfg->api_key,      v, MAX_CONFIG_VAL-1);
            if (!strcmp(k,"city"))         strncpy(cfg->city,         v, MAX_CONFIG_VAL-1);
            if (!strcmp(k,"history_path")) strncpy(cfg->history_path, v, MAX_PATH-1);
        }
    }
    fclose(f); return 1;
}

static int config_init(Config *cfg) {
    memset(cfg, 0, sizeof(Config));
    strcpy(cfg->city, "Kharkiv");
    detect_history(cfg->history_path, sizeof(cfg->history_path));

    if (!config_load(cfg)) {
        /* Create default config */
        char dir[MAX_PATH]; snprintf(dir, sizeof(dir), "%s/.config/jarvis", safe_home());
        mkdir(dir, 0700);
        char path[MAX_PATH]; config_path(path, sizeof(path));
        FILE *f = fopen(path, "w");
        if (f) {
            fprintf(f, "# Jarvis config - https://github.com/Tehns/Jarvis\n"
                       "# Get a free key at: https://aistudio.google.com\n\n"
                       "api_key=YOUR_GEMINI_API_KEY\ncity=Kharkiv\nhistory_path=%s\n",
                       cfg->history_path);
            fclose(f);
        }
        printf(YELLOW BOLD "First run! Config created: %s\n" RESET, path);
        printf(YELLOW "Add your Gemini API key and restart.\n" RESET);
        printf(DIM    "  Free key: https://aistudio.google.com\n\n" RESET);
        return 0;
    }
    if (!strcmp(cfg->api_key,"YOUR_GEMINI_API_KEY") || cfg->api_key[0]=='\0') {
        char path[MAX_PATH]; config_path(path, sizeof(path));
        fprintf(stderr, RED "No API key in %s\n" RESET, path);
        fprintf(stderr, DIM "  Free key: https://aistudio.google.com\n" RESET);
        return 0;
    }
    return 1;
}

/* ══════════════════════════════════════════════════════════════════════════════
 * HISTORY
 * ══════════════════════════════════════════════════════════════════════════════ */

static void strip_fish(char *l) {
    if (!strncmp(l, "- cmd: ", 7)) { memmove(l, l+7, strlen(l+7)+1); return; }
    /* Suppress non-command fish history lines */
    if (!strncmp(l, "- when: ", 8) || !strncmp(l, "  paths:", 8) ||
        !strncmp(l, "  - ", 4)) { l[0] = '\0'; }
}
static void strip_zsh(char *l)  { if (l[0]==':'&&l[1]==' ') { char *s=strchr(l,';'); if(s) memmove(l,s+1,strlen(s+1)+1); } }

static void history_load(History *h, const char *path) {
    h->count = 0;
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, RED "Cannot open history: %s\n" RESET, path); return; }

    /* Seek to end and read last chunk - we want the most recent commands */
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    long chunk = (fsize > 65536) ? 65536 : fsize; /* read last 64KB */
    fseek(f, fsize - chunk, SEEK_SET);

    /* Heap-allocate to avoid ~1.6MB stack usage */
    typedef char HistLine[MAX_LINE];
    HistLine *all  = malloc(sizeof(HistLine) * MAX_HISTORY*4);
    HistLine *seen = malloc(sizeof(HistLine) * MAX_HISTORY*4);
    if (!all || !seen) { free(all); free(seen); fclose(f); return; }

    int total=0;
    char line[MAX_LINE];

    /* If we seeked mid-line, discard the partial first line */
    if (fsize > chunk) fgets(line, sizeof(line), f);

    /* No ceiling on total - we want all lines in the chunk so
       the backwards traversal picks the most recent unique ones */
    while (fgets(line, sizeof(line), f)) {
        if (total >= MAX_HISTORY*4) break;
        line[strcspn(line,"\n")] = '\0';
        strip_fish(line);
        strip_zsh(line);
        if (line[0]) strncpy(all[total++], line, MAX_LINE-1);
    }
    fclose(f);

    int sc=0;
    for (int i=total-1; i>=0 && h->count<MAX_HISTORY; i--) {
        int dup=0;
        for (int j=0; j<sc; j++) if (!strcmp(seen[j],all[i])) { dup=1; break; }
        if (!dup) { strncpy(seen[sc++],all[i],MAX_LINE-1); strncpy(h->cmds[h->count++],all[i],MAX_LINE-1); }
    }
    free(all); free(seen);
}

static void history_context(const History *h, char *buf, size_t max) {
    size_t pos=0; int n=(h->count<50)?h->count:50;
    for (int i=0; i<n && pos<max-64; i++) {
        for (int j=0; h->cmds[i][j] && pos<max-4; j++) {
            char c=h->cmds[i][j];
            if (c=='"'||c=='\\') buf[pos++]='\\';
            if (c!='\r'&&c!='\n') buf[pos++]=c;
        }
        buf[pos++]='\n';
    }
    buf[pos]='\0';
}

/* ══════════════════════════════════════════════════════════════════════════════
 * GEMINI
 * ══════════════════════════════════════════════════════════════════════════════ */

/* extract_text: heap-allocates result - caller must free() */
static char *extract_text(const char *json) {
    char *s = strstr(json, "\"text\": \""); if (!s) return NULL;
    s += 9;

    size_t len = 0;
    const char *p = s;
    while (*p && *p != '"') {
        if (*p == '\\' && *(p+1)) { p++; }
        len++; p++;
    }

    char *r = malloc(len + 1); if (!r) return NULL;
    int i = 0;
    while (*s && *s != '"') {
        if (*s == '\\' && *(s+1)) {
            s++;
            switch (*s) {
                case 'n':  r[i++] = '\n'; break;
                case 't':  r[i++] = '\t'; break;
                case 'r':  r[i++] = '\r'; break;
                case '"':  r[i++] = '"';  break;
                case '\\': r[i++] = '\\'; break;
                case 'u': {
                    /* \uXXXX: s is on 'u', need to skip u+4 hex = 5 total.
                       main loop does s++ so we advance 4 here. */
                    unsigned int cp = 0;
                    if (sscanf(s+1, "%4x", &cp) == 1) {
                        s += 4;
                        r[i++] = (cp < 0x80) ? (char)cp : '?';
                    } else {
                        r[i++] = '?';
                    }
                    break;
                }
                default: r[i++] = *s; break;
            }
        } else {
            r[i++] = *s;
        }
        s++;
    }
    r[i] = '\0';
    return r;
}

static void json_esc(const char *src, char *dst, size_t max) {
    size_t j = 0;
    for (size_t i = 0; src[i] && j < max - 7; i++) {
        unsigned char c = (unsigned char)src[i];
        if      (c == '"')  { dst[j++]='\\'; dst[j++]='"';  }
        else if (c == '\\') { dst[j++]='\\'; dst[j++]='\\'; }
        else if (c == '\n') { dst[j++]='\\'; dst[j++]='n';  }
        else if (c == '\r') { dst[j++]='\\'; dst[j++]='r';  }
        else if (c == '\t') { dst[j++]='\\'; dst[j++]='t';  }
        else if (c < 0x20) {
            if (max - j < 7) break;
            snprintf(dst+j, max-j, "\\u%04x", c);
            j += 6; /* \uXXXX is always exactly 6 chars */
        }
        else { dst[j++] = (char)c; }
    }
    dst[j] = '\0';
}

/* Strip <THINK>...</THINK> (or THINK\n...\n\n) reasoning blocks some models emit */
static void strip_think(char *s) {
    /* Form 1: <THINK>...</THINK> or <think>...</think> */
    char *start;
    while ((start = strcasestr(s, "<think>")) != NULL) {
        char *end = strcasestr(start, "</think>");
        if (!end) { *start = '\0'; break; }
        end += 8; /* skip </think> */
        while (*end == '\n' || *end == '\r') end++;
        memmove(start, end, strlen(end) + 1);
    }
    /* Form 2: bare "THINK\n...\n\n" block at start */
    if (strncmp(s, "THINK\n", 6) == 0) {
        char *end = strstr(s + 6, "\n\n");
        if (end) { end += 2; memmove(s, end, strlen(end) + 1); }
        else { s[0] = '\0'; }
    }
    /* Trim leading whitespace/newlines left behind */
    size_t lead = strspn(s, " \t\r\n");
    if (lead) memmove(s, s + lead, strlen(s + lead) + 1);
}

static char *gemini_ask(const Config *cfg, const char *prompt) {
    char url[512]; snprintf(url, sizeof(url), GEMINI_URL, cfg->api_key);
    size_t plen = strlen(prompt);
    char *esc = malloc(plen*6+4); if (!esc) return NULL;
    json_esc(prompt, esc, plen*6+4);
    char *json = malloc(strlen(esc)+64); if (!json) { free(esc); return NULL; }
    snprintf(json, strlen(esc)+64, "{\"contents\":[{\"parts\":[{\"text\":\"%s\"}]}]}", esc);
    free(esc);
    Response *resp = http_post(url, json); free(json);
    if (!resp) return NULL;

    /* Debug: JARVIS_DEBUG=1 jarvis explain - prints raw Gemini response */
    if (getenv("JARVIS_DEBUG"))
        fprintf(stderr, DIM "\n[debug] raw response:\n%.2000s\n\n" RESET, resp->data);

    /* Check for rate limit (429) specifically */
    if (strstr(resp->data, "RESOURCE_EXHAUSTED") || strstr(resp->data, "429")) {
        /* Try to extract retry delay */
        char *retry = strstr(resp->data, "retryDelay\":\"");
        int seconds = 60; /* default */
        if (retry) sscanf(retry + 13, "%ds", &seconds);
        fprintf(stderr, YELLOW "Rate limit reached (20 req/day on free tier).\n" RESET);
        fprintf(stderr, YELLOW "Try again in %d seconds.\n" RESET, seconds);
        free(resp->data); free(resp);
        return NULL;
    }

    char *result = extract_text(resp->data);
    if (!result) {
        char *err = strstr(resp->data, "\"message\":");
        if (err) fprintf(stderr, RED "Gemini API error: %.200s\n" RESET, err);
        else {
            fprintf(stderr, RED "Could not parse Gemini response.\n" RESET);
            fprintf(stderr, DIM "Run with JARVIS_DEBUG=1 to see raw response.\n" RESET);
        }
    }
    free(resp->data); free(resp);
    if (result) strip_think(result);
    return result;
}

/* ══════════════════════════════════════════════════════════════════════════════
 * CMD: EXPLAIN  -  the pipe magic
 *   Usage: make 2>&1 | jarvis explain
 * ══════════════════════════════════════════════════════════════════════════════ */

static void cmd_explain(const Config *cfg) {
    if (isatty(STDIN_FILENO)) {
        printf(YELLOW "No piped input detected.\n" RESET);
        printf(YELLOW "Usage: your-command 2>&1 | jarvis explain\n\n" RESET);
        return;
    }
    char *buf = malloc(EXPLAIN_MAX); if (!buf) return;
    size_t total=0; int c;
    while ((c=fgetc(stdin))!=EOF && total<EXPLAIN_MAX-1) buf[total++]=(char)c;
    buf[total]='\0';

    if (total==0) {
        printf(YELLOW "No input captured.\n" RESET);
        free(buf); return;
    }

    /* last 3000 chars - tail is almost always the relevant error */
    const char *input = (total>3000) ? buf+total-3000 : buf;

    printf(CYAN "── Jarvis is reading the error " RESET DIM "────────────────\n\n" RESET);

    size_t plen = strlen(input)*2 + 512;
    char *prompt = malloc(plen); if (!prompt) { free(buf); return; }
    snprintf(prompt, plen,
        "You are a Linux expert. A command produced this output:\n\n---\n%s\n---\n\n"
        "Reply in plain text (no markdown, no asterisks, no bullet symbols):\n"
        "1. What went wrong - one sentence.\n"
        "2. The exact fix - the command or config change.\n"
        "3. Why it happened - one sentence.\n"
        "Be direct. If multiple errors, address the root cause first.", input);

    char *answer = gemini_ask(cfg, prompt);
    free(prompt); free(buf);
    if (!answer) return;

    char *line = strtok(answer, "\n");
    int   lnum = 0;
    while (line) {
        char *text = line;
        while (*text==' '||*text=='\t') text++;
        if (text[0]>='1'&&text[0]<='3'&&text[1]=='.') text+=2;
        while (*text==' ') text++;
        if (!text[0]) { line=strtok(NULL,"\n"); continue; }
        if      (lnum==0) printf(RED   "  ✗ %s\n" RESET, text);
        else if (lnum==1) printf(GREEN "  ✓ %s\n" RESET, text);
        else              printf(DIM   "  i %s\n" RESET, text);
        lnum++;
        line = strtok(NULL, "\n");
    }
    printf("\n");
    free(answer);
}

/* ══════════════════════════════════════════════════════════════════════════════
 * CMD: ALIAS MINER  -  pure local, no API needed
 * ══════════════════════════════════════════════════════════════════════════════ */

static void alias_name(const char *cmd, char *name, size_t max) {
    int j=0, words=0, in_word=0;
    for (int i=0; cmd[i]&&j<(int)max-1&&words<2; i++) {
        char c=cmd[i];
        if (c==' '||c=='\t') { if(in_word){name[j++]='_';words++;in_word=0;} }
        else if ((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='.') { name[j++]=c; in_word=1; }
    }
    if (j>0&&name[j-1]=='_') j--;
    name[j]='\0';
    if (j>12) name[12]='\0';
}

static void cmd_alias(const History *h) {
    CmdFreq freq[MAX_HISTORY*4]; int fc=0;
    for (int i=0; i<h->count; i++) {
        const char *cmd=h->cmds[i];
        if (strlen(cmd)<8||strchr(cmd,'=')) continue;
        int found=0;
        for (int j=0; j<fc; j++) if (!strcmp(freq[j].cmd,cmd)){freq[j].count++;found=1;break;}
        if (!found&&fc<MAX_HISTORY*4) { strncpy(freq[fc].cmd,cmd,MAX_LINE-1); freq[fc].count=1; fc++; }
    }
    /* sort descending */
    for (int i=0;i<fc-1;i++) for (int j=0;j<fc-i-1;j++)
        if (freq[j].count<freq[j+1].count) { CmdFreq t=freq[j]; freq[j]=freq[j+1]; freq[j+1]=t; }

    printf(CYAN "── Alias suggestions " RESET DIM "──────────────────────────\n" RESET);
    printf(DIM  "   Based on your %d most recent unique commands.\n\n" RESET, h->count);

    int shown=0;
    for (int i=0; i<fc&&shown<ALIAS_TOP; i++) {
        if (freq[i].count<2||strlen(freq[i].cmd)<=10) continue;
        char name[32]; alias_name(freq[i].cmd, name, sizeof(name));
        if (!name[0]) continue;
        printf(YELLOW "  alias %s=" RESET BOLD "'%s'" RESET DIM "   # used %dx\n" RESET,
               name, freq[i].cmd, freq[i].count);
        shown++;
    }
    if (!shown) printf(DIM "  Not enough repeated commands yet.\n" RESET);
    else {
        printf(DIM "\n  Paste into your ~/.bashrc or ~/.zshrc, then run:\n" RESET);
        printf(DIM "    source ~/.bashrc\n\n" RESET);
    }
}

/* ══════════════════════════════════════════════════════════════════════════════
 * CMD: SUGGEST
 * ══════════════════════════════════════════════════════════════════════════════ */

static void cmd_suggest(const Config *cfg, const History *h, const char *query) {
    int hits=0;
    for (int i=0; i<h->count; i++) {
        if (strstr(h->cmds[i],query)) {
            if (!hits) printf(YELLOW "── History matches ──────────────────────────\n" RESET);
            printf(YELLOW "  → " RESET "%s\n", h->cmds[i]);
            if (++hits>=5) break;
        }
    }
    printf(CYAN "── Asking Gemini " RESET DIM "──────────────────────────────\n" RESET);

    char ctx[4096]; history_context(h, ctx, sizeof(ctx));
    char prompt[8192];
    snprintf(prompt, sizeof(prompt),
        "You are a Linux terminal expert. "
        "The user's recent shell history (most recent first):\n%s\n"
        "Suggest ONE shell command for: %s\n"
        "Reply with ONLY the raw command. No explanation, no markdown, no backticks.",
        ctx, query);

    char *answer = gemini_ask(cfg, prompt); if (!answer) return;
    char *t = answer;
    while (*t == ' ' || *t == '\n' || *t == '\t') t++;
    size_t tlen = strlen(t);
    if (tlen == 0) { free(answer); return; }
    char *e = t + tlen - 1;
    while (e >= t && (*e == ' ' || *e == '\n' || *e == '\t')) { *e = '\0'; e--; }
    if (!t[0]) { free(answer); return; }

    printf(BLUE BOLD "  Jarvis suggests: " RESET BOLD "%s\n" RESET, t);
    printf(CYAN "  Run it? (y/n) > " RESET);
    char ch[4];
    if (fgets(ch, sizeof(ch), stdin) && ch[0] == 'y') {
        char working_buf[4096];
        strncpy(working_buf, t, sizeof(working_buf) - 1);
        working_buf[sizeof(working_buf) - 1] = '\0';
        char *args[128];
        split_args(working_buf, args, 128);
        if (args[0]) {
            pid_t pid = fork();
            if (pid == 0) {
                execvp(args[0], args);
                fprintf(stderr, "jarvis: command not found: %s\n", args[0]);
                _exit(127);
            } else if (pid > 0) {
                int status; waitpid(pid, &status, 0);
            }
        }
    }
    free(answer);
}

/* ══════════════════════════════════════════════════════════════════════════════
 * CMD: TALK
 * ══════════════════════════════════════════════════════════════════════════════ */

static void cmd_talk(const Config *cfg) {
    printf(GREEN "Talk mode - type " RESET BOLD "'back'" RESET GREEN " to return.\n\n" RESET);
    char session[16384]={0}; int first=1;
    while (1) {
        printf(CYAN "You > " RESET);
        char input[512];
        if (!fgets(input,sizeof(input),stdin)) break;
        input[strcspn(input,"\n")]='\0';
        if (!strcmp(input,"back")) { printf(GREEN "Back to command mode.\n\n" RESET); break; }
        if (!input[0]) continue;
        char prompt[8192];
        if (first) {
            snprintf(prompt,sizeof(prompt),
                "You are Jarvis, a sharp Linux terminal assistant. "
                "Plain text only - no markdown, no asterisks. Be concise. User: %s", input);
            first=0;
        } else {
            snprintf(prompt,sizeof(prompt),
                "You are Jarvis, a Linux terminal assistant. "
                "Conversation:\n%s\nUser: %s\nPlain text, no markdown, be concise.", session, input);
        }
        char *answer=gemini_ask(cfg,prompt); if (!answer) continue;
        printf(GREEN "Jarvis: " RESET "%s\n\n", answer);

        /* Rolling session: keep dropping oldest exchanges until new one fits */
        size_t exchange_len = strlen(input) + strlen(answer) + 32;
        while (strlen(session) + exchange_len >= sizeof(session) - 1) {
            char *next = strstr(session + 1, "User:");
            if (next) memmove(session, next, strlen(next) + 1);
            else { session[0] = '\0'; break; }
        }
        snprintf(session + strlen(session), sizeof(session) - strlen(session),
                 "User: %s\nJarvis: %s\n", input, answer);

        free(answer);
    }
}

/* ══════════════════════════════════════════════════════════════════════════════
 * CMD: WEATHER / SYSINFO / HELP
 * ══════════════════════════════════════════════════════════════════════════════ */

static void cmd_weather(const Config *cfg, const char *city) {
    if (!city||!city[0]) city=cfg->city;
    char url[512];
    snprintf(url,sizeof(url),"https://wttr.in/%s?format=%%25l:+%%25C+%%25t+Humidity:+%%25h+Wind:+%%25w",city);
    Response *r=http_get(url); if (!r) return;
    printf(CYAN "── Weather " RESET DIM "────────────────────────────────────\n" RESET);
    printf("  %s\n\n", r->data);
    free(r->data); free(r);
}

static void cmd_sysinfo(void) {
    printf(CYAN "── System Info " RESET DIM "─────────────────────────────────\n" RESET);
    FILE *f=fopen("/proc/meminfo","r");
    if (f) {
        long tot=0,avail=0; char k[64]; long v; char u[16];
        while (fscanf(f,"%63s %ld %15s",k,&v,u)>=2) {
            if (!strcmp(k,"MemTotal:"))     tot=v;
            if (!strcmp(k,"MemAvailable:")) avail=v;
        }
        fclose(f);
        printf("  RAM  : %ldMB / %ldMB (%d%%)\n",
               (tot-avail)/1024, tot/1024, tot?(int)(100.0*(tot-avail)/tot):0);
    }
    const char *tp[]=
        {"/sys/class/thermal/thermal_zone0/temp","/sys/class/thermal/thermal_zone1/temp",NULL};
    for (int i=0; tp[i]; i++) {
        FILE *tf=fopen(tp[i],"r"); if (!tf) continue;
        int t=0; fscanf(tf,"%d",&t); fclose(tf);
        if (t>0) { printf("  CPU  : %d°C\n",t/1000); break; }
    }
    FILE *df=popen("df -h / 2>/dev/null | tail -1","r");
    if (df) {
        char dev[64],sz[16],used[16],av[16],use[16],mnt[64];
        if (fscanf(df,"%63s%15s%15s%15s%15s%63s",dev,sz,used,av,use,mnt)==6)
            printf("  Disk : %s used / %s total (%s)\n",used,sz,use);
        pclose(df);
    }
    FILE *up=fopen("/proc/uptime","r");
    if (up) {
        double s; fscanf(up,"%lf",&s); fclose(up);
        int d=(int)(s/86400),h=(int)(s/3600)%24,m=(int)(s/60)%60;
        if (d) printf("  Up   : %dd %dh %dm\n",d,h,m);
        else   printf("  Up   : %dh %dm\n",h,m);
    }
    printf("\n");
}

/* ══════════════════════════════════════════════════════════════════════════════
 * CMD: UPDATE  -  pull and reinstall latest from GitHub
 * ══════════════════════════════════════════════════════════════════════════════ */

static void cmd_update(void) {
    printf(CYAN "── Updating Jarvis " RESET DIM "────────────────────────────\n" RESET);
    printf(DIM  "  Fetching latest from GitHub...\n\n" RESET);
    fflush(stdout);

    /* curl -fsSL <url> | bash - needs shell for the pipe, but URL is hardcoded */
    char *args[] = { "bash", "-c",
        "curl -fsSL https://raw.githubusercontent.com/Tehns/Jarvis/main/install.sh | bash",
        NULL };
    pid_t pid = fork();
    if (pid == 0) {
        execvp("bash", args);
        fprintf(stderr, RED "bash not found\n" RESET);
        _exit(1);
    } else if (pid > 0) {
        int status; waitpid(pid, &status, 0);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
            printf(RED "Update failed. Check your internet connection.\n" RESET);
    } else {
        printf(RED "fork failed.\n" RESET);
    }
}

/* ══════════════════════════════════════════════════════════════════════════════
 * CMD: WATCH  -  run a command, catch errors, explain them automatically
 *   Usage: jarvis watch make
 *          jarvis watch gcc foo.c
 *
 *  Uses fork+execvp - no shell injection risk.
 * ══════════════════════════════════════════════════════════════════════════════ */

/* Split a string into argv for execvp. Modifies working_buf in place.
   Caller must keep working_buf alive until execvp/waitpid completes. */
static int split_args(char *working_buf, char **args, int max_args) {
    int argc = 0;
    char *p = working_buf;
    while (*p && argc < max_args - 1) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        if (*p == '"' || *p == '\'') {
            char q = *p++;
            args[argc++] = p;
            while (*p && *p != q) p++;
            if (*p) *p++ = '\0';
        } else {
            args[argc++] = p;
            while (*p && *p != ' ' && *p != '\t') p++;
            if (*p) *p++ = '\0';
        }
    }
    args[argc] = NULL;
    return argc;
}

static void cmd_watch(const Config *cfg, const char *cmd) {
    if (!cmd || !cmd[0]) {
        printf(YELLOW "Usage: jarvis watch <command>\n" RESET);
        printf(YELLOW "  e.g: jarvis watch make\n\n" RESET);
        return;
    }

    printf(CYAN "── Watching: " RESET BOLD "%s\n\n" RESET, cmd);
    fflush(stdout);

    char working_buf[4096];
    strncpy(working_buf, cmd, sizeof(working_buf) - 1);
    working_buf[sizeof(working_buf) - 1] = '\0';
    char *args[128];
    split_args(working_buf, args, 128);
    if (!args[0]) { printf(RED "Empty command.\n" RESET); return; }

    char tmpfile[MAX_PATH];
    snprintf(tmpfile, sizeof(tmpfile), "/tmp/jarvis_watch_%d.log", (int)getpid());

    /* Open log file for writing captured output */
    int logfd = open(tmpfile, O_WRONLY|O_CREAT|O_TRUNC, 0600);
    if (logfd < 0) { printf(RED "Cannot create temp file.\n" RESET); return; }

    /* pipe: child writes to pipefd[1], parent reads from pipefd[0] and tees to terminal+logfd */
    int pipefd[2];
    if (pipe(pipefd) < 0) { close(logfd); printf(RED "pipe failed.\n" RESET); return; }

    pid_t child = fork();
    if (child < 0) { close(logfd); close(pipefd[0]); close(pipefd[1]); printf(RED "fork failed.\n" RESET); return; }

    if (child == 0) {
        /* child: stdout+stderr -> pipe write end */
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        close(logfd);
        execvp(args[0], args);
        fprintf(stderr, "jarvis: command not found: %s\n", args[0]);
        _exit(127);
    }

    /* parent: read from pipe, write to terminal and logfile simultaneously */
    close(pipefd[1]);
    char rbuf[4096];
    ssize_t n;
    while ((n = read(pipefd[0], rbuf, sizeof(rbuf))) > 0) {
        write(STDOUT_FILENO, rbuf, n);   /* terminal */
        write(logfd, rbuf, n);           /* log file */
    }
    close(pipefd[0]);
    close(logfd);

    int status = 0;
    waitpid(child, &status, 0);

    int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;

    if (exit_code == 0) {
        printf("\n" GREEN "✓ Command completed successfully.\n\n" RESET);
        remove(tmpfile);
        return;
    }

    /* Command failed - read the log and explain */
    printf("\n" RED "✗ Command failed (exit %d). Asking Jarvis...\n\n" RESET, exit_code);
    fflush(stdout);

    FILE *f = fopen(tmpfile, "r");
    if (!f) { printf(RED "Could not read command output.\n" RESET); return; }

    char buf[EXPLAIN_MAX]; size_t total = 0; int c;
    while ((c = fgetc(f)) != EOF && total < EXPLAIN_MAX - 1) buf[total++] = (char)c;
    buf[total] = '\0';
    fclose(f);
    remove(tmpfile);

    if (!total) { printf(YELLOW "No output captured.\n" RESET); return; }

    const char *input = (total > 3000) ? buf + total - 3000 : buf;

    size_t plen = strlen(input) * 2 + 512;
    char *prompt = malloc(plen); if (!prompt) return;
    snprintf(prompt, plen,
        "You are a Linux expert. The command '%s' failed with this output:\n\n---\n%s\n---\n\n"
        "Reply in plain text (no markdown, no asterisks, no bullet symbols):\n"
        "1. What went wrong - one sentence.\n"
        "2. The exact fix - the command or config change.\n"
        "3. Why it happened - one sentence.\n"
        "Be direct. Address root cause first.", cmd, input);

    printf(CYAN "── Jarvis explains " RESET DIM "────────────────────────────\n\n" RESET);

    char *answer = gemini_ask(cfg, prompt);
    free(prompt);
    if (!answer) return;

    /* Color-coded: red = problem, green = fix, dim = why */
    char *line = strtok(answer, "\n");
    int   lnum = 0;
    while (line) {
        char *text = line;
        while (*text == ' ' || *text == '\t') text++;
        if (text[0] >= '1' && text[0] <= '3' && text[1] == '.') text += 2;
        while (*text == ' ') text++;
        if (!text[0]) { line = strtok(NULL, "\n"); continue; }
        if      (lnum == 0) printf(RED   "  ✗ %s\n" RESET, text);
        else if (lnum == 1) printf(GREEN "  ✓ %s\n" RESET, text);
        else                printf(DIM   "  i %s\n" RESET, text);
        lnum++;
        line = strtok(NULL, "\n");
    }
    printf("\n");
    free(answer);
}

static void cmd_help(void) {
    printf(CYAN "── Commands " RESET DIM "───────────────────────────────────\n" RESET);
    printf(YELLOW "  %-22s" RESET " ask Gemini for a command\n",          "<anything>");
    printf(YELLOW "  %-22s" RESET " explain piped error output\n",        "explain");
    printf(YELLOW "  %-22s" RESET " run cmd, auto-explain on failure\n",  "watch <cmd>");
    printf(YELLOW "  %-22s" RESET " suggest aliases from history\n",      "alias");
    printf(YELLOW "  %-22s" RESET " update to latest version\n",          "update");
    printf(YELLOW "  %-22s" RESET " weather for configured city\n",       "weather");
    printf(YELLOW "  %-22s" RESET " weather for any city\n",              "weather [city]");
    printf(YELLOW "  %-22s" RESET " system info\n",                       "sysinfo");
    printf(YELLOW "  %-22s" RESET " chat with Jarvis\n",                  "talk");
    printf(YELLOW "  %-22s" RESET " this menu\n",                         "help");
    printf(YELLOW "  %-22s" RESET " exit\n",                              "close / exit / q");
    printf(DIM "\n  Pipe usage:\n    make 2>&1 | jarvis explain\n    gcc foo.c 2>&1 | jarvis explain\n\n" RESET);
}

/* ══════════════════════════════════════════════════════════════════════════════
 * UI
 * ══════════════════════════════════════════════════════════════════════════════ */

static void startup_animation(void) {
    const char *f[]={"J","JA","JAR","JARV","JARVI","JARVIS"};
    for (int i=0;i<6;i++) { printf("\r" GREEN BOLD "%s" RESET,f[i]); fflush(stdout); usleep(120000); }
    printf(DIM "  v%s\n────────────────────────────────────────────\n" RESET, VERSION);
}

static void print_version(void) { printf("jarvis %s\n", VERSION); }

static void print_usage(void) {
    printf(BOLD "jarvis " RESET DIM "v%s\n\n" RESET, VERSION);
    printf("Usage:\n"
           "  jarvis                     interactive mode\n"
           "  jarvis --version           print version\n"
           "  jarvis --help              print this help\n"
           "  jarvis explain             read piped stderr and explain it\n"
           "  jarvis watch <cmd>         run command, auto-explain on failure\n"
           "  jarvis alias               suggest aliases from shell history\n"
           "  jarvis update              update to latest version\n"
           "  jarvis \"find big files\"    one-shot command suggestion\n"
           "\nPipe usage:\n"
           "  make 2>&1 | jarvis explain\n"
           "  cargo build 2>&1 | jarvis explain\n"
           "  gcc foo.c 2>&1 | jarvis explain\n\n");
}

/* ══════════════════════════════════════════════════════════════════════════════
 * MAIN
 * ══════════════════════════════════════════════════════════════════════════════ */

int main(int argc, char *argv[]) {

    /* ── Flags (no config needed) ── */
    if (argc>=2) {
        if (!strcmp(argv[1],"--version")||!strcmp(argv[1],"-v")) { print_version(); return 0; }
        if (!strcmp(argv[1],"--help")   ||!strcmp(argv[1],"-h")) { print_usage();   return 0; }
    }

    /* ── Sub-commands ── */
    if (argc>=2) {
        Config cfg; int cfg_ok=config_init(&cfg);

        if (!strcmp(argv[1],"update")) {
            cmd_update(); return 0;
        }

        if (!strcmp(argv[1],"watch")) {
            if (!cfg_ok) return 1;
            /* Join argv[2..] as the command to watch */
            char wcmd[MAX_LINE]={0};
            for (int i=2;i<argc;i++) {
                if (i>2) strncat(wcmd," ",sizeof(wcmd)-strlen(wcmd)-1);
                strncat(wcmd,argv[i],sizeof(wcmd)-strlen(wcmd)-1);
            }
            cmd_watch(&cfg, wcmd); return 0;
        }

        if (!strcmp(argv[1],"explain")) {
            if (!cfg_ok) return 1;
            cmd_explain(&cfg); return 0;
        }

        if (!strcmp(argv[1],"alias")) {
            /* alias is local-only: works even without API key */
            History h; history_load(&h, cfg.history_path);
            cmd_alias(&h); return 0;
        }

        /* one-shot query: jarvis "find big files" */
        if (!cfg_ok) return 1;
        History h; history_load(&h, cfg.history_path);
        char query[512]={0};
        for (int i=1;i<argc;i++) {
            if (i>1) strncat(query," ",sizeof(query)-strlen(query)-1);
            strncat(query,argv[i],sizeof(query)-strlen(query)-1);
        }
        cmd_suggest(&cfg, &h, query); return 0;
    }

    /* ── Interactive mode ── */
    startup_animation();
    Config cfg;
    if (!config_init(&cfg)) { printf(DIM "Fix your config and restart.\n" RESET); return 1; }

    History history; history_load(&history, cfg.history_path);
    printf(DIM "  Shell history: %s (%d commands)\n\n" RESET, cfg.history_path, history.count);
    cmd_sysinfo();

    printf(DIM "── Recent commands ──────────────────────────\n" RESET);
    int n=history.count<10?history.count:10;
    for (int i=0;i<n;i++) printf(DIM "  %2d  " RESET "%s\n",i+1,history.cmds[i]);
    printf("\n");

    const char *bye[]={"Have a nice day!","See you later!","Goodbye!",
                       "Cya! Don't forget to touch grass.","Take care!","Farewell!"};
    srand((unsigned)time(NULL));

    while (1) {
        printf(CYAN "jarvis" RESET DIM " > " RESET); fflush(stdout);
        char input[2048];
        if (!fgets(input, sizeof(input), stdin)) break;

        /* Detect truncation: if no newline found, the line was longer than our buffer */
        if (!strchr(input, '\n') && !feof(stdin)) {
            /* Drain the rest of the line */
            int ch; while ((ch = fgetc(stdin)) != '\n' && ch != EOF);
            printf(YELLOW "  Input too long (max %d chars), please shorten your query.\n\n" RESET,
                   (int)sizeof(input) - 1);
            continue;
        }

        input[strcspn(input,"\n")] = '\0';
        if (!input[0]) continue;

        if (!strcmp(input,"close")||!strcmp(input,"exit")||!strcmp(input,"q")) {
            printf(GREEN "%s\n" RESET, bye[rand()%(sizeof(bye)/sizeof(*bye))]); break;
        }
        if (!strcmp(input,"help"))    { cmd_help();          continue; }
        if (!strcmp(input,"sysinfo")) { cmd_sysinfo();       continue; }
        if (!strcmp(input,"talk"))    { cmd_talk(&cfg);      continue; }
        if (!strcmp(input,"alias"))   { cmd_alias(&history); continue; }
        if (!strcmp(input,"update"))  { cmd_update();        continue; }
        if (!strncmp(input,"watch",5)) {
            char *wcmd=input+5; while(*wcmd==' ') wcmd++;
            cmd_watch(&cfg, wcmd); continue;
        }
        if (!strcmp(input,"explain")) {
            printf(YELLOW "  Usage: your-command 2>&1 | jarvis explain\n\n" RESET); continue;
        }
        if (!strncmp(input,"weather",7)) {
            char *city=input+7; while(*city==' ') city++;
            cmd_weather(&cfg,city); continue;
        }
        cmd_suggest(&cfg, &history, input);
    }
    return 0;
}
