/****************************************************************************
 *
 * Copyright (C) 2014 Cisco and/or its affiliates. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License Version 2 as
 * published by the Free Software Foundation.  You may not use, modify or
 * distribute this program under any other version of the GNU General
 * Public License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 ****************************************************************************/

#include "flow/flow_control.h"

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <assert.h>

#include "flow/flow_cache.h"
#include "flow/expect_cache.h"
#include "flow/session.h"
#include "packet_io/active.h"
#include "packet_io/sfdaq.h"

FlowControl::FlowControl()
{
    ip_cache = nullptr;
    icmp_cache = nullptr;
    tcp_cache = nullptr;
    udp_cache = nullptr;
    exp_cache = nullptr;
}

FlowControl::~FlowControl()
{
    delete tcp_cache;
    delete udp_cache;
    delete icmp_cache;
    delete ip_cache;
    delete exp_cache;
}

//-------------------------------------------------------------------------
// cache foo
//-------------------------------------------------------------------------

inline FlowCache* FlowControl::get_cache (int proto)
{
    switch ( proto ) {
    case IPPROTO_TCP:  return tcp_cache;
    case IPPROTO_UDP:  return udp_cache;
    case IPPROTO_ICMP: return icmp_cache;
    case IPPROTO_IP:   return ip_cache;
    }
    return NULL;
}

Flow* FlowControl::find_flow (const FlowKey* key)
{
    FlowCache* cache = get_cache(key->protocol);

    if ( cache )
        return cache->find(key);

    return NULL;
}

Flow* FlowControl::new_flow (const FlowKey* key)
{
    FlowCache* cache = get_cache(key->protocol);

    if ( !cache )
        return NULL;

    return cache->get(key);
}

// FIXIT cache* can be put in flow so that lookups by
// protocol are obviated for existing / initialized flows
void FlowControl::delete_flow (const FlowKey* key)
{
    FlowCache* cache = get_cache(key->protocol);

    if ( !cache )
        return;

    Flow* flow = cache->find(key);

    if ( flow )
        cache->release(flow, "ha sync");
}

void FlowControl::delete_flow (Flow* flow, const char* why)
{
    FlowCache* cache = get_cache(flow->protocol);

    if ( cache )
        cache->release(flow, why);
}

void FlowControl::purge_flows (int proto)
{
    FlowCache* cache = get_cache(proto);

    if ( cache )
        cache->purge();
}

void FlowControl::prune_flows (int proto, Packet* p)
{
    FlowCache* cache = get_cache(proto);

    if ( !cache )
        return;

    // smack the older timed out flows
    if (!cache->prune_stale(p->pkth->ts.tv_sec, (Flow*)p->flow))
    {
        // if no luck, try the memcap
        cache->prune_excess(true, (Flow*)p->flow);
    }
}

void FlowControl::timeout_flows(uint32_t flowCount, time_t cur_time)
{
    Active_Suspend();

    if ( tcp_cache )
        tcp_cache->timeout(flowCount, cur_time);

    if ( udp_cache )
        udp_cache->timeout(flowCount, cur_time);

    //if ( icmp_cache )
    //icmp_cache does not need cleaning

    if ( ip_cache )
        ip_cache->timeout(flowCount, cur_time);

    Active_Resume();
}

uint32_t FlowControl::max_flows(int proto)
{
    FlowCache* cache = get_cache(proto);

    if ( cache )
        return cache->get_max_flows();

    return 0;
}

void FlowControl::get_prunes (int proto, PegCount& prunes)
{
    FlowCache* cache = get_cache(proto);

    if ( cache )
        prunes = cache->get_prunes();
}

void FlowControl::reset_prunes (int proto)
{
    FlowCache* cache = get_cache(proto);

    if ( cache )
        cache->reset_prunes();
}

void FlowControl::set_key(FlowKey* key, const Packet* p)
{
    char proto = GET_IPH_PROTO(p);
    uint32_t mplsId;
    uint16_t vlanId;
    uint16_t addressSpaceId;

    if ( p->vh )
        vlanId = (uint16_t)VTH_VLAN(p->vh);
    else
        vlanId = 0;

    if ( p->mpls )
        mplsId = p->mplsHdr.label;
    else
        mplsId = 0;

#ifdef HAVE_DAQ_ADDRESS_SPACE_ID
    addressSpaceId = DAQ_GetAddressSpaceID(p->pkth);
#else
    addressSpaceId = 0;
#endif

    if ( p->frag_flag )
    {
        key->init(GET_SRC_IP(p), GET_DST_IP(p), GET_IPH_ID(p),
            proto, vlanId, mplsId, addressSpaceId);
    }
    else if ((proto == IPPROTO_ICMP) || (proto == IPPROTO_ICMPV6))
    {
        key->init(GET_SRC_IP(p), p->icmph->type, GET_DST_IP(p), 0,
            proto, vlanId, mplsId, addressSpaceId);
    }
    else
    {
        key->init(GET_SRC_IP(p), p->sp, GET_DST_IP(p), p->dp,
            proto, vlanId, mplsId, addressSpaceId);
    }
}

static bool is_bidirectional(Flow* flow)
{
    constexpr unsigned bidir = SSNFLAG_SEEN_CLIENT | SSNFLAG_SEEN_SERVER;
    return (flow->s5_state.session_flags & bidir) == bidir;
}

void FlowControl::process(FlowCache* cache, Inspector* ins, Packet* p)
{
    FlowKey key;
    set_key(&key, p);

    Flow* flow = cache->get(&key);

    if ( !flow )
        return;

    if ( flow->init )
    {
#if 0
        // FIXIT BIND this is where bindings are used to set
        // inspectors on session:
        if ( bindings )
        {
            // -- must set client and server session inspectors
            // -- must set all bound service inspectors 
            //    (service inspectors also set later by auto id)
        }
        else
#endif
        {
            // default case
            flow->client = ins;
            flow->server = ins;
        }
        // -- all inspectors must be ref counted when set
        flow->client->add_ref();
        flow->server->add_ref();

        if ( !flow->session->setup(p) )
            return;

        flow->init = false;
    }

    p->flow = flow;
    flow->session->process(p);

    if ( flow->init && is_bidirectional(flow) )
        cache->unlink_uni(flow);
}

//-------------------------------------------------------------------------
// tcp
//-------------------------------------------------------------------------

void FlowControl::init_tcp(
    const FlowConfig& fc, InspectSsnFunc get_ssn)
{
    if ( !fc.max_sessions )
    {
        tcp_cache = nullptr;
        return;
    }
    tcp_cache = new FlowCache(
        fc.max_sessions, fc.cache_pruning_timeout,
        fc.cache_nominal_timeout, 5, 0);

    for ( unsigned i = 0; i < fc.max_sessions; ++i )
    {
        Flow* flow = new Flow(IPPROTO_TCP);
        flow->session = get_ssn(flow);
        tcp_cache->push(flow);
    }
}

void FlowControl::process_tcp(Inspector* user, Packet* p)
{
    if( !p->tcph || !tcp_cache )
        return;

    process(tcp_cache, user, p);
}

//-------------------------------------------------------------------------
// udp
//-------------------------------------------------------------------------

void FlowControl::init_udp(
    const FlowConfig& fc, InspectSsnFunc get_ssn)
{
    if ( !fc.max_sessions )
    {
        udp_cache = nullptr;
        return;
    }
    udp_cache = new FlowCache(
        fc.max_sessions, fc.cache_pruning_timeout,
        fc.cache_nominal_timeout, 5, 0);

    for ( unsigned i = 0; i < fc.max_sessions; ++i )
    {
        Flow* flow = new Flow(IPPROTO_UDP);
        flow->session = get_ssn(flow);
        udp_cache->push(flow);
    }
}

void FlowControl::process_udp(Inspector* user, Packet* p)
{
    if( !p->udph || !udp_cache )
        return;

    process(udp_cache, user, p);
}

//-------------------------------------------------------------------------
// icmp
//-------------------------------------------------------------------------

void FlowControl::init_icmp(
    const FlowConfig& fc, InspectSsnFunc get_ssn)
{
    if ( !fc.max_sessions )
    {
        icmp_cache = nullptr;
        return;
    }
    icmp_cache = new FlowCache(
        fc.max_sessions, fc.cache_pruning_timeout,
        fc.cache_nominal_timeout, 5, 0);

    for ( unsigned i = 0; i < fc.max_sessions; ++i )
    {
        Flow* flow = new Flow(IPPROTO_ICMP);
        flow->session = get_ssn(flow);
        icmp_cache->push(flow);
    }
}

void FlowControl::process_icmp(Inspector* user, Packet* p)
{
    if ( !p->icmph )
        return;

    if ( icmp_cache )
        process(icmp_cache, user, p);

    else
        process_ip(user, p);
}

//-------------------------------------------------------------------------
// ip
//-------------------------------------------------------------------------

void FlowControl::init_ip(
    const FlowConfig& fc, InspectSsnFunc get_ssn)
{
    if ( !fc.max_sessions )
    {
        ip_cache = nullptr;
        return;
    }
    ip_cache = new FlowCache(
        fc.max_sessions, fc.cache_pruning_timeout,
        fc.cache_nominal_timeout, 5, 0);

    for ( unsigned i = 0; i < fc.max_sessions; ++i )
    {
        Flow* flow = new Flow(IPPROTO_IP);
        flow->session = get_ssn(flow);
        ip_cache->push(flow);
    }
}

void FlowControl::process_ip(Inspector* user, Packet* p)
{
    if ( !p->iph || !ip_cache )
        return;

    process(ip_cache, user, p);
}

//-------------------------------------------------------------------------
// expected
//-------------------------------------------------------------------------

void FlowControl::init_exp(
    const FlowConfig& tcp, const FlowConfig& udp)
{
    uint32_t max = tcp.max_sessions + udp.max_sessions;
    max >>= 9;

    if ( !max )
        max = 2;

    exp_cache = new ExpectCache(max);
}

char FlowControl::expected_flow (Flow* flow, Packet* p)
{
    char ignore = exp_cache->check(p, flow);

    if ( ignore )
    {
        DEBUG_WRAP(DebugMessage(DEBUG_STREAM_STATE,
            "Stream5: Ignoring packet from %d. Marking flow marked as ignore.\n",
            p->packet_flags & PKT_FROM_CLIENT? "sender" : "responder"););

        flow->s5_state.ignore_direction = ignore;
        DisableInspection(p);
    }

    return ignore;
}

int FlowControl::add_expected(
    snort_ip_p srcIP, uint16_t srcPort,
    snort_ip_p dstIP, uint16_t dstPort,
    uint8_t protocol, char direction,
    FlowData* fd)
{
    return exp_cache->add_flow(
        srcIP, srcPort, dstIP, dstPort, protocol, direction, fd);
}

int FlowControl::add_expected(
    snort_ip_p srcIP, uint16_t srcPort,
    snort_ip_p dstIP, uint16_t dstPort,
    uint8_t protocol, int16_t appId,
    FlowData* fd)
{
    return exp_cache->add_flow(
        srcIP, srcPort, dstIP, dstPort, protocol, SSN_DIR_BOTH, fd, appId);
}

bool FlowControl::is_expected(Packet* p)
{
    return exp_cache->is_expected(p);
}

