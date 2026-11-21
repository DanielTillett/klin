/*
 * Copyright (c) 2026 Daniel Tillett
 * All rights reserved.
 *
 * This file is part of Klin (derived from KLone), and as such it is subject to the license stated
 * in the LICENSE file which you have received as part of this distribution.
 */

#ifndef _KLIN_ACL_H_
#define _KLIN_ACL_H_

#include <klin/http.h>
#include <klin/request.h>
#include <klin/vhost.h>

#ifdef __cplusplus
extern "C" {
#endif

/* IP access control of a server or vhost (doc/ACCESS.md), used by the HTTP
 * server */
struct acl_rule_s;
typedef struct acl_rule_s acl_rule_t;

int acl_load(vhost_t *vhost);
void acl_free(vhost_t *vhost);
int acl_check(http_t *h, request_t *rq);
int acl_check_conn(const vhost_t *vhost, const char *peer);

#ifdef __cplusplus
}
#endif

#endif
