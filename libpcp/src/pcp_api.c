/*
 * Copyright (c) 2013 by Cisco Systems, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

#ifdef WIN32
#include <winsock2.h>
#include "pcp_win_defines.h"
#include "pcp_gettimeofday.h"
#else
#include <sys/select.h>
#include <time.h>
#include <sys/types.h>
#include <unistd.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#endif
#include "pcp.h"
#include "pcp_client_db.h"
#include "pcp_logger.h"
#include "pcp_event_handler.h"
#include "pcp_utils.h"
#include "pcp_server_discovery.h"
#include "net/findsaddr.h"

int pcp_add_server(struct sockaddr* pcp_server, uint8_t pcp_version)
{
    int res;

    PCP_LOGGER_BEGIN(PCP_DEBUG_DEBUG);
    if (pcp_version>PCP_MAX_SUPPORTED_VERSION) {
        PCP_LOGGER_END(PCP_DEBUG_INFO);
        return PCP_ERR_UNSUP_VERSION;
    }

    res = psd_add_pcp_server(pcp_server, pcp_version);

    PCP_LOGGER_END(PCP_DEBUG_INFO);
    return res;
}

static inline unsigned long mix(unsigned long a, unsigned long b, unsigned long c)
{
     a=a-b;  a=a-c;  a=a^(c >> 13);
     b=b-c;  b=b-a;  b=b^(a << 8);
     c=c-a;  c=c-b;  c=c^(b >> 13);
     a=a-b;  a=a-c;  a=a^(c >> 12);
     b=b-c;  b=b-a;  b=b^(a << 16);
     c=c-a;  c=c-b;  c=c^(b >> 5);
     a=a-b;  a=a-c;  a=a^(c >> 3);
     b=b-c;  b=b-a;  b=b^(a << 10);
     c=c-a;  c=c-b;  c=c^(b >> 15);

     return c;
}

void pcp_init(uint8_t autodiscovery)
{
    srand(mix(clock(), (unsigned long)time(NULL), getpid()));

    if (autodiscovery)
        psd_add_gws();
}

typedef struct {
    fd_set *read_fds;
    int *fdmax;
} set_fd_data_t;

static int set_fd(pcp_server_t* s, void* data)
{
    set_fd_data_t* wd = (set_fd_data_t*) data;
    if (s->pcp_server_socket == PCP_INVALID_SOCKET) {
        return 0;
    }
    FD_SET(s->pcp_server_socket, (wd->read_fds));

    if (s->pcp_server_socket!=PCP_INVALID_SOCKET) {
        if ((*wd->fdmax < 0) || (*wd->fdmax==PCP_INVALID_SOCKET)||
        ((int)s->pcp_server_socket >= (*wd->fdmax))) {
                *wd->fdmax = s->pcp_server_socket+1;
        }
    }

    return 0;
}

void pcp_set_read_fdset(int *fd_max, fd_set *read_fd_set)
{
    set_fd_data_t d;

    PCP_LOGGER_BEGIN(PCP_DEBUG_DEBUG);
    d.read_fds = read_fd_set;
    d.fdmax = fd_max;
    if ((!fd_max)||(!read_fd_set)) {
        return;
    }
    pcp_db_foreach_server(set_fd,&d);
    PCP_LOGGER_END(PCP_DEBUG_DEBUG);
}

int pcp_eval_flow_state(pcp_flow_t flow, pcp_fstate_e *fstate)
{
    int nexit_states = 0;
    pcp_flow_t fiter;
    int nall = 0;
    int nsuccess = 0;
    int nfailed = 0;
    int nslfail = 0;

    PCP_LOGGER_BEGIN(PCP_DEBUG_DEBUG);

    for (fiter = flow; fiter!=NULL; fiter=fiter->next_child) {
        nall++;
        switch(fiter->state) {
        case pfs_wait_after_short_life_error:
            nexit_states++;
            nslfail++;
            break;
        case pfs_wait_for_lifetime_renew:
            nexit_states++;
            nsuccess++;
            break;
        case pfs_failed:
            nexit_states++;
            nfailed++;
            break;
        default:
            /* Not in any of exit states. Do not increment any of counters */
            break;
        }
    }

    if (fstate) {
        if (nall==nsuccess) {
            *fstate = pcp_state_succeeded;
        } else if (nall==nfailed) {
            *fstate = pcp_state_failed;
        } else if (nall==nfailed+nslfail) {
            *fstate = pcp_state_short_lifetime_error;
        } else if (nall==nsuccess+nfailed+nslfail) {
            *fstate = pcp_state_succeeded;
        } else if ((nexit_states>0) && (nsuccess)) {
            *fstate = pcp_state_partial_result;
        } else {
            *fstate = pcp_state_processing;
        }
    }

    PCP_LOGGER_END(PCP_DEBUG_DEBUG);
    return nexit_states;
}

pcp_fstate_e pcp_wait(pcp_flow_t flow, int timeout, int exit_on_partial_res)
{
    fd_set read_fds;
    int fdmax;
    struct timeval tout_end;
    struct timeval tout_select;
    pcp_fstate_e fstate;
    int nflow_exit_states = pcp_eval_flow_state(flow, &fstate);

    PCP_LOGGER_BEGIN(PCP_DEBUG_DEBUG);

    if(!flow) {
        PCP_LOGGER(PCP_DEBUG_PERR,
                "Flow argument of %s function set to NULL!", __FUNCTION__);
        return pcp_state_failed;
    }

    switch (fstate) {
    case pcp_state_partial_result:
    case pcp_state_processing:
        break;
    default:
        nflow_exit_states = 0;
        break;
    }

    gettimeofday(&tout_end, NULL);
    tout_end.tv_usec += (timeout * 1000) % 1000000;
    tout_end.tv_sec += tout_end.tv_usec / 1000000;
    tout_end.tv_usec = tout_end.tv_usec % 1000000;
    tout_end.tv_sec += timeout / 1000;

    fdmax = 0;

    PCP_LOGGER(PCP_DEBUG_INFO,
            "Initialized wait for result of flow: %d, wait timeout %d ms",
            flow->key_bucket, timeout);

    FD_ZERO(&read_fds);

    // main loop
    for (;;) {
        int ret_count;
        pcp_fstate_e ret_state;
        struct timeval ctv;

        OSDEP(ret_count);
        // check expiration of wait timeout
        gettimeofday(&ctv, NULL);
        if ((timeval_subtract(&tout_select, &tout_end, &ctv))
                || ((tout_select.tv_sec == 0) && (tout_select.tv_usec == 0))
                || (tout_select.tv_sec < 0)) {
            return pcp_state_processing;
        }

        //process all events and get timeout value for next select
        pcp_handle_select(fdmax, &read_fds, &tout_select);

        // check flow for reaching one of exit from wait states
        // (also handles case when flow is MAP for 0.0.0.0)
        if (pcp_eval_flow_state(flow, &ret_state) > nflow_exit_states)
        {
            if ((exit_on_partial_res)||(ret_state!=pcp_state_partial_result)) {
                PCP_LOGGER_END(PCP_DEBUG_DEBUG);
                return ret_state;
            }
        }

        FD_ZERO(&read_fds);
        pcp_set_read_fdset(&fdmax, &read_fds);

        PCP_LOGGER(PCP_DEBUG_DEBUG,
                "Executing select with "
                        "fdmax=%d, timeout = %ld s; %ld us", fdmax,
                tout_select.tv_sec, tout_select.tv_usec);

        ret_count = select(fdmax, &read_fds, NULL, NULL, &tout_select);

        // check of select result // only for debug purposes
#ifdef DEBUG
        if (ret_count == -1) {
            char error[ERR_BUF_LEN];
            pcp_strerror(errno, error, sizeof(error));
            PCP_LOGGER(PCP_DEBUG_PERR,
                    "select failed: %s", error);
        } else if (ret_count == 0) {
            PCP_LOGGER(PCP_DEBUG_DEBUG, "%s",
                    "select timed out");
        } else {
            PCP_LOGGER(PCP_DEBUG_DEBUG,
                    "select returned %d i/o events.", ret_count);
        }
#endif
    }
    PCP_LOGGER_END(PCP_DEBUG_DEBUG);
    return pcp_state_succeeded;
}

static inline void fill_in6_addr(struct in6_addr *dst_ip6, uint16_t *dst_port,
        struct sockaddr* src)
{
    if (src->sa_family == AF_INET) {
        struct sockaddr_in* src_ip4 = (struct sockaddr_in*) src;
        if (src_ip4->sin_addr.s_addr != INADDR_ANY) {
            S6_ADDR32(dst_ip6)[0] = 0;
            S6_ADDR32(dst_ip6)[1] = 0;
            S6_ADDR32(dst_ip6)[2] = htonl(0xFFFF);
            S6_ADDR32(dst_ip6)[3] = src_ip4->sin_addr.s_addr;
        } else {
            unsigned i;
            for (i=0; i<4; ++i)
                S6_ADDR32(dst_ip6)[i]=0;
        }
        *dst_port = src_ip4->sin_port;
    } else if (src->sa_family == AF_INET6) {
        struct sockaddr_in6* src_ip6 = (struct sockaddr_in6*) src;
        memcpy(dst_ip6,
                src_ip6->sin6_addr.s6_addr,
                sizeof(*dst_ip6));
        *dst_port = src_ip6->sin6_port;
    }
}

static inline void
init_flow(pcp_flow_t f, pcp_server_t* s, int lifetime,
        struct sockaddr* ext_addr)
{
    PCP_LOGGER_BEGIN(PCP_DEBUG_DEBUG);
    if (f && s) {
        struct timeval curtime;

        if (ext_addr) {
            fill_in6_addr(&f->map_peer.ext_ip,
                &f->map_peer.ext_port, ext_addr);
        }

        gettimeofday(&curtime, NULL);
        f->lifetime = lifetime;
        f->timeout = curtime;

        if (s->server_state == pss_wait_io) {
            f->state = pfs_send;
        } else {
            f->state = pfs_wait_for_server_init;
        }
        s->next_timeout = curtime;

        pcp_db_add_flow(f);

        if (pcp_log_level>=PCP_DEBUG_INFO) {
            char src_buf[INET6_ADDRSTRLEN];
            char dst_buf[INET6_ADDRSTRLEN];
            char pcp_buf[INET6_ADDRSTRLEN];
            inet_ntop(AF_INET6, &f->kd.src_ip, src_buf, sizeof(src_buf));
            inet_ntop(AF_INET6, &f->kd.map_peer.dst_ip, dst_buf,
                sizeof(dst_buf));
            inet_ntop(AF_INET6, &f->kd.pcp_server_ip, pcp_buf, sizeof(pcp_buf));
            PCP_LOGGER(PCP_DEBUG_INFO,
                "Added new flow info:\n\tPCP server: %s\n\tInt.  addr: [%s]:%d \n\tDest. addr: [%s]:%d\n"
                "\tKey bucket: %d",
                pcp_buf,
                src_buf,
                ntohs(f->kd.map_peer.src_port),
                dst_buf,
                ntohs(f->kd.map_peer.dst_port), f->key_bucket);
        }
    }
    PCP_LOGGER_END(PCP_DEBUG_DEBUG);
}

struct caasi_data {
    struct flow_key_data *kd;
    pcp_flow_t  fprev;
    pcp_flow_t  ffirst;
    uint32_t    lifetime;
    struct sockaddr* ext_addr;
    struct in6_addr* src_ip;
    uint8_t toler_fields;
    char* app_name;
};

static int chain_and_assign_src_ip(pcp_server_t* s, void * data)
{
    struct caasi_data *d = (struct caasi_data *)data;

    PCP_LOGGER_BEGIN(PCP_DEBUG_DEBUG);

    if (s->server_state == pss_not_working) {
        PCP_LOGGER_END(PCP_DEBUG_DEBUG);
        return 0;
    }

    if ((IN6_IS_ADDR_UNSPECIFIED(d->src_ip)) ||
            (IN6_ARE_ADDR_EQUAL(d->src_ip, (struct in6_addr *) s->src_ip))) {

        pcp_flow_t f = NULL;

        memcpy(&d->kd->src_ip, s->src_ip, sizeof(d->kd->src_ip));
        memcpy(&d->kd->pcp_server_ip, s->pcp_ip, sizeof(d->kd->pcp_server_ip));
        memcpy(&d->kd->nonce, &s->nonce, sizeof(d->kd->nonce));

        //f = pcp_create_flow(s, d->kd);
        CHECK_NULL_EXIT((f = pcp_create_flow(s, d->kd)));
        if (d->kd->operation == PCP_OPCODE_SADSCP) {
            f->sadscp.toler_fields = d->toler_fields;
            if (d->app_name) {
                f->sadscp.app_name_length = strlen(d->app_name);
                f->sadscp_app_name = strdup(d->app_name);
            } else {
                f->sadscp.app_name_length = 0;
                f->sadscp_app_name = NULL;
            }
        }
        init_flow(f, s, d->lifetime, d->ext_addr);
        if (d->fprev) {
            d->fprev->next_child = f;
        } else {
            d->ffirst = f;
        }
        d->fprev = f;
    }

    PCP_LOGGER_END(PCP_DEBUG_DEBUG);
    return 0;
}

pcp_flow_t pcp_new_flow(
        struct sockaddr* src_addr,
        struct sockaddr* dst_addr,
        struct sockaddr* ext_addr,
        uint8_t protocol, uint32_t lifetime)
{
    struct flow_key_data    kd;
    struct caasi_data data;
    struct in6_addr src_ip;

    PCP_LOGGER_BEGIN(PCP_DEBUG_DEBUG);

    memset(&kd, 0, sizeof(kd));

    if (!src_addr) {
        return NULL;
    }
    fill_in6_addr(&src_ip, &kd.map_peer.src_port, src_addr);

    kd.map_peer.protocol = protocol;

    if (dst_addr) {
        switch (dst_addr->sa_family)
        {
        case AF_INET:
            if (((struct sockaddr_in*)(dst_addr))->sin_addr.s_addr == INADDR_ANY) {
                dst_addr = NULL;
            }
            break;
        case AF_INET6:
            if (IN6_IS_ADDR_UNSPECIFIED(
                    &((struct sockaddr_in6 *)(dst_addr))->sin6_addr)) {
                dst_addr = NULL;
            }
            break;
        default:
            dst_addr = NULL;
            break;
        }
    }

    if (dst_addr) {
        fill_in6_addr(&kd.map_peer.dst_ip, &kd.map_peer.dst_port,
            dst_addr);
        kd.operation = PCP_OPCODE_PEER;

#if 0
// get gw
#ifdef __APPLE__
        memcpy(&(routein.dst4), dst_addr, sizeof(struct sockaddr_in));
        route_get_sa(dst_addr, NULL, &gw_sock, &ifname[0], &routein, &routeout);
        if (get_if_addr_from_name(&(routeout.ifname[0]), &ifsock, AF_INET)) {
        //if (get_if_addr_from_name(&ifname[0], &ifsock, AF_INET)) {
            exit(EXIT_FAILURE);
        }
        if (ifsock.sa_family == AF_INET) {
            srcip.s6_addr32[3] = ((struct sockaddr_in *) &ifsock)->sin_addr.s_addr;
            gw_ip.s6_addr32[3] = routeout.gw4.sin_addr.s_addr;
            //gw_ip.s6_addr32[3] = ((struct sockaddr_in *) &gw_sock)->sin_addr.s_addr;
        } else if (ifsock.sa_family == AF_INET6) {
            srcip = ((struct sockaddr_in6 *) &ifsock)->sin6_addr;
            gw_ip = ((struct sockaddr_in6 *) &gw_sock)->sin6_addr;
        } else {
            PCP_LOGGER(PCP_DEBUG_WARN, "%s", "Unknown Address Family");
            PCP_LOGGER_END(PCP_DEBUG_DEBUG);
            return NULL;
        }

        gw_ip = ((struct sockaddr_in6 *) &gw_sock)->sin6_addr;
#else
        if (get_src_gw_for_route_to(dst_addr, &srcip, &gw_ip) < 0) {
            PCP_LOGGER(PCP_DEBUG_WARN, "%s", "Can't get gateway to PCP server.");
            return NULL;
        }
#endif
#endif
        if (src_addr->sa_family == AF_INET) {
            if (S6_ADDR32(&src_ip)[3] == INADDR_ANY) {
                findsaddr((struct sockaddr_in*)dst_addr, &src_ip);
            }
        } else if (IN6_IS_ADDR_UNSPECIFIED(&src_ip)) {
                findsaddr6((struct sockaddr_in6*)dst_addr, &src_ip);
        }
    } else {
        kd.operation = PCP_OPCODE_MAP;
    }

    data.fprev = NULL;
    data.lifetime = lifetime;
    data.ext_addr = ext_addr;
    data.src_ip = &src_ip;
    data.kd = &kd;
    data.ffirst = NULL;

    pcp_db_foreach_server(chain_and_assign_src_ip, &data);

    PCP_LOGGER_END(PCP_DEBUG_DEBUG);
    return data.ffirst;
}

void pcp_flow_set_lifetime(pcp_flow_t f, uint32_t lifetime)
{
    pcp_flow_t fiter;
    for (fiter = f; fiter!=NULL; fiter=fiter->next_child) {
        fiter->lifetime = lifetime;

        pcp_flow_updated(fiter);
    }
}

void pcp_set_3rd_party_opt (UNUSED pcp_flow_t f,
                            UNUSED struct sockaddr* thirdp_addr)
{

}

void pcp_flow_set_filter_opt(pcp_flow_t f, struct sockaddr *filter_ip,
                             uint8_t filter_prefix)
{
    pcp_flow_t fiter;
    for (fiter = f; fiter != NULL; fiter = fiter->next_child){
        if (!fiter->filter_option_present){
            fiter->filter_option_present = 1;
        }
        fill_in6_addr(&fiter->filter_ip, &fiter->filter_port, filter_ip);
        fiter->filter_prefix = filter_prefix;
        pcp_flow_updated(fiter);
    }
}

void pcp_flow_set_prefer_failure_opt (pcp_flow_t f)
{
    pcp_flow_t fiter;
    for (fiter = f; fiter != NULL; fiter = fiter->next_child){
        if (!fiter->pfailure_option_present) {
            fiter->pfailure_option_present = 1;
            pcp_flow_updated(fiter);
        }
    }
}

int pcp_flow_set_userid(pcp_flow_t f, pcp_userid_option_p user)
{
    pcp_flow_t fiter;
    for (fiter = f; fiter != NULL; fiter = fiter->next_child) {
        memcpy(&(fiter->f_userid.userid[0]), &(user->userid[0]), MAX_USER_ID);
        pcp_flow_updated(fiter);
    }
    return 0;
}


int pcp_flow_set_location(pcp_flow_t f, pcp_location_option_p loc)
{
    pcp_flow_t fiter;
    for (fiter = f; fiter != NULL; fiter = fiter->next_child) {
        memcpy(&(fiter->f_location.location[0]), &(loc->location[0]), MAX_GEO_STR);
        pcp_flow_updated(fiter);
    }

    return 0;
}

int pcp_flow_set_deviceid(pcp_flow_t f, pcp_deviceid_option_p dev)
{
    pcp_flow_t fiter;
    for (fiter = f; fiter != NULL; fiter = fiter->next_child) {
        memcpy(&(fiter->f_deviceid.deviceid[0]), &(dev->deviceid[0]), MAX_DEVICE_ID);
        pcp_flow_updated(fiter);
    }
    return 0;
}


void pcp_flow_set_flowp(pcp_flow_t f, uint8_t dscp_up, uint8_t dscp_down)
{
    pcp_flow_t fiter;
    for (fiter = f; fiter!=NULL; fiter=fiter->next_child) {
        uint8_t fpresent = (dscp_up!=0)||(dscp_down!=0);
        if (fiter->flowp_option_present != fpresent) {
            fiter->flowp_option_present = fpresent;
        }
        if (fpresent) {
            fiter->flowp_dscp_up = dscp_up;
            fiter->flowp_dscp_down = dscp_down;
        }
        pcp_flow_updated(fiter);
    }
}

void
pcp_flow_add_md (pcp_flow_t f, uint32_t md_id, void *value, size_t val_len)
{
    pcp_flow_t fiter;
    for (fiter = f; fiter!=NULL; fiter=fiter->next_child) {
        pcp_db_add_md(fiter, md_id, value, val_len);
        pcp_flow_updated(fiter);
    }
}

static inline void pcp_close_flow_intern(pcp_flow_t f)
{
    if ((f->state!= pfs_wait_for_server_init) &&
            (f->state!= pfs_idle) &&
            (f->state!= pfs_failed)) {

        f->lifetime = 0;
        pcp_flow_updated(f);
    } else {
        f->state = pfs_failed;
    }
}

void pcp_close_flow(pcp_flow_t f)
{
    pcp_flow_t fiter;
    for (fiter = f; fiter!=NULL; fiter=fiter->next_child) {
        pcp_close_flow_intern(fiter);
    }
    pcp_pulse(NULL);
}

void pcp_delete_flow(pcp_flow_t f)
{
    pcp_flow_t fiter = f, fnext = NULL;
    while (fiter != NULL) {
        fnext = fiter->next;
        pcp_delete_flow_intern(fiter);
        fiter = fnext;
    }
}

static int delete_flow_iter(pcp_flow_t f, void * data)
{
    if (*(int*)data) {
        pcp_close_flow_intern(f);
    }
    pcp_delete_flow_intern(f);

    return 0;
}

void pcp_terminate(int close_flows)
{
    /* Causes compilation warning in x64 machines since pointer is 64-bit. Fixed*/
    pcp_db_foreach_flow(delete_flow_iter,(void*)(int *)&close_flows);
    pcp_db_free_pcp_servers();
}

pcp_flow_info_t*
pcp_flow_get_info(pcp_flow_t f, pcp_flow_info_t **info_buf, size_t *info_count)
{
    pcp_flow_t fiter = f;
    uint32_t   n = 0;

    if ((!info_buf)||(!info_count)) {
        return NULL;
    }

     for (;fiter!=NULL; fiter=fiter->next_child) {
        *info_buf = (pcp_flow_info_t *)realloc(*info_buf,
            ++n * sizeof(pcp_flow_info_t));
        memset((*info_buf) + n - 1, 0, sizeof(pcp_flow_info_t));

        switch (fiter->state) {
        case pfs_wait_after_short_life_error:
            (*info_buf)[n-1].result = pcp_state_short_lifetime_error;
            break;
        case pfs_wait_for_lifetime_renew:
            (*info_buf)[n-1].result = pcp_state_succeeded;
            break;
        case pfs_failed:
            (*info_buf)[n-1].result = pcp_state_failed;
            break;
        default:
            (*info_buf)[n-1].result = pcp_state_processing;
            break;
        }

        (*info_buf)[n-1].recv_lifetime_end = fiter->recv_lifetime;
        (*info_buf)[n-1].lifetime_renew_s = fiter->lifetime;
        (*info_buf)[n-1].pcp_result_code = fiter->recv_result;
        memcpy(&(*info_buf)[n-1].int_ip, &fiter->kd.src_ip,
                sizeof((*info_buf)->int_ip));
        memcpy(&(*info_buf)[n-1].pcp_server_ip, &fiter->kd.pcp_server_ip,
                sizeof((*info_buf)->pcp_server_ip));
        if ((fiter->kd.operation == PCP_OPCODE_MAP) ||
            (fiter->kd.operation == PCP_OPCODE_PEER)) {
            memcpy(&(*info_buf)[n-1].dst_ip, &fiter->kd.map_peer.dst_ip,
            sizeof((*info_buf)->dst_ip));
            memcpy(&(*info_buf)[n-1].ext_ip, &fiter->map_peer.ext_ip,
            sizeof((*info_buf)->ext_ip));
            (*info_buf)[n-1].int_port = fiter->kd.map_peer.src_port;
            (*info_buf)[n-1].dst_port = fiter->kd.map_peer.dst_port;
            (*info_buf)[n-1].ext_port = fiter->map_peer.ext_port;
            (*info_buf)[n-1].protocol = fiter->kd.map_peer.protocol;
        } else if (fiter->kd.operation == PCP_OPCODE_SADSCP){
            (*info_buf)[n-1].learned_dscp = fiter->sadscp.learned_dscp;
        }
    }
    *info_count = n;

    return (*info_buf);
}


pcp_flow_t pcp_learn_dscp(uint8_t delay_tol, uint8_t loss_tol,
uint8_t jitter_tol, char* app_name)
{
    struct flow_key_data kd;
    struct caasi_data data;
    struct in6_addr src_ip = IN6ADDR_ANY_INIT;
    memset(&data, 0 ,sizeof(data));
    memset(&kd, 0 ,sizeof(kd));

    kd.operation = PCP_OPCODE_SADSCP;

    data.fprev = NULL;

    data.fprev = NULL;
    data.src_ip = &src_ip;
    data.kd = &kd;
    data.ffirst = NULL;

    data.lifetime = 0;
    data.ext_addr = NULL;
    data.toler_fields =
        (delay_tol&3)<<6 | ((loss_tol&3)<<4) | ((jitter_tol&3)<<2);
    data.app_name = app_name;
    pcp_db_foreach_server(chain_and_assign_src_ip, &data);

    return data.ffirst;
}