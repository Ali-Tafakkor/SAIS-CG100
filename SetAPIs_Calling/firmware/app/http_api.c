#include "http_api.h"
#include "access_control.h"
#include "board_status.h"
#include "device_config.h"
#include "device_identity.h"
#include "led_control.h"
#include "lwip/mem.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"
#include "main.h"
#include "rj_api.h"
#include "rj_json.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    char request[RJ_BODY_MAX + 2049];
    char response[RJ_RESPONSE_MAX + 1024];
    unsigned response_size, queued, acknowledged;
    int in_use, responding;
    unsigned int used;
    uint32_t last_activity;
} HttpConnection;
static HttpConnection connections[2];

static void connection_free(struct tcp_pcb *pcb, HttpConnection *connection) {
    tcp_arg(pcb, NULL);
    tcp_recv(pcb, NULL);
    tcp_poll(pcb, NULL, 0);
    tcp_err(pcb, NULL);
    tcp_sent(pcb, NULL);
    if (connection)
        rj_zero(connection, sizeof(*connection));
}
static void connection_error(void *arg, err_t error) {
    (void)error;
    if (arg)
        rj_zero(arg, sizeof(HttpConnection));
}
static err_t pump_response(struct tcp_pcb *pcb, HttpConnection *c) {
    while (c->queued < c->response_size) {
        unsigned n = c->response_size - c->queued;
        unsigned room = tcp_sndbuf(pcb);
        if (!room)
            break;
        if (n > room)
            n = room;
        if (n > 1460)
            n = 1460;
        err_t e = tcp_write(pcb, c->response + c->queued, (u16_t)n, TCP_WRITE_FLAG_COPY);
        if (e == ERR_MEM)
            break;
        if (e != ERR_OK) {
            connection_free(pcb, c);
            tcp_abort(pcb);
            return ERR_ABRT;
        }
        c->queued += n;
    }
    (void)tcp_output(pcb);
    if (c->acknowledged == c->response_size) {
        connection_free(pcb, c);
        if (tcp_close(pcb) != ERR_OK) {
            tcp_abort(pcb);
            return ERR_ABRT;
        }
    }
    return ERR_OK;
}
static err_t response_sent(void *arg, struct tcp_pcb *pcb, u16_t n) {
    HttpConnection *c = arg;
    if (!c)
        return ERR_OK;
    c->acknowledged += n;
    c->last_activity = HAL_GetTick();
    return pump_response(pcb, c);
}
static err_t connection_poll(void *arg, struct tcp_pcb *pcb) {
    HttpConnection *c = arg;
    if (c && (uint32_t)(HAL_GetTick() - c->last_activity) >= 5000u) {
        connection_free(pcb, c);
        tcp_abort(pcb);
        return ERR_ABRT;
    }
    if (c && c->responding)
        return pump_response(pcb, c);
    return ERR_OK;
}
static err_t send_response_ex(struct tcp_pcb *pcb, HttpConnection *c, const char *status,
                              const char *body, const char *extra, const char *content_type) {
    int length = snprintf(c->response, sizeof(c->response),
                          "HTTP/1.1 %s\r\nContent-Type: %s\r\n"
                          "Content-Length: %u\r\nConnection: close\r\nCache-Control: no-store\r\n"
                          "%s\r\n%s",
                          status, content_type, (unsigned)strlen(body), extra, body);
    ++g_board_status.request_count;
    if (length <= 0 || (size_t)length >= sizeof(c->response)) {
        connection_free(pcb, c);
        tcp_abort(pcb);
        return ERR_ABRT;
    }
    c->response_size = (unsigned)length;
    c->queued = 0;
    c->acknowledged = 0;
    c->responding = 1;
    rj_zero(c->request, sizeof(c->request));
    tcp_sent(pcb, response_sent);
    return pump_response(pcb, c);
}
static err_t send_response(struct tcp_pcb *pcb, HttpConnection *c, const char *status,
                           const char *body) {
    return send_response_ex(pcb, c, status, body, "", "application/json");
}
static int equal_header(const char *begin, size_t n, const char *name) {
    if (n != strlen(name))
        return 0;
    for (size_t i = 0; i < n; ++i)
        if (tolower((unsigned char)begin[i]) != tolower((unsigned char)name[i]))
            return 0;
    return 1;
}
/* Only bounded Content-Length requests are accepted. Reject ambiguous framing. */
static int body_length(const char *request, const char *end, unsigned *length) {
    const char *line = strstr(request, "\r\n");
    int seen = 0;
    *length = 0;
    if (!line)
        return -1;
    line += 2;
    while (line < end) {
        const char *next = strstr(line, "\r\n");
        if (!next || next > end)
            return -1;
        const char *colon = memchr(line, ':', (size_t)(next - line));
        if (!colon)
            return -1;
        if (equal_header(line, (size_t)(colon - line), "Transfer-Encoding"))
            return -1;
        if (equal_header(line, (size_t)(colon - line), "Content-Length")) {
            if (seen++)
                return -1;
            const char *p = colon + 1;
            while (p < next && (*p == ' ' || *p == '\t'))
                ++p;
            if (p == next || !isdigit((unsigned char)*p))
                return -1;
            unsigned n = 0;
            while (p < next && isdigit((unsigned char)*p)) {
                n = n * 10u + (unsigned)(*p++ - '0');
                if (n > RJ_BODY_MAX)
                    return -2;
            }
            while (p < next && (*p == ' ' || *p == '\t'))
                ++p;
            if (p != next)
                return -1;
            *length = n;
        }
        line = next + 2;
    }
    return 0;
}
static int header_value(const char *request, const char *name, char *out, size_t cap) {
    out[0] = 0;
    int seen = 0;
    const char *p = strstr(request, "\r\n");
    if (!p)
        return -1;
    p += 2;
    while (*p && strncmp(p, "\r\n", 2)) {
        const char *end = strstr(p, "\r\n"), *colon;
        if (!end)
            return -1;
        colon = memchr(p, ':', (size_t)(end - p));
        if (!colon)
            return -1;
        if (equal_header(p, (size_t)(colon - p), name)) {
            if (seen++)
                return -1;
            const char *start = colon + 1;
            while (start < end && (*start == ' ' || *start == '\t'))
                start++;
            const char *last = end;
            while (last > start && (last[-1] == ' ' || last[-1] == '\t'))
                last--;
            size_t n = (size_t)(last - start);
            if (n >= cap)
                return -1;
            memcpy(out, start, n);
            out[n] = 0;
        }
        p = end + 2;
    }
    return seen;
}
static const char *status_text(int s) {
    switch (s) {
    case 200:
        return "200 OK";
    case 201:
        return "201 Created";
    case 202:
        return "202 Accepted";
    case 204:
        return "204 No Content";
    case 400:
        return "400 Bad Request";
    case 403:
        return "403 Forbidden";
    case 404:
        return "404 Not Found";
    case 405:
        return "405 Method Not Allowed";
    case 409:
        return "409 Conflict";
    case 412:
        return "412 Precondition Failed";
    case 413:
        return "413 Payload Too Large";
    case 415:
        return "415 Unsupported Media Type";
    case 422:
        return "422 Unprocessable Entity";
    case 428:
        return "428 Precondition Required";
    case 503:
        return "503 Service Unavailable";
    default:
        return "500 Internal Server Error";
    }
}
static const char *skip_space(const char *p) {
    while (*p && strchr(" \t\r\n", *p))
        ++p;
    return p;
}
/* Backward-compatible JSON endpoint: accept exactly one boolean field "on". */
static int parse_on(const char *body) {
    const char *p = skip_space(body);
    if (*p++ != '{')
        return -1;
    p = skip_space(p);
    if (strncmp(p, "\"on\"", 4))
        return -1;
    p = skip_space(p + 4);
    if (*p++ != ':')
        return -1;
    p = skip_space(p);
    int on;
    if (!strncmp(p, "true", 4)) {
        on = 1;
        p += 4;
    } else if (!strncmp(p, "false", 5)) {
        on = 0;
        p += 5;
    } else
        return -1;
    p = skip_space(p);
    if (*p++ != '}')
        return -1;
    return *skip_space(p) == '\0' ? on : -1;
}
static err_t led_response(struct tcp_pcb *pcb, HttpConnection *c) {
    char body[192];
    led_update(HAL_GetTick(), 0);
    snprintf(body, sizeof(body),
             "{\"ok\":true,\"mode\":\"%s\",\"led_on\":%s,\"period_ms\":%lu,"
             "\"transitions\":%lu}",
             led_mode_name(), g_board_status.led_on ? "true" : "false",
             (unsigned long)led_period_ms(), (unsigned long)led_transition_count());
    return send_response(pcb, c, "200 OK", body);
}
static err_t handle_request(struct tcp_pcb *pcb, HttpConnection *c, const char *body) {
    char method[8], path[160], version[16], json[720];
    int consumed = 0;
    if (!g100_source_is_authorized(&pcb->remote_ip))
        return send_response(pcb, c, "403 Forbidden",
                             "{\"ok\":false,\"error\":\"source IP is not authorized\"}");
    const char *line_end = strstr(c->request, "\r\n");
    if (sscanf(c->request, "%7s %159s %15s%n", method, path, version, &consumed) != 3 ||
        !line_end || c->request + consumed != line_end ||
        (strcmp(version, "HTTP/1.1") && strcmp(version, "HTTP/1.0")))
        return send_response(pcb, c, "400 Bad Request",
                             "{\"ok\":false,\"error\":\"invalid request line\"}");
    if (!strcmp(method, "OPTIONS"))
        return send_response_ex(pcb, c, "204 No Content", "",
                                "Allow: GET, POST, PUT, DELETE, OPTIONS\r\n", "application/json");
    if (!strncmp(path, "/api/v1", 7) && (path[7] == '/' || path[7] == 0 || path[7] == '?')) {
        static char result[RJ_RESPONSE_MAX];
        char extra[512], match[129], none[129], idem[65], content_type[80], local[16];
        if (header_value(c->request, "If-Match", match, sizeof(match)) < 0 ||
            header_value(c->request, "If-None-Match", none, sizeof(none)) < 0 ||
            header_value(c->request, "Idempotency-Key", idem, sizeof(idem)) < 0 ||
            header_value(c->request, "Content-Type", content_type, sizeof(content_type)) < 0)
            return send_response(pcb, c, "400 Bad Request", "{\"code\":\"HEADER_INVALID\"}");
        if (*body && strcmp(content_type, "application/json") &&
            strcmp(content_type, "application/json; charset=utf-8"))
            return send_response(pcb, c, "415 Unsupported Media Type",
                                 "{\"code\":\"CONTENT_TYPE_INVALID\"}");
        ip4addr_ntoa_r(ip_2_ip4(&pcb->local_ip), local, sizeof(local));
        RjRequest r = {method, path, body, match, none, idem, local};
        int code = rj_api_handle(&r, result, sizeof(result), extra, sizeof(extra));
        return send_response_ex(pcb, c, status_text(code), result, extra,
                                code >= 400 ? "application/problem+json"
                                : !strncmp(path, "/api/v1/schemas/", 16) ? "application/schema+json"
                                                                         : "application/json");
    }
    int get = !strcmp(method, "GET");
    int post = !strcmp(method, "POST");
    if (!get && !post)
        return send_response(pcb, c, "405 Method Not Allowed",
                             "{\"ok\":false,\"error\":\"use GET or POST\"}");

    if ((!strcmp(path, "/api/health") || !strcmp(path, "/api/status")) && get) {
        int length = g100_write_status_json(json, sizeof(json));
        if (length <= 0 || (size_t)length >= sizeof(json))
            return send_response(pcb, c, "500 Internal Server Error",
                                 "{\"ok\":false,\"error\":\"status serialization failed\"}");
        return send_response(pcb, c, "200 OK", json);
    }
    if ((!strcmp(path, "/api/device") || !strcmp(path, "/api/identify")) && get) {
        int length = g100_write_identity_json(json, sizeof(json));
        if (length <= 0 || (size_t)length >= sizeof(json))
            return send_response(pcb, c, "500 Internal Server Error",
                                 "{\"ok\":false,\"error\":\"identity serialization failed\"}");
        return send_response(pcb, c, "200 OK", json);
    }
    if (!strcmp(path, "/api/access") && get) {
        int length = g100_write_authorized_sources_json(json, sizeof(json));
        if (length <= 0 || (size_t)length >= sizeof(json))
            return send_response(pcb, c, "500 Internal Server Error",
                                 "{\"ok\":false,\"error\":\"access serialization failed\"}");
        return send_response(pcb, c, "200 OK", json);
    }
    if (!strcmp(path, "/api/led")) {
        if (post) {
            int on = parse_on(body);
            if (on < 0)
                return send_response(
                    pcb, c, "400 Bad Request",
                    "{\"ok\":false,\"error\":\"expected JSON object with boolean on\"}");
            led_set_mode(on ? LED_ON : LED_OFF, 0);
        }
        return led_response(pcb, c);
    }
    if (!strcmp(path, "/api/led/on"))
        led_set_mode(LED_ON, 0);
    else if (!strcmp(path, "/api/led/off"))
        led_set_mode(LED_OFF, 0);
    else if (!strcmp(path, "/api/led/auto"))
        led_set_mode(LED_AUTO, 0);
    else if (!strcmp(path, "/api/led/dance") || !strncmp(path, "/api/led/dance?", 15)) {
        uint32_t period = 500;
        const char *q = strchr(path, '?');
        if (q) {
            if (strncmp(q + 1, "period_ms=", 10))
                return send_response(pcb, c, "400 Bad Request",
                                     "{\"ok\":false,\"error\":\"use period_ms=100..10000\"}");
            q += 11;
            period = 0;
            if (!*q)
                period = 10001;
            for (; *q; ++q) {
                if (!isdigit((unsigned char)*q) || period > 10000u) {
                    period = 10001;
                    break;
                }
                period = period * 10u + (unsigned)(*q - '0');
            }
            if (period < 100u || period > 10000u)
                return send_response(pcb, c, "400 Bad Request",
                                     "{\"ok\":false,\"error\":\"period_ms must be 100..10000\"}");
        }
        led_set_mode(LED_DANCE, period);
    } else if (!strcmp(path, "/") && get) {
        return send_response(
            pcb, c, "200 OK",
            "{\"service\":\"G100 Ethernet API\",\"api_version\":\"" G100_FIRMWARE_VERSION
            "\",\"rj45_api\":\"/api/v1\",\"security_mode\":\"development_plaintext\","
            "\"endpoints\":[\"GET /api/device\",\"GET /api/identify\","
            "\"GET /api/status\",\"GET /api/health\",\"GET /api/access\",\"GET /api/led\",\"POST "
            "/api/led\","
            "\"POST /api/led/on\",\"POST /api/led/off\",\"POST /api/led/dance?period_ms=500\","
            "\"POST /api/led/auto\"],\"udp_discovery\":\"G100_DISCOVER_V1 on port 37020\","
            "\"command_get_aliases\":true}");
    } else {
        return send_response(pcb, c, "404 Not Found", "{\"ok\":false,\"error\":\"not found\"}");
    }
    return led_response(pcb, c);
}
static err_t receive_request(void *arg, struct tcp_pcb *pcb, struct pbuf *packet, err_t error) {
    HttpConnection *c = arg;
    if (error != ERR_OK || !c) {
        if (packet)
            pbuf_free(packet);
        connection_free(pcb, c);
        tcp_abort(pcb);
        return ERR_ABRT;
    }
    if (!packet) {
        connection_free(pcb, c);
        if (tcp_close(pcb) != ERR_OK) {
            tcp_abort(pcb);
            return ERR_ABRT;
        }
        return ERR_OK;
    }
    c->last_activity = HAL_GetTick();
    if (c->responding) {
        tcp_recved(pcb, packet->tot_len);
        pbuf_free(packet);
        return ERR_OK;
    }
    u16_t received = packet->tot_len;
    unsigned available = sizeof(c->request) - 1u - c->used;
    unsigned copy = received < available ? received : available;
    pbuf_copy_partial(packet, c->request + c->used, (u16_t)copy, 0);
    c->used += copy;
    c->request[c->used] = '\0';
    tcp_recved(pcb, received);
    pbuf_free(packet);
    if (copy < received)
        return send_response(pcb, c, "413 Payload Too Large", "{\"code\":\"REQUEST_TOO_LARGE\"}");
    if (memchr(c->request, '\0', c->used))
        return send_response(pcb, c, "400 Bad Request",
                             "{\"ok\":false,\"error\":\"invalid NUL byte\"}");
    char *end = strstr(c->request, "\r\n\r\n");
    if (!end) {
        if (c->used >= 2048)
            return send_response(pcb, c, "413 Payload Too Large",
                                 "{\"ok\":false,\"error\":\"headers too large\"}");
        return ERR_OK;
    }
    unsigned length;
    int framing = body_length(c->request, end, &length);
    unsigned header_size = (unsigned)(end + 4 - c->request);
    if (header_size > 2048)
        return send_response(pcb, c, "413 Payload Too Large", "{\"code\":\"HEADERS_TOO_LARGE\"}");
    if (framing == -2 || (framing == 0 && length > sizeof(c->request) - 1u - header_size))
        return send_response(pcb, c, "413 Payload Too Large",
                             "{\"ok\":false,\"error\":\"body too large\"}");
    if (framing)
        return send_response(pcb, c, "400 Bad Request",
                             "{\"ok\":false,\"error\":\"invalid or unsupported HTTP framing\"}");
    unsigned required = header_size + length;
    if (c->used < required)
        return ERR_OK;
    if (c->used > required)
        return send_response(pcb, c, "400 Bad Request", "{\"code\":\"EXTRA_REQUEST_BYTES\"}");
    c->request[required] = '\0';
    return handle_request(pcb, c, end + 4);
}
static err_t accept_connection(void *arg, struct tcp_pcb *pcb, err_t error) {
    (void)arg;
    if (error != ERR_OK)
        return error;
    HttpConnection *c = NULL;
    for (unsigned i = 0; i < 2; i++)
        if (!connections[i].in_use) {
            c = &connections[i];
            break;
        }
    if (!c) {
        tcp_abort(pcb);
        return ERR_ABRT;
    }
    memset(c, 0, sizeof(*c));
    c->in_use = 1;
    c->last_activity = HAL_GetTick();
    tcp_arg(pcb, c);
    tcp_recv(pcb, receive_request);
    tcp_err(pcb, connection_error);
    tcp_poll(pcb, connection_poll, 2);
    return ERR_OK;
}
int http_api_init(void) {
    struct tcp_pcb *pcb = tcp_new();
    if (!pcb)
        return -1;
    if (tcp_bind(pcb, IP_ADDR_ANY, 80) != ERR_OK) {
        tcp_abort(pcb);
        return -2;
    }
    struct tcp_pcb *listener = tcp_listen(pcb);
    if (!listener) {
        tcp_abort(pcb);
        return -3;
    }
    tcp_accept(listener, accept_connection);
    return 0;
}
