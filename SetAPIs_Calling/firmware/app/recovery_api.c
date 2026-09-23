/* Small factory service: no protocol catalogue, user configuration or application logic. */
#include "rj_api.h"
#include "rj_security.h"
#include "architecture.h"
#include "main.h"
#include "device_config.h"
#include "board_status.h"
#include <stdio.h>
#include <string.h>
void rj_api_init(void) { rj_security_init(); }
void rj_api_process(uint32_t now) { (void)now; }
int rj_discovery_reply(const char *probe,char *out,size_t cap) { (void)probe;(void)out;(void)cap;return 0; }
int rj_api_handle(const RjRequest *r,char *out,size_t cap,char *extra,size_t extra_cap) {
    if(extra_cap) extra[0]=0;
    if(!strcmp(r->method,"GET") && !strcmp(r->path,"/api/v1/identity")) {
        (void)snprintf(out,cap,"{\"device_id\":\"%08lX%08lX%08lX\",\"firmware_version\":\"%s\",\"role\":\"recovery\"}",
            (unsigned long)HAL_GetUIDw0(),(unsigned long)HAL_GetUIDw1(),(unsigned long)HAL_GetUIDw2(),G100_FIRMWARE_VERSION);
        return 200;
    }
    if(!strcmp(r->method,"GET") && !strcmp(r->path,"/api/v1/health")) {
        (void)snprintf(out,cap,"{\"status\":\"%s\",\"management_ready\":%s,\"role\":\"recovery\"}",
            g_board_status.fault?"error":"ok",g_board_status.fault?"false":"true");return 200;
    }
    if(!strcmp(r->method,"GET") && !strcmp(r->path,"/api/v1/system/memory")) {
        g100_architecture_json(out,cap);return 200;
    }
    (void)snprintf(out,cap,"{\"error\":\"RECOVERY_SERVICE_ONLY\"}");return 404;
}
