#include "rj_api.h"
#include "board_status.h"
#include "device_config.h"
#include "device_identity.h"
#include "lwip/netif.h"
#include "main.h"
#include "network_config.h"
#include "rj_catalogue.h"
#include "rj_json.h"
#include "rj_security.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#ifdef G100_SHADOW
#include "architecture.h"
#include "config_store.h"
#define RJ_CONFIG_STORAGE "redundant_nor"
#else
#define RJ_CONFIG_STORAGE "volatile_ram"
#endif
extern struct netif gnetif;
static RjJson input;
static char uid[25], boot_id[40];
static char instances[8193];
static unsigned instances_revision;
static const char default_settings_format[] =
    "{\"label\":\"%s\",\"location\":\"\",\"network\":{\"interface_id\":\"eth0\","
    "\"mode\":\"dhcp\",\"ipv4_address\":null,\"prefix_length\":null,\"gateway\":null,\"dns_"
    "servers\":[],\"hostname\":\"%s\"},\"time\":{\"timezone\":\"UTC\",\"allow_companion_"
    "sync\":false},\"log_level\":\"info\"}";
static char settings[2048], candidate[2048];
static unsigned settings_revision, candidate_revision;
static uint32_t candidate_at, apply_at, operation_created_at;
static int candidate_valid, network_pending;
static char operation_id[64], operation_state[32], apply_key[65];
static unsigned request_number, apply_revision;
static int append(char *out, size_t cap, const char *fmt, ...) {
    size_t n = strlen(out);
    if (n >= cap)
        return -1;
    va_list a;
    va_start(a, fmt);
    int w = vsnprintf(out + n, cap - n, fmt, a);
    va_end(a);
    return w < 0 || (size_t)w >= cap - n ? -1 : 0;
}
static int problem(char *out, size_t cap, int status, const char *code, const char *detail) {
    char quoted[640];
    rj_quote(quoted, sizeof(quoted), detail);
    snprintf(out, cap,
             "{\"type\":\"urn:aiscella:cg100:problem:%s\",\"title\":\"Request "
             "rejected\",\"status\":%d,\"code\":\"%s\",\"detail\":%s,\"instance\":\"/api/v1/"
             "requests/%u\",\"request_id\":\"rj-%u\",\"retryable\":false}",
             code, status, code, quoted, request_number, request_number);
    return status;
}
static int id_valid(const char *s) {
    size_t n = strlen(s);
    if (!n || n > 64)
        return 0;
    for (size_t i = 0; i < n; i++)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') ||
              (s[i] >= '0' && s[i] <= '9') || strchr("_.:-", s[i])))
            return 0;
    return 1;
}
static int hostname_valid(const char *s) {
    size_t n = strlen(s);
    if (!n || n > 63 || s[0] == '-' || s[n - 1] == '-')
        return 0;
    for (size_t i = 0; i < n; i++)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '-'))
            return 0;
    return 1;
}
static int fields(const RjJson *j, int k, const char *const *names, size_t count) {
    if (k < 0 || j->t[k].type != '{')
        return 0;
    for (int x = k + 1; x < j->t[k].next; x = j->t[j->t[x].next].next) {
        size_t i;
        for (i = 0; i < count; i++)
            if (rj_eq(j, x, names[i]))
                break;
        if (i == count)
            return 0;
    }
    return 1;
}
#define FIELDS(j, k, ...)                                                                          \
    fields(j, k, (const char *[]){__VA_ARGS__},                                                    \
           sizeof((const char *[]){__VA_ARGS__}) / sizeof(char *))
static void ip_string(char out[16], const ip4_addr_t *ip) {
    snprintf(out, 16, "%u.%u.%u.%u", ip4_addr1(ip), ip4_addr2(ip), ip4_addr3(ip), ip4_addr4(ip));
}
static void ip_json(char *out, size_t cap) {
    char ip[16], mask[16], gw[16];
    ip_string(ip, netif_ip4_addr(&gnetif));
    ip_string(mask, netif_ip4_netmask(&gnetif));
    ip_string(gw, netif_ip4_gw(&gnetif));
    snprintf(out, cap,
             "{\"interface_id\":\"eth0\",\"ip\":\"%s\",\"netmask\":\"%s\",\"gateway\":\"%s\","
             "\"network_mode\":\"%s\",\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\",\"link_up\":%s,"
             "\"netif_up\":%s,\"http_port\":80,\"https_port\":null,\"discovery_port\":55670}",
             ip, mask, gw, g100_network_mode(), gnetif.hwaddr[0], gnetif.hwaddr[1],
             gnetif.hwaddr[2], gnetif.hwaddr[3], gnetif.hwaddr[4], gnetif.hwaddr[5],
             netif_is_link_up(&gnetif) ? "true" : "false", netif_is_up(&gnetif) ? "true" : "false");
}
static void interface_json(char *out, size_t cap) {
    snprintf(out, cap,
             "{\"interface_id\":\"eth0\",\"kind\":\"ethernet\",\"label\":\"RJ45 "
             "Ethernet\",\"populated\":true,\"enabled\":true,\"link_state\":\"%s\",\"supported_"
             "protocol_ids\":[],\"configurable_protocol_ids\":[",
             netif_is_link_up(&gnetif) ? "up" : "down");
    for (unsigned i = 0; i < rj_profile_count; i++)
        append(out, cap, "%s\"%s\"", i ? "," : "", rj_profiles[i].id);
    append(out, cap,
           "],\"active_instance_ids\":[],\"exclusive_protocol_instance\":false,\"bus_power\":\"not_"
           "applicable\",\"counters\":{\"rx_frames\":%lu,\"tx_frames\":%lu,\"tx_errors\":%lu}}",
           (unsigned long)g_board_status.rx_frames, (unsigned long)g_board_status.tx_frames,
           (unsigned long)g_board_status.tx_errors);
}
static int profile_index(const char *id) {
    for (unsigned i = 0; i < rj_profile_count; i++)
        if (!strcmp(id, rj_profiles[i].id))
            return (int)i;
    return -1;
}
static void instance_schema(unsigned i, char *out, size_t cap) {
    snprintf(out, cap,
             "{\"type\":\"object\",\"additionalProperties\":false,\"required\":[\"instance_id\","
             "\"protocol_id\",\"interface_id\",\"enabled\",\"role\",\"parameters\"],\"properties\":"
             "{\"instance_id\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":64},\"protocol_"
             "id\":{\"type\":\"string\",\"const\":\"%s\"},\"interface_id\":{\"type\":\"string\","
             "\"const\":\"eth0\"},\"enabled\":{\"type\":\"boolean\"},\"role\":{\"type\":\"string\","
             "\"enum\":%s},\"parameters\":%s}}",
             rj_profiles[i].id, rj_profiles[i].roles, rj_profiles[i].schema);
}
static int validate_instances(const RjJson *j, int array, char *why, size_t cap) {
    if (array < 0 || j->t[array].type != '[' || j->t[array].count > 8) {
        snprintf(why, cap, "instances must be an array of at most 8 configurations");
        return 0;
    }
    char ids[8][65];
    unsigned count = 0;
    for (int k = array + 1; k < j->t[array].next; k = j->t[k].next) {
        char id[65], pid[65];
        if (rj_string(j, rj_get(j, k, "instance_id"), id, sizeof(id)) < 0 || !id_valid(id) ||
            rj_string(j, rj_get(j, k, "protocol_id"), pid, sizeof(pid)) < 0) {
            snprintf(why, cap, "Invalid instance or protocol ID");
            return 0;
        }
        int p = profile_index(pid);
        static char schema[4096];
        if (p >= 0)
            instance_schema((unsigned)p, schema, sizeof(schema));
        if (p < 0 || !rj_validate(j, k, schema)) {
            snprintf(why, cap, "Instance %s does not match its advertised schema", id);
            return 0;
        }
        for (unsigned i = 0; i < count; i++)
            if (!strcmp(ids[i], id)) {
                snprintf(why, cap, "Duplicate instance ID: %s", id);
                return 0;
            }
        strcpy(ids[count++], id);
        int params = rj_get(j, k, "parameters"), tls = rj_get(j, params, "tls");
        if (tls >= 0 && strcmp(pid, "mqtt") && j->t[rj_get(j, tls, "enabled")].type != 't') {
            snprintf(why, cap, "Secure profile requires tls.enabled=true");
            return 0;
        }
        if (tls >= 0 && j->t[rj_get(j, tls, "enabled")].type == 't' &&
            j->t[rj_get(j, tls, "verify_peer")].type != 't') {
            snprintf(why, cap, "Secure protocol configuration requires peer verification");
            return 0;
        }
        int mult = rj_get(j, params, "multicast_ipv4");
        if (mult >= 0) {
            char m[16];
            unsigned long v;
            rj_string(j, mult, m, sizeof(m));
            if (!rj_ipv4(m, &v) || (v >> 28) != 14) {
                snprintf(why, cap, "Expected multicast IPv4 address");
                return 0;
            }
        }
    }
    return 1;
}
static int settings_valid(const char *raw) {
    static RjJson j;
    if (rj_parse(&j, raw) || !FIELDS(&j, 0, "label", "location", "network", "time", "log_level"))
        return 0;
    char b[256];
    if (rj_string(&j, rj_get(&j, 0, "label"), b, 65) < 0 ||
        rj_string(&j, rj_get(&j, 0, "location"), b, 129) < 0)
        return 0;
    int log = rj_get(&j, 0, "log_level");
    if (!rj_eq(&j, log, "error") && !rj_eq(&j, log, "warn") && !rj_eq(&j, log, "info") &&
        !rj_eq(&j, log, "debug"))
        return 0;
    int net = rj_get(&j, 0, "network"), tm = rj_get(&j, 0, "time");
    if (!FIELDS(&j, net, "interface_id", "mode", "ipv4_address", "prefix_length", "gateway",
                "dns_servers", "hostname") ||
        !FIELDS(&j, tm, "timezone", "allow_companion_sync"))
        return 0;
    if (!rj_eq(&j, rj_get(&j, net, "interface_id"), "eth0") ||
        !rj_eq(&j, rj_get(&j, tm, "timezone"), "UTC"))
        return 0;
    int sync = rj_get(&j, tm, "allow_companion_sync");
    if (sync < 0 || j.t[sync].type != 'f')
        return 0; /* no wall-clock service yet */
    if (rj_string(&j, rj_get(&j, net, "hostname"), b, 64) < 0 || !hostname_valid(b))
        return 0;
    int mode = rj_get(&j, net, "mode"), ip = rj_get(&j, net, "ipv4_address"),
        pr = rj_get(&j, net, "prefix_length"), gw = rj_get(&j, net, "gateway");
    if (ip < 0 || pr < 0 || gw < 0)
        return 0;
    if (rj_eq(&j, mode, "static")) {
        unsigned long v, g = 0;
        if (rj_string(&j, ip, b, 16) < 0 || !rj_ipv4(b, &v) || v == 0 || (v >> 24) == 127 ||
            (v >> 24) >= 224)
            return 0;
        double prefix = rj_number(&j, pr);
        if (prefix < 1 || prefix > 30 || prefix != (int)prefix)
            return 0;
        unsigned long mask = (0xffffffffUL << (32 - (int)prefix)) & 0xffffffffUL;
        if (!(v & ~mask) || (v & ~mask) == (~mask & 0xffffffffUL))
            return 0;
        if (j.t[gw].type != 'n') {
            if (rj_string(&j, gw, b, 16) < 0 || !rj_ipv4(b, &g) || (g & mask) != (v & mask) ||
                !(g & ~mask) || (g & ~mask) == (~mask & 0xffffffffUL) || g == v)
                return 0;
        }
    } else if (!rj_eq(&j, mode, "dhcp") || j.t[ip].type != 'n' || j.t[pr].type != 'n' ||
               j.t[gw].type != 'n')
        return 0;
    int dns = rj_get(&j, net, "dns_servers");
    if (dns < 0 || j.t[dns].type != '[' || j.t[dns].count > 2)
        return 0;
    for (int k = dns + 1; k < j.t[dns].next; k = j.t[k].next)
        if (rj_string(&j, k, b, 16) < 0 || !rj_ipv4(b, NULL))
            return 0;
    return 1;
}
static void apply_network(const char *raw) {
    static RjJson j;
    rj_parse(&j, raw);
    int net = rj_get(&j, 0, "network");
    char ip[16] = "0.0.0.0", gw[16] = "0.0.0.0", host[64], dns[2][16] = {{0}};
    rj_string(&j, rj_get(&j, net, "ipv4_address"), ip, sizeof(ip));
    rj_string(&j, rj_get(&j, net, "gateway"), gw, sizeof(gw));
    rj_string(&j, rj_get(&j, net, "hostname"), host, sizeof(host));
    int d = rj_get(&j, net, "dns_servers"), i = 0;
    for (int k = d + 1; k < j.t[d].next && i < 2; k = j.t[k].next)
        rj_string(&j, k, dns[i++], 16);
    g100_network_configure(rj_eq(&j, rj_get(&j, net, "mode"), "dhcp"), ip,
                           (int)rj_number(&j, rj_get(&j, net, "prefix_length")), gw, host, dns[0],
                           dns[1]);
}
void rj_api_init(void) {
    snprintf(uid, sizeof(uid), "%08lX%08lX%08lX", (unsigned long)HAL_GetUIDw0(),
             (unsigned long)HAL_GetUIDw1(), (unsigned long)HAL_GetUIDw2());
    rj_security_init();
    rj_boot_id(boot_id);
    snprintf(settings, sizeof(settings), default_settings_format,
             g100_device_name(), g100_device_hostname());
    strcpy(instances, "[]");
#ifdef G100_SHADOW
    if(g100_config_load(settings,sizeof(settings),instances,sizeof(instances))) {
        settings_revision=instances_revision=g100_config_revision();
        static RjJson saved;char why[128];
        if(!settings_valid(settings) || rj_parse(&saved,instances) || !validate_instances(&saved,0,why,sizeof(why))) {
            g_board_status.fault=0xA003; /* Incompatible configuration must not confirm a trial. */
        }
    }
#endif
}
void rj_api_process(uint32_t now) {
    if (network_pending == 1 && (uint32_t)(now - apply_at) >= 2000) {
        apply_network(candidate);
        network_pending = 2;
        apply_at = now;
        strcpy(operation_state, "awaiting_confirmation");
    } else if (network_pending == 2 && (uint32_t)(now - apply_at) >= 60000) {
        apply_network(settings);
        network_pending = 0;
        candidate[0] = 0;
        candidate_valid = 0;
        strcpy(operation_state, "rolled_back");
    }
    if (candidate[0] && !network_pending && (uint32_t)(now - candidate_at) >= 900000) {
        candidate[0] = 0;
        candidate_valid = 0;
    }
}
static int op_json(char *out, size_t cap) {
    return snprintf(
        out, cap,
        "{\"operation_id\":\"%s\",\"kind\":\"settings_apply\",\"state\":\"%s\",\"created_at\":null,"
        "\"created_boot_id\":\"%s\",\"created_uptime_ms\":%lu,\"progress_percent\":%d,"
        "\"cancellable\":false,\"result_count\":0,\"results_path\":\"/api/v1/operations/%s/"
        "results\",\"target_id\":\"settings-candidate\",\"expires_in_seconds\":0,\"error\":null,"
        "\"storage\":\"volatile_ram\"}",
        operation_id, operation_state, boot_id, (unsigned long)operation_created_at, network_pending ? 0 : 100,
        operation_id);
}
int rj_api_handle(const RjRequest *r, char *out, size_t cap, char *extra, size_t extra_cap) {
    out[0] = 0;
    extra[0] = 0;
    request_number++;
    snprintf(extra, extra_cap, "X-Request-ID: rj-%u\r\n", request_number);
    if (!rj_authorize(r->method, r->path))
        return problem(out, cap, 403, "ACCESS_DENIED", "Access denied by policy provider");
    int get = !strcmp(r->method, "GET"), post = !strcmp(r->method, "POST"),
        put = !strcmp(r->method, "PUT"), del = !strcmp(r->method, "DELETE");
    char path[160];
    snprintf(path, sizeof(path), "%s", r->path);
    char *query = strchr(path, '?');
    if (query) {
        *query++ = 0; /* Small catalogue fits one page. Do not silently ignore cursors. */
        if (*query)
            return problem(out, cap, 400, "QUERY_UNSUPPORTED",
                           "Phase 1 lists fit one bounded page; omit query parameters");
    }
    if (*r->body && rj_parse(&input, r->body))
        return problem(out, cap, 400, "JSON_INVALID",
                       "Malformed, duplicate-key, excessively nested or oversized JSON");
    if ((get || del) && *r->body)
        return problem(out, cap, 400, "BODY_NOT_ALLOWED", "This method does not accept a body");
    if (!*r->body)
        rj_parse(&input, "{}");
#define FAIL(s, c, d) return problem(out, cap, s, c, d)
#define METHOD(ok)                                                                                 \
    if (!(ok)) {                                                                                   \
        append(extra, extra_cap, "Allow: GET, POST, PUT, DELETE, OPTIONS\r\n");                    \
        FAIL(405, "METHOD_NOT_ALLOWED", "Method not supported for this resource");                 \
    }
    if (!strcmp(path, "/api/v1") || !strcmp(path, "/api/v1/")) {
        METHOD(get);
        snprintf(
            out, cap,
            "{\"product\":\"AIScella CONNECT "
            "G100\",\"firmware_version\":\"%s\",\"profile\":\"rj45-configuration-only-v1\",\"full_"
            "v1_conformant\":false,\"security_mode\":\"development_plaintext\",\"authentication_"
            "required\":false,\"configuration_storage\":\"" RJ_CONFIG_STORAGE "\",\"discovery\":{\"port\":"
            "55670,\"legacy_port\":37020},\"resources\":[\"identity\",\"health\",\"system/"
            "status\",\"capabilities\",\"interfaces\",\"protocols\",\"protocol-instances\","
            "\"settings\",\"security/mode\",\"manufacturing/super-operator/"
            "credentials\",\"manufacturing/tls/csr\",\"manufacturing/tls/certificate\",\"security/"
            "tls/status\"]}",
            G100_FIRMWARE_VERSION);
        return 200;
    }
    if (!strcmp(path, "/api/v1/identity")) {
        METHOD(get);
        snprintf(out, cap,
                 "{\"device_id\":\"%s\",\"model\":\"CONNECT-G100\",\"hardware_revision\":"
                 "\"MainBoard-v2.6-H750\",\"api_version\":\"1.0\",\"firmware_version\":\"%s\","
                 "\"tls_name\":null,\"identity_key_sha256\":null,\"label\":\"%s\",\"identity_"
                 "provisioned\":false,\"security_mode\":\"development_plaintext\"}",
                 uid, G100_FIRMWARE_VERSION, g100_device_name());
        return 200;
    }
    if (!strcmp(path, "/api/v1/health")) {
        METHOD(get);
        snprintf(out, cap,
                 "{\"status\":\"%s\",\"boot_id\":\"%s\",\"uptime_ms\":%lu,\"runtime_state\":"
                 "\"empty\",\"management_ready\":true}",
                 g_board_status.fault ? "fault" : "ok", boot_id, (unsigned long)HAL_GetTick());
        return 200;
    }
#ifdef G100_SHADOW
    if (!strcmp(path, "/api/v1/system/memory")) {
        METHOD(get);
        g100_architecture_json(out, cap);
        return 200;
    }
#endif
    if (!strcmp(path, "/api/v1/system/status")) {
        METHOD(get);
        char net[640];
        ip_json(net, sizeof(net));
        snprintf(
            out, cap,
            "{\"device_id\":\"%s\",\"boot_id\":\"%s\",\"firmware_version\":\"%s\",\"api_version\":"
            "\"1.0\",\"uptime_ms\":%lu,\"runtime_state\":\"empty\",\"configuration_revision\":0,"
            "\"protocol_settings_revision\":%u,\"settings_revision\":%u,\"active_configuration_"
            "id\":null,\"active_package_sha256\":null,\"security_mode\":\"development_plaintext\","
            "\"authentication_required\":false,\"network\":%s,\"storage\":{\"configuration\":"
            "\"" RJ_CONFIG_STORAGE "\"},\"warnings\":[\"Development APIs are open\",\"Unconfirmed settings are discarded "
            "on reset\",\"Protocol drivers and Flow runtime are not "
            "implemented\",\"TLS provisioning is volatile; HTTPS listener is not implemented\"]}",
            uid, boot_id, G100_FIRMWARE_VERSION, (unsigned long)HAL_GetTick(), instances_revision,
            settings_revision, net);
        return 200;
    }
    if (!strcmp(path, "/api/v1/capabilities")) {
        METHOD(get);
        snprintf(out, cap,
                 "{\"device_id\":\"%s\",\"capabilities_revision\":1,\"api_version\":\"1.0\","
                 "\"profile\":\"rj45-configuration-only-v1\",\"full_v1_conformant\":false,"
                 "\"interface_ids\":[\"eth0\"],\"protocol_ids\":[],\"configurable_protocol_ids\":[",
                 uid);
        for (unsigned i = 0; i < rj_profile_count; i++)
            append(out, cap, "%s\"%s\"", i ? "," : "", rj_profiles[i].id);
        append(out, cap,
               "],\"block_types\":[],\"package_schema_versions\":[],\"features\":{\"protocol_"
               "configuration\":true,\"flow_runtime\":false,\"custom_expressions\":false,"
               "\"fieldbus_scan\":false,\"manual_point_write\":false,\"trace_values\":false,"
               "\"factory_reset\":false,\"trust_rotation\":false,\"firmware_update\":false,\"tls_"
               "csr\":true,\"tls_certificate_staging\":true,\"https\":false,\"credential_"
               "registration\":true,\"persistent_storage\":false},\"limits\":{\"max_http_"
               "connections\":2,\"max_json_body_bytes\":16384,\"max_response_bytes\":24576,\"max_"
               "protocol_instances\":8,\"max_protocol_configuration_bytes\":8192,\"max_json_"
               "depth\":12},\"auth\":{\"mode\":\"development_open\",\"required\":false}}");
        return 200;
    }
    if (!strcmp(path, "/api/v1/interfaces")) {
        METHOD(get);
        char it[1800];
        interface_json(it, sizeof(it));
        snprintf(out, cap, "{\"items\":[%s],\"next_cursor\":null,\"snapshot_id\":\"%s-eth0\"}", it,
                 boot_id);
        return 200;
    }
    if (!strcmp(path, "/api/v1/interfaces/eth0")) {
        METHOD(get);
        interface_json(out, cap);
        return 200;
    }
    if (!strcmp(path, "/api/v1/interfaces/eth0/network")) {
        METHOD(get);
        ip_json(out, cap);
        return 200;
    }
    if (!strcmp(path, "/api/v1/protocols")) {
        METHOD(get);
        strcpy(out, "{\"items\":[");
        for (unsigned i = 0; i < rj_profile_count; i++)
            if (append(out, cap, "%s%s", i ? "," : "", rj_profiles[i].metadata))
                FAIL(500, "SERIALIZATION_FAILED", "Catalogue exceeds response budget");
        append(out, cap, "],\"next_cursor\":null,\"snapshot_id\":\"rj45-profiles-1\"}");
        return 200;
    }
    if (!strncmp(path, "/api/v1/protocols/", 18)) {
        METHOD(get);
        int i = profile_index(path + 18);
        if (i < 0)
            FAIL(404, "PROTOCOL_NOT_FOUND", "Unknown protocol profile");
        snprintf(out, cap, "%s", rj_profiles[i].metadata);
        return 200;
    }
    if (!strncmp(path, "/api/v1/schemas/", 16)) {
        METHOD(get);
        for (unsigned i = 0; i < rj_profile_count; i++) {
            char id[100];
            snprintf(id, sizeof(id), "rj45.%s.instance.v1", rj_profiles[i].id);
            if (!strcmp(path + 16, id)) {
                instance_schema(i, out, cap);
                return 200;
            }
        }
        FAIL(404, "SCHEMA_NOT_FOUND", "Unknown schema ID");
    }
    if (!strcmp(path, "/api/v1/protocol-instances") ||
        !strcmp(path, "/api/v1/protocol-instances/validate") ||
        !strcmp(path, "/api/v1/protocol-instances/export")) {
        int check = !strcmp(path, "/api/v1/protocol-instances/validate"),
            exporting = !strcmp(path, "/api/v1/protocol-instances/export");
        METHOD(check ? post : exporting ? get : (get || put));
        if (!get) {
            if (!FIELDS(&input, 0, "base_revision", "instances"))
                FAIL(422, "CONFIGURATION_INVALID", "Expected base_revision and instances only");
            int rev = rj_get(&input, 0, "base_revision"), arr = rj_get(&input, 0, "instances");
            if (rev < 0 || rj_number(&input, rev) != instances_revision)
                FAIL(409, "REVISION_MISMATCH", "Refresh the current protocol settings revision");
            char why[192], next[8193];
            if (!validate_instances(&input, arr, why, sizeof(why)))
                FAIL(422, "CONFIGURATION_INVALID", why);
            if (rj_raw(&input, arr, next, sizeof(next)) < 0)
                FAIL(413, "CONFIGURATION_TOO_LARGE",
                     "Maximum protocol configuration is 8192 bytes");
            if (check) {
                strcpy(out, "{\"valid\":true,\"issues\":[],\"scope\":\"configuration_schema_only\","
                            "\"runtime_validated\":false,\"network_traffic_generated\":false}");
                return 200;
            }
            if (network_pending)
                FAIL(409, "DEVICE_BUSY", "Network settings change is pending");
#ifdef G100_SHADOW
            if(!g100_config_save(settings,next)) FAIL(500,"CONFIG_SAVE_FAILED","Previous configuration retained");
#endif
            strcpy(instances, next);
            instances_revision++;
        }
        snprintf(out, cap,
                 "{\"revision\":%u,\"instances\":%s,\"storage\":\"" RJ_CONFIG_STORAGE "\",\"applied_to_"
                 "drivers\":false}",
                 instances_revision, instances);
        append(extra, extra_cap, "ETag: \"protocols-%u\"\r\n", instances_revision);
        return 200;
    }
    if (!strncmp(path, "/api/v1/protocol-instances/", 27)) {
        METHOD(get);
        static RjJson saved;
        rj_parse(&saved, instances);
        for (int k = 1; k < saved.t[0].next; k = saved.t[k].next)
            if (rj_eq(&saved, rj_get(&saved, k, "instance_id"), path + 27)) {
                rj_raw(&saved, k, out, cap);
                return 200;
            }
        FAIL(404, "INSTANCE_NOT_FOUND", "Unknown configured instance");
    }
    if (!strcmp(path, "/api/v1/settings")) {
        METHOD(get);
        char pending[80];
        if (network_pending)
            rj_quote(pending, sizeof(pending), operation_id);
        else
            strcpy(pending, "null");
        snprintf(out, cap,
                 "{\"revision\":%u,\"settings\":%s,\"pending_operation_id\":%s,\"storage\":"
                 "\"" RJ_CONFIG_STORAGE "\"}",
                 settings_revision, settings, pending);
        append(extra, extra_cap, "ETag: \"settings-%u\"\r\n", settings_revision);
        return 200;
    }
    if (!strcmp(path, "/api/v1/settings/candidate")) {
        METHOD(get || put || del);
        if (network_pending && !get)
            FAIL(409, "CHANGE_PENDING", "Confirm the pending operation or wait for rollback");
        if (!get) {
            char etag[64];
            snprintf(etag, sizeof(etag), "\"candidate-%u\"", candidate_revision);
            if (candidate[0]) {
                if (!*r->if_match)
                    FAIL(428, "PRECONDITION_REQUIRED", "If-Match is required");
                if (strcmp(r->if_match, etag))
                    FAIL(412, "REVISION_MISMATCH", "Candidate ETag is stale");
            } else if (del)
                FAIL(404, "CANDIDATE_NOT_FOUND", "No candidate");
            else if (strcmp(r->if_none_match, "*"))
                FAIL(428, "PRECONDITION_REQUIRED", "Use If-None-Match: * for creation");
            if (del) {
                candidate[0] = 0;
                candidate_valid = 0;
                return 204;
            }
            if (!FIELDS(&input, 0, "base_settings_revision", "settings") ||
                rj_number(&input, rj_get(&input, 0, "base_settings_revision")) != settings_revision)
                FAIL(409, "REVISION_MISMATCH",
                     "Expected current base_settings_revision and settings");
            char next[2048];
            if (rj_raw(&input, rj_get(&input, 0, "settings"), next, sizeof(next)) < 0 ||
                !settings_valid(next))
                FAIL(422, "SETTINGS_INVALID",
                     "Invalid settings; eth0 IPv4, UTC and allow_companion_sync=false required");
            int existed = candidate[0] != 0;
            strcpy(candidate, next);
            candidate_revision++;
            candidate_at = HAL_GetTick();
            candidate_valid = 0;
            snprintf(out, cap,
                     "{\"candidate_id\":\"settings-candidate\",\"candidate_revision\":%u,\"base_"
                     "settings_revision\":%u,\"settings\":%s,\"validated\":false,\"expires_in_"
                     "seconds\":900}",
                     candidate_revision, settings_revision, candidate);
            append(extra, extra_cap, "ETag: \"candidate-%u\"\r\n", candidate_revision);
            return existed ? 200 : 201;
        }
        if (!candidate[0])
            FAIL(404, "CANDIDATE_NOT_FOUND", "No candidate");
        snprintf(
            out, cap,
            "{\"candidate_id\":\"settings-candidate\",\"candidate_revision\":%u,\"base_settings_"
            "revision\":%u,\"settings\":%s,\"validated\":%s,\"expires_in_seconds\":%lu}",
            candidate_revision, settings_revision, candidate, candidate_valid ? "true" : "false",
            (unsigned long)(900 - (HAL_GetTick() - candidate_at) / 1000));
        append(extra, extra_cap, "ETag: \"candidate-%u\"\r\n", candidate_revision);
        return 200;
    }
    if (!strcmp(path, "/api/v1/settings/candidate/validate")) {
        METHOD(post);
        if (!candidate[0])
            FAIL(404, "CANDIDATE_NOT_FOUND", "No candidate");
        char etag[64];
        snprintf(etag, sizeof(etag), "\"candidate-%u\"", candidate_revision);
        if (!*r->if_match)
            FAIL(428, "PRECONDITION_REQUIRED", "If-Match is required");
        if (strcmp(r->if_match, etag))
            FAIL(412, "REVISION_MISMATCH", "Candidate ETag is stale");
        candidate_valid = settings_valid(candidate);
        snprintf(out, cap,
                 "{\"valid\":%s,\"issues\":[],\"issues_truncated\":false,\"validated_capabilities_"
                 "revision\":1,\"estimated\":{}}",
                 candidate_valid ? "true" : "false");
        return 200;
    }
    if (!strcmp(path, "/api/v1/settings/candidate/apply")) {
        METHOD(post);
        if (!FIELDS(&input, 0, "candidate_id", "candidate_revision",
                    "confirmation_timeout_seconds") ||
            !rj_eq(&input, rj_get(&input, 0, "candidate_id"), "settings-candidate") ||
            rj_number(&input, rj_get(&input, 0, "confirmation_timeout_seconds")) != 60)
            FAIL(422, "SETTINGS_INVALID",
                 "Expected candidate ID/revision and 60 second confirmation timeout");
        double revision = rj_number(&input, rj_get(&input, 0, "candidate_revision"));
        if (strlen(r->idempotency_key) < 16 || strlen(r->idempotency_key) > 64)
            FAIL(400, "IDEMPOTENCY_KEY_REQUIRED", "Supply an Idempotency-Key of 16..64 characters");
        if (*apply_key && !strcmp(apply_key, r->idempotency_key)) {
            if (revision != apply_revision)
                FAIL(409, "IDEMPOTENCY_CONFLICT", "This key belongs to another candidate revision");
            op_json(out, cap);
            return 202;
        }
        if (revision != candidate_revision)
            FAIL(422, "SETTINGS_INVALID", "Expected current candidate revision");
        if (!candidate[0] || !candidate_valid)
            FAIL(422, "CANDIDATE_NOT_VALIDATED", "Validate candidate first");
        if (network_pending)
            FAIL(409, "DEVICE_BUSY", "Network change pending");
        strcpy(apply_key, r->idempotency_key);
        apply_revision = candidate_revision;
        snprintf(operation_id, sizeof(operation_id), "settings-%.12s-%u", boot_id,
                 candidate_revision);
        strcpy(operation_state, "queued");
        network_pending = 1;
        apply_at = HAL_GetTick();
        operation_created_at = apply_at;
        op_json(out, cap);
        append(extra, extra_cap, "Location: /api/v1/operations/%s\r\nRetry-After: 2\r\n",
               operation_id);
        return 202;
    }
    if (!strcmp(path, "/api/v1/settings/confirm")) {
        METHOD(post);
        if (!FIELDS(&input, 0, "operation_id", "candidate_id") ||
            !rj_eq(&input, rj_get(&input, 0, "operation_id"), operation_id) ||
            !rj_eq(&input, rj_get(&input, 0, "candidate_id"), "settings-candidate"))
            FAIL(409, "NO_PENDING_CHANGE", "Operation/candidate mismatch");
        if (!strcmp(operation_state, "succeeded")) {
            snprintf(out, cap,
                     "{\"revision\":%u,\"settings\":%s,\"pending_operation_id\":null,\"storage\":"
                     "\"" RJ_CONFIG_STORAGE "\"}",
                     settings_revision, settings);
            return 200;
        }
        if (network_pending != 2)
            FAIL(409, "NO_PENDING_CHANGE", "No applied change awaiting confirmation");
        char actual[16];
        ip_string(actual, netif_ip4_addr(&gnetif));
        if (!strcmp(actual, "0.0.0.0") || strcmp(actual, r->local_ip))
            FAIL(403, "WRONG_CONFIRMATION_PATH", "Reconnect through the new management address");
#ifdef G100_SHADOW
        if(!g100_config_save(candidate,instances)) FAIL(500,"CONFIG_SAVE_FAILED","Previous configuration retained; network rollback remains active");
#endif
        strcpy(settings, candidate);
        candidate[0] = 0;
        candidate_valid = 0;
        settings_revision++;
        network_pending = 0;
        strcpy(operation_state, "succeeded");
        snprintf(out, cap,
                 "{\"revision\":%u,\"settings\":%s,\"pending_operation_id\":null,\"storage\":"
                 "\"" RJ_CONFIG_STORAGE "\"}",
                 settings_revision, settings);
        return 200;
    }
    if (!strcmp(path, "/api/v1/operations")) {
        METHOD(get);
        char op[1200];
        op_json(op, sizeof(op));
        snprintf(out, cap,
                 "{\"items\":[%s],\"next_cursor\":null,\"snapshot_id\":\"%s-operations\"}",
                 *operation_id ? op : "", boot_id);
        return 200;
    }
    if (!strncmp(path, "/api/v1/operations/", 19)) {
        METHOD(get);
        char results[96];
        snprintf(results, sizeof(results), "%s/results", operation_id);
        if (*operation_id && !strcmp(path + 19, results)) {
            strcpy(out, "{\"items\":[],\"next_cursor\":null,\"snapshot_id\":\"no-field-results\"}");
            return 200;
        }
        if (!*operation_id || strcmp(path + 19, operation_id))
            FAIL(404, "OPERATION_NOT_FOUND", "Unknown retained operation");
        op_json(out, cap);
        return 200;
    }
    if (!strcmp(path, "/api/v1/security/mode") ||
        !strcmp(path, "/api/v1/manufacturing/security-mode")) {
        METHOD(!strcmp(path, "/api/v1/security/mode") ? get : put);
        if (put && (!FIELDS(&input, 0, "mode") ||
                    !rj_eq(&input, rj_get(&input, 0, "mode"), "development_plaintext")))
            FAIL(422, "CAPABILITY_UNSUPPORTED",
                 "Secure listener/provider is not installed in this development build");
        strcpy(out, "{\"mode\":\"development_plaintext\",\"production_locked\":false,\"plaintext_"
                    "reenable_allowed\":true,\"authentication_required\":false,\"source_ip_"
                    "filter\":false,\"policy_provider\":\"rj_security.c:rj_authorize\"}");
        return 200;
    }
    if (!strcmp(path, "/api/v1/manufacturing/status") ||
        !strcmp(path, "/api/v1/manufacturing/profile")) {
        METHOD(get);
        snprintf(out, cap,
                 "{\"device_id\":\"%s\",\"state\":\"development\",\"production_locked\":false,"
                 "\"factory_identity_provisioned\":false,\"interface_ids\":[\"eth0\"],\"storage\":"
                 "\"volatile_ram\",\"permanent_lock_supported\":false}",
                 uid);
        return 200;
    }
    if (!strcmp(path, "/api/v1/manufacturing/super-operator/credentials")) {
        METHOD(get || post);
        if (post) {
            char name[65], password[129];
            if (!FIELDS(&input, 0, "username", "password") ||
                rj_string(&input, rj_get(&input, 0, "username"), name, sizeof(name)) < 4 ||
                !id_valid(name) ||
                rj_string(&input, rj_get(&input, 0, "password"), password, sizeof(password)) < 16) {
                rj_zero(password, sizeof(password));
                FAIL(422, "CREDENTIAL_INVALID",
                     "Username 4..64 identifier characters; password 16..128 UTF-8 bytes");
            }
            int rc = rj_credentials(name, password);
            rj_zero(password, sizeof(password));
            if (rc)
                FAIL(503, "CRYPTO_UNAVAILABLE",
                     "RNG or password KDF failed; prior credential unchanged");
        }
        rj_credentials_status(out, cap);
        return 200;
    }
    if (!strcmp(path, "/api/v1/manufacturing/tls/csr")) {
        METHOD(post);
        char cn[129], profile[40];
        snprintf(cn, sizeof(cn), "%s.local", g100_device_hostname());
        if (!FIELDS(&input, 0, "common_name", "key_profile"))
            FAIL(422, "CSR_INVALID", "Unknown CSR field");
        int c = rj_get(&input, 0, "common_name"), k = rj_get(&input, 0, "key_profile");
        if (c >= 0 && rj_string(&input, c, cn, sizeof(cn)) < 1)
            FAIL(422, "CSR_INVALID", "Invalid common name");
        for (const char *p = cn; *p; p++)
            if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '.'))
                FAIL(422, "CSR_INVALID", "Common name must be a lower-case DNS name");
        if (k >= 0 &&
            (rj_string(&input, k, profile, sizeof(profile)) < 0 || strcmp(profile, "secp256r1")))
            FAIL(422, "KEY_PROFILE_UNSUPPORTED", "Only secp256r1 is supported");
        char pem[1600], q[2000];
        int rc = rj_tls_csr(cn, pem, sizeof(pem));
        if (rc)
            FAIL(503, "CRYPTO_UNAVAILABLE", "CSR generation failed; check RNG and heap capacity");
        rj_quote(q, sizeof(q), pem);
        snprintf(out, cap,
                 "{\"csr_pem\":%s,\"key_origin\":\"generated_on_device\",\"private_key_"
                 "exportable\":false,\"storage\":\"volatile_ram\"}",
                 q);
        return 201;
    }
    if (!strcmp(path, "/api/v1/manufacturing/tls/certificate")) {
        METHOD(put);
        static char leaf[4097], chain[8193];
        chain[0] = 0;
        if (!FIELDS(&input, 0, "certificate_pem", "chain_pem") ||
            rj_string(&input, rj_get(&input, 0, "certificate_pem"), leaf, sizeof(leaf)) < 1)
            FAIL(422, "CERTIFICATE_INVALID",
                 "Expected certificate_pem and optional chain_pem array");
        int a = rj_get(&input, 0, "chain_pem");
        if (a >= 0) {
            if (input.t[a].type != '[' || input.t[a].count > 3)
                FAIL(422, "CERTIFICATE_INVALID", "At most 3 chain certificates");
            for (int k = a + 1; k < input.t[a].next; k = input.t[k].next) {
                size_t n = strlen(chain);
                if (rj_string(&input, k, chain + n, sizeof(chain) - n) < 1)
                    FAIL(413, "CERTIFICATE_TOO_LARGE", "Certificate chain limit exceeded");
            }
        }
        int rc = rj_tls_certificate(leaf, chain);
        if (rc)
            FAIL(422, "CERTIFICATE_INVALID",
                 "Certificate malformed, key absent, or leaf does not match resident key");
        rj_tls_status(out, cap);
        return 200;
    }
    if (!strcmp(path, "/api/v1/security/tls/status")) {
        METHOD(get);
        rj_tls_status(out, cap);
        return 200;
    }
    if (!strcmp(path, "/api/v1/blocks") || !strcmp(path, "/api/v1/points")) {
        METHOD(get);
        strcpy(out, "{\"items\":[],\"next_cursor\":null,\"snapshot_id\":\"configuration-only\"}");
        return 200;
    }
    if (!strcmp(path, "/api/v1/runtime")) {
        METHOD(get);
        snprintf(out, cap,
                 "{\"state\":\"empty\",\"configuration_id\":null,\"configuration_revision\":0,"
                 "\"package_sha256\":null,\"processed_events\":0,\"queue_depth\":0,\"dropped_"
                 "events\":0,\"last_error\":null,\"outputs_inhibited\":true,\"boot_id\":\"%s\"}",
                 boot_id);
        return 200;
    }
    if (!strcmp(path, "/api/v1/point-reads") || !strcmp(path, "/api/v1/point-writes") ||
        !strcmp(path, "/api/v1/scans") || !strncmp(path, "/api/v1/deployments", 19) ||
        !strncmp(path, "/api/v1/runtime/", 16) || !strncmp(path, "/api/v1/firmware/", 17) ||
        !strcmp(path, "/api/v1/manufacturing/lock") || !strcmp(path, "/api/v1/manufacturing/trust"))
        FAIL(422, "CAPABILITY_UNSUPPORTED",
             "Not implemented in the RJ45 configuration-only profile");
    FAIL(404, "RESOURCE_NOT_FOUND", "Unknown API resource");
}
int rj_discovery_reply(const char *probe, char *out, size_t cap) {
    static RjJson j;
    if (rj_parse(&j, probe) || !rj_eq(&j, rj_get(&j, 0, "magic"), "AISCELLA_DISCOVERY") ||
        !rj_eq(&j, rj_get(&j, 0, "type"), "probe") || rj_number(&j, rj_get(&j, 0, "version")) != 1)
        return -1;
    char req[65], nonce[65];
    if (rj_string(&j, rj_get(&j, 0, "request_id"), req, sizeof(req)) < 1 || !id_valid(req) ||
        rj_string(&j, rj_get(&j, 0, "nonce"), nonce, sizeof(nonce)) < 22 || !id_valid(nonce))
        return -1;
    int f = rj_get(&j, 0, "product_filter"),
        match = rj_eq(&j, f, "all") || rj_eq(&j, f, "CONNECT-G100");
    if (f >= 0 && j.t[f].type == '[')
        for (int k = f + 1; k < j.t[f].next; k = j.t[k].next)
            if (rj_eq(&j, k, "CONNECT-G100") || rj_eq(&j, k, "all"))
                match = 1;
    if (!match)
        return -1;
    char ip[16];
    ip_string(ip, netif_ip4_addr(&gnetif));
    return snprintf(
        out, cap,
        "{\"magic\":\"AISCELLA_DISCOVERY\",\"version\":1,\"type\":\"device\",\"request_id\":\"%s\","
        "\"nonce\":\"%s\",\"device_id\":\"%s\",\"product\":\"CONNECT-G100\",\"firmware_version\":"
        "\"%s\",\"api_version\":\"1.0\",\"security_mode\":\"development_plaintext\",\"management\":"
        "{\"scheme\":\"http\",\"ip\":\"%s\",\"port\":80,\"tls_name\":null}}",
        req, nonce, uid, G100_FIRMWARE_VERSION, ip);
}
