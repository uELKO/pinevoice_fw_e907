#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <aos/kv.h>

#include "lwip/apps/httpd.h"
#include "lwip/apps/fs.h"
#include "lwip/pbuf.h"
#include "lwip/tcpip.h"

#include "http_config.h"

#define KV_STR_MAX  64
#define POST_BUF_SIZE 256

static const char s_form_html[] =
    "<!doctype html><html><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>PineVoice Setup</title></head>"
    "<body style=\"font-family:sans-serif;max-width:360px;margin:2em auto\">"
    "<h2>PineVoice MQTT Setup</h2>"
    "<form method=\"POST\" action=\"/save\">"
    "<label>MQTT Benutzername<br><input name=\"user\" style=\"width:100%\"></label><br><br>"
    "<label>MQTT Passwort<br><input name=\"pass\" type=\"password\" style=\"width:100%\"></label><br><br>"
    "<button type=\"submit\">Speichern</button>"
    "</form></body></html>";

static const char s_saved_html[] =
    "<!doctype html><html><head><meta charset=\"utf-8\"></head>"
    "<body style=\"font-family:sans-serif\">"
    "<p>Gespeichert. PineVoice verbindet sich jetzt mit dem MQTT-Broker.</p>"
    "</body></html>";

static void *s_post_conn;
static char s_post_buf[POST_BUF_SIZE];
static size_t s_post_len;

int fs_open_custom(struct fs_file *file, const char *name)
{
    const char *data;
    size_t len;

    if (!strcmp(name, "/index.html")) {
        data = s_form_html;
        len  = sizeof(s_form_html) - 1;
    } else if (!strcmp(name, "/saved.html")) {
        data = s_saved_html;
        len  = sizeof(s_saved_html) - 1;
    } else {
        return 0;
    }

    memset(file, 0, sizeof(*file));
    file->data  = data;
    file->len   = (int)len;
    file->index = (int)len;
    file->flags = 0;
    return 1;
}

void fs_close_custom(struct fs_file *file)
{
    (void)file;
}

err_t httpd_post_begin(void *connection, const char *uri, const char *http_request,
                        u16_t http_request_len, int content_len, char *response_uri,
                        u16_t response_uri_len, u8_t *post_auto_wnd)
{
    (void)http_request;
    (void)http_request_len;
    (void)response_uri;
    (void)response_uri_len;

    if (strcmp(uri, "/save") != 0) {
        return ERR_VAL;
    }
    if (content_len <= 0 || content_len >= POST_BUF_SIZE) {
        return ERR_VAL;
    }

    s_post_conn = connection;
    s_post_len  = 0;
    *post_auto_wnd = 1;
    return ERR_OK;
}

err_t httpd_post_receive_data(void *connection, struct pbuf *p)
{
    struct pbuf *q;

    if (connection != s_post_conn) {
        pbuf_free(p);
        return ERR_VAL;
    }

    for (q = p; q != NULL; q = q->next) {
        size_t copy = q->len;
        if (s_post_len + copy >= POST_BUF_SIZE) {
            copy = POST_BUF_SIZE - 1 - s_post_len;
        }
        if (copy > 0) {
            memcpy(s_post_buf + s_post_len, q->payload, copy);
            s_post_len += copy;
        }
    }
    pbuf_free(p);
    return ERR_OK;
}

static void url_decode(char *s)
{
    char *o = s;

    while (*s) {
        if (*s == '+') {
            *o++ = ' ';
            s++;
        } else if (*s == '%' && s[1] && s[2]) {
            char hex[3] = { s[1], s[2], 0 };
            *o++ = (char)strtol(hex, NULL, 16);
            s += 3;
        } else {
            *o++ = *s++;
        }
    }
    *o = 0;
}

static int extract_field(const char *body, const char *key, char *out, size_t out_len)
{
    size_t key_len = strlen(key);
    const char *p = body;

    while (p) {
        if (!strncmp(p, key, key_len) && p[key_len] == '=') {
            const char *v = p + key_len + 1;
            const char *end = strchr(v, '&');
            size_t len = end ? (size_t)(end - v) : strlen(v);
            if (len >= out_len) {
                len = out_len - 1;
            }
            memcpy(out, v, len);
            out[len] = 0;
            url_decode(out);
            return 1;
        }
        p = strchr(p, '&');
        if (p) {
            p++;
        }
    }
    return 0;
}

void httpd_post_finished(void *connection, char *response_uri, u16_t response_uri_len)
{
    char user[KV_STR_MAX] = {0};
    char pass[KV_STR_MAX] = {0};

    if (connection != s_post_conn || s_post_len == 0) {
        snprintf(response_uri, response_uri_len, "/index.html");
        return;
    }

    s_post_buf[s_post_len] = 0;
    s_post_conn = NULL;

    if (extract_field(s_post_buf, "user", user, sizeof(user))) {
        aos_kv_setstring(MQTT_USER_KV, user);
    }
    if (extract_field(s_post_buf, "pass", pass, sizeof(pass))) {
        aos_kv_setstring(MQTT_PASS_KV, pass);
    }

    snprintf(response_uri, response_uri_len, "/saved.html");
}

void http_config_start(void)
{
    LOCK_TCPIP_CORE();
    httpd_init();
    UNLOCK_TCPIP_CORE();
}
