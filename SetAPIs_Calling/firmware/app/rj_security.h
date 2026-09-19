#ifndef RJ_SECURITY_H
#define RJ_SECURITY_H
#include <stddef.h>
/* One replaceable policy entry point. All callers are allowed in this dev build.
 * Future security provider must authenticate AND enforce per-route scopes here.
 */
#define RJ_DEVELOPMENT_OPEN 1
int rj_authorize(const char *method,const char *path);
void rj_security_init(void);
int rj_random(void *unused,unsigned char *out,size_t size);
int rj_credentials(const char *username,const char *password);
int rj_credentials_status(char *out,size_t cap);
int rj_tls_csr(const char *common_name,char *out,size_t cap);
int rj_tls_certificate(const char *pem,const char *chain);
int rj_tls_status(char *out,size_t cap);
void rj_boot_id(char out[40]);
#endif
