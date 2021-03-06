/*
 * Copyright (c) 2017 Cisco and/or its affiliates.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
/**
 * @file
 * @brief NAT64 IPv6 to IPv4 translation (inside to outside network)
 */

#include <nat/nat64.h>
#include <nat/nat_reass.h>
#include <nat/nat_inlines.h>
#include <vnet/ip/ip6_to_ip4.h>
#include <vnet/fib/fib_table.h>

typedef struct
{
  u32 sw_if_index;
  u32 next_index;
  u8 is_slow_path;
} nat64_in2out_trace_t;

static u8 *
format_nat64_in2out_trace (u8 * s, va_list * args)
{
  CLIB_UNUSED (vlib_main_t * vm) = va_arg (*args, vlib_main_t *);
  CLIB_UNUSED (vlib_node_t * node) = va_arg (*args, vlib_node_t *);
  nat64_in2out_trace_t *t = va_arg (*args, nat64_in2out_trace_t *);
  char *tag;

  tag = t->is_slow_path ? "NAT64-in2out-slowpath" : "NAT64-in2out";

  s =
    format (s, "%s: sw_if_index %d, next index %d", tag, t->sw_if_index,
	    t->next_index);

  return s;
}

typedef struct
{
  u32 sw_if_index;
  u32 next_index;
  u8 cached;
} nat64_in2out_reass_trace_t;

static u8 *
format_nat64_in2out_reass_trace (u8 * s, va_list * args)
{
  CLIB_UNUSED (vlib_main_t * vm) = va_arg (*args, vlib_main_t *);
  CLIB_UNUSED (vlib_node_t * node) = va_arg (*args, vlib_node_t *);
  nat64_in2out_reass_trace_t *t =
    va_arg (*args, nat64_in2out_reass_trace_t *);

  s =
    format (s, "NAT64-in2out-reass: sw_if_index %d, next index %d, status %s",
	    t->sw_if_index, t->next_index,
	    t->cached ? "cached" : "translated");

  return s;
}


#define foreach_nat64_in2out_error                       \
_(UNSUPPORTED_PROTOCOL, "unsupported protocol")          \
_(IN2OUT_PACKETS, "good in2out packets processed")       \
_(NO_TRANSLATION, "no translation")                      \
_(UNKNOWN, "unknown")                                    \
_(DROP_FRAGMENT, "drop fragment")                        \
_(MAX_REASS, "maximum reassemblies exceeded")            \
_(MAX_FRAG, "maximum fragments per reassembly exceeded") \
_(TCP_PACKETS, "TCP packets")                            \
_(UDP_PACKETS, "UDP packets")                            \
_(ICMP_PACKETS, "ICMP packets")                          \
_(OTHER_PACKETS, "other protocol packets")               \
_(FRAGMENTS, "fragments")                                \
_(CACHED_FRAGMENTS, "cached fragments")                  \
_(PROCESSED_FRAGMENTS, "processed fragments")


typedef enum
{
#define _(sym,str) NAT64_IN2OUT_ERROR_##sym,
  foreach_nat64_in2out_error
#undef _
    NAT64_IN2OUT_N_ERROR,
} nat64_in2out_error_t;

static char *nat64_in2out_error_strings[] = {
#define _(sym,string) string,
  foreach_nat64_in2out_error
#undef _
};

typedef enum
{
  NAT64_IN2OUT_NEXT_IP4_LOOKUP,
  NAT64_IN2OUT_NEXT_IP6_LOOKUP,
  NAT64_IN2OUT_NEXT_DROP,
  NAT64_IN2OUT_NEXT_SLOWPATH,
  NAT64_IN2OUT_NEXT_REASS,
  NAT64_IN2OUT_N_NEXT,
} nat64_in2out_next_t;

typedef struct nat64_in2out_set_ctx_t_
{
  vlib_buffer_t *b;
  vlib_main_t *vm;
  u32 thread_index;
} nat64_in2out_set_ctx_t;

static inline u8
nat64_not_translate (u32 sw_if_index, ip6_address_t ip6_addr)
{
  ip6_address_t *addr;
  ip6_main_t *im6 = &ip6_main;
  ip_lookup_main_t *lm6 = &im6->lookup_main;
  ip_interface_address_t *ia = 0;

  /* *INDENT-OFF* */
  foreach_ip_interface_address (lm6, ia, sw_if_index, 0,
  ({
	addr = ip_interface_address_get_address (lm6, ia);
	if (0 == ip6_address_compare (addr, &ip6_addr))
		return 1;
  }));
  /* *INDENT-ON* */

  return 0;
}

/**
 * @brief Check whether is a hairpinning.
 *
 * If the destination IP address of the packet is an IPv4 address assigned to
 * the NAT64 itself, then the packet is a hairpin packet.
 *
 * param dst_addr Destination address of the packet.
 *
 * @returns 1 if hairpinning, otherwise 0.
 */
static_always_inline int
is_hairpinning (ip6_address_t * dst_addr)
{
  nat64_main_t *nm = &nat64_main;
  int i;

  for (i = 0; i < vec_len (nm->addr_pool); i++)
    {
      if (nm->addr_pool[i].addr.as_u32 == dst_addr->as_u32[3])
	return 1;
    }

  return 0;
}

static int
nat64_in2out_tcp_udp_set_cb (ip6_header_t * ip6, ip4_header_t * ip4,
			     void *arg)
{
  nat64_main_t *nm = &nat64_main;
  nat64_in2out_set_ctx_t *ctx = arg;
  nat64_db_bib_entry_t *bibe;
  nat64_db_st_entry_t *ste;
  ip46_address_t saddr, daddr;
  u32 sw_if_index, fib_index;
  udp_header_t *udp = ip6_next_header (ip6);
  u8 proto = ip6->protocol;
  u16 sport = udp->src_port;
  u16 dport = udp->dst_port;
  nat64_db_t *db = &nm->db[ctx->thread_index];

  sw_if_index = vnet_buffer (ctx->b)->sw_if_index[VLIB_RX];
  fib_index =
    fib_table_get_index_for_sw_if_index (FIB_PROTOCOL_IP6, sw_if_index);

  saddr.as_u64[0] = ip6->src_address.as_u64[0];
  saddr.as_u64[1] = ip6->src_address.as_u64[1];
  daddr.as_u64[0] = ip6->dst_address.as_u64[0];
  daddr.as_u64[1] = ip6->dst_address.as_u64[1];

  ste =
    nat64_db_st_entry_find (db, &saddr, &daddr, sport, dport, proto,
			    fib_index, 1);

  if (ste)
    {
      bibe = nat64_db_bib_entry_by_index (db, proto, ste->bibe_index);
      if (!bibe)
	return -1;
    }
  else
    {
      bibe = nat64_db_bib_entry_find (db, &saddr, sport, proto, fib_index, 1);

      if (!bibe)
	{
	  u16 out_port;
	  ip4_address_t out_addr;
	  if (nat64_alloc_out_addr_and_port
	      (fib_index, ip_proto_to_snat_proto (proto), &out_addr,
	       &out_port, ctx->thread_index))
	    return -1;

	  bibe =
	    nat64_db_bib_entry_create (ctx->thread_index, db,
				       &ip6->src_address, &out_addr, sport,
				       out_port, fib_index, proto, 0);
	  if (!bibe)
	    return -1;

	  vlib_set_simple_counter (&nm->total_bibs, ctx->thread_index, 0,
				   db->bib.bib_entries_num);
	}

      nat64_extract_ip4 (&ip6->dst_address, &daddr.ip4, fib_index);
      ste =
	nat64_db_st_entry_create (ctx->thread_index, db, bibe,
				  &ip6->dst_address, &daddr.ip4, dport);
      if (!ste)
	return -1;

      vlib_set_simple_counter (&nm->total_sessions, ctx->thread_index, 0,
			       db->st.st_entries_num);
    }

  ip4->src_address.as_u32 = bibe->out_addr.as_u32;
  udp->src_port = bibe->out_port;

  ip4->dst_address.as_u32 = ste->out_r_addr.as_u32;

  if (proto == IP_PROTOCOL_TCP)
    {
      u16 *checksum;
      ip_csum_t csum;
      tcp_header_t *tcp = ip6_next_header (ip6);

      nat64_tcp_session_set_state (ste, tcp, 1);
      checksum = &tcp->checksum;
      csum = ip_csum_sub_even (*checksum, sport);
      csum = ip_csum_add_even (csum, udp->src_port);
      mss_clamping (nm->sm, tcp, &csum);
      *checksum = ip_csum_fold (csum);
    }

  nat64_session_reset_timeout (ste, ctx->vm);

  return 0;
}

static int
nat64_in2out_icmp_set_cb (ip6_header_t * ip6, ip4_header_t * ip4, void *arg)
{
  nat64_main_t *nm = &nat64_main;
  nat64_in2out_set_ctx_t *ctx = arg;
  nat64_db_bib_entry_t *bibe;
  nat64_db_st_entry_t *ste;
  ip46_address_t saddr, daddr;
  u32 sw_if_index, fib_index;
  icmp46_header_t *icmp = ip6_next_header (ip6);
  nat64_db_t *db = &nm->db[ctx->thread_index];

  sw_if_index = vnet_buffer (ctx->b)->sw_if_index[VLIB_RX];
  fib_index =
    fib_table_get_index_for_sw_if_index (FIB_PROTOCOL_IP6, sw_if_index);

  saddr.as_u64[0] = ip6->src_address.as_u64[0];
  saddr.as_u64[1] = ip6->src_address.as_u64[1];
  daddr.as_u64[0] = ip6->dst_address.as_u64[0];
  daddr.as_u64[1] = ip6->dst_address.as_u64[1];

  if (icmp->type == ICMP4_echo_request || icmp->type == ICMP4_echo_reply)
    {
      u16 in_id = ((u16 *) (icmp))[2];
      ste =
	nat64_db_st_entry_find (db, &saddr, &daddr, in_id, 0,
				IP_PROTOCOL_ICMP, fib_index, 1);

      if (ste)
	{
	  bibe =
	    nat64_db_bib_entry_by_index (db, IP_PROTOCOL_ICMP,
					 ste->bibe_index);
	  if (!bibe)
	    return -1;
	}
      else
	{
	  bibe =
	    nat64_db_bib_entry_find (db, &saddr, in_id,
				     IP_PROTOCOL_ICMP, fib_index, 1);

	  if (!bibe)
	    {
	      u16 out_id;
	      ip4_address_t out_addr;
	      if (nat64_alloc_out_addr_and_port
		  (fib_index, SNAT_PROTOCOL_ICMP, &out_addr, &out_id,
		   ctx->thread_index))
		return -1;

	      bibe =
		nat64_db_bib_entry_create (ctx->thread_index, db,
					   &ip6->src_address, &out_addr,
					   in_id, out_id, fib_index,
					   IP_PROTOCOL_ICMP, 0);
	      if (!bibe)
		return -1;

	      vlib_set_simple_counter (&nm->total_bibs, ctx->thread_index, 0,
				       db->bib.bib_entries_num);
	    }

	  nat64_extract_ip4 (&ip6->dst_address, &daddr.ip4, fib_index);
	  ste =
	    nat64_db_st_entry_create (ctx->thread_index, db, bibe,
				      &ip6->dst_address, &daddr.ip4, 0);
	  if (!ste)
	    return -1;

	  vlib_set_simple_counter (&nm->total_sessions, ctx->thread_index, 0,
				   db->st.st_entries_num);
	}

      nat64_session_reset_timeout (ste, ctx->vm);

      ip4->src_address.as_u32 = bibe->out_addr.as_u32;
      ((u16 *) (icmp))[2] = bibe->out_port;

      ip4->dst_address.as_u32 = ste->out_r_addr.as_u32;
    }
  else
    {
      if (!vec_len (nm->addr_pool))
	return -1;

      ip4->src_address.as_u32 = nm->addr_pool[0].addr.as_u32;
      nat64_extract_ip4 (&ip6->dst_address, &ip4->dst_address, fib_index);
    }

  return 0;
}

static int
nat64_in2out_inner_icmp_set_cb (ip6_header_t * ip6, ip4_header_t * ip4,
				void *arg)
{
  nat64_main_t *nm = &nat64_main;
  nat64_in2out_set_ctx_t *ctx = arg;
  nat64_db_st_entry_t *ste;
  nat64_db_bib_entry_t *bibe;
  ip46_address_t saddr, daddr;
  u32 sw_if_index, fib_index;
  u8 proto = ip6->protocol;
  nat64_db_t *db = &nm->db[ctx->thread_index];

  sw_if_index = vnet_buffer (ctx->b)->sw_if_index[VLIB_RX];
  fib_index =
    fib_table_get_index_for_sw_if_index (FIB_PROTOCOL_IP6, sw_if_index);

  saddr.as_u64[0] = ip6->src_address.as_u64[0];
  saddr.as_u64[1] = ip6->src_address.as_u64[1];
  daddr.as_u64[0] = ip6->dst_address.as_u64[0];
  daddr.as_u64[1] = ip6->dst_address.as_u64[1];

  if (proto == IP_PROTOCOL_ICMP6)
    {
      icmp46_header_t *icmp = ip6_next_header (ip6);
      u16 in_id = ((u16 *) (icmp))[2];
      proto = IP_PROTOCOL_ICMP;

      if (!
	  (icmp->type == ICMP4_echo_request
	   || icmp->type == ICMP4_echo_reply))
	return -1;

      ste =
	nat64_db_st_entry_find (db, &daddr, &saddr, in_id, 0, proto,
				fib_index, 1);
      if (!ste)
	return -1;

      bibe = nat64_db_bib_entry_by_index (db, proto, ste->bibe_index);
      if (!bibe)
	return -1;

      ip4->dst_address.as_u32 = bibe->out_addr.as_u32;
      ((u16 *) (icmp))[2] = bibe->out_port;
      ip4->src_address.as_u32 = ste->out_r_addr.as_u32;
    }
  else
    {
      udp_header_t *udp = ip6_next_header (ip6);
      tcp_header_t *tcp = ip6_next_header (ip6);
      u16 *checksum;
      ip_csum_t csum;

      u16 sport = udp->src_port;
      u16 dport = udp->dst_port;

      ste =
	nat64_db_st_entry_find (db, &daddr, &saddr, dport, sport, proto,
				fib_index, 1);
      if (!ste)
	return -1;

      bibe = nat64_db_bib_entry_by_index (db, proto, ste->bibe_index);
      if (!bibe)
	return -1;

      ip4->dst_address.as_u32 = bibe->out_addr.as_u32;
      udp->dst_port = bibe->out_port;
      ip4->src_address.as_u32 = ste->out_r_addr.as_u32;

      if (proto == IP_PROTOCOL_TCP)
	checksum = &tcp->checksum;
      else
	checksum = &udp->checksum;
      csum = ip_csum_sub_even (*checksum, dport);
      csum = ip_csum_add_even (csum, udp->dst_port);
      *checksum = ip_csum_fold (csum);
    }

  return 0;
}

typedef struct unk_proto_st_walk_ctx_t_
{
  ip6_address_t src_addr;
  ip6_address_t dst_addr;
  ip4_address_t out_addr;
  u32 fib_index;
  u32 thread_index;
  u8 proto;
} unk_proto_st_walk_ctx_t;

static int
unk_proto_st_walk (nat64_db_st_entry_t * ste, void *arg)
{
  nat64_main_t *nm = &nat64_main;
  unk_proto_st_walk_ctx_t *ctx = arg;
  nat64_db_bib_entry_t *bibe;
  ip46_address_t saddr, daddr;
  nat64_db_t *db = &nm->db[ctx->thread_index];

  if (ip46_address_is_equal (&ste->in_r_addr, &ctx->dst_addr))
    {
      bibe = nat64_db_bib_entry_by_index (db, ste->proto, ste->bibe_index);
      if (!bibe)
	return -1;

      if (ip46_address_is_equal (&bibe->in_addr, &ctx->src_addr)
	  && bibe->fib_index == ctx->fib_index)
	{
	  clib_memset (&saddr, 0, sizeof (saddr));
	  saddr.ip4.as_u32 = bibe->out_addr.as_u32;
	  clib_memset (&daddr, 0, sizeof (daddr));
	  nat64_extract_ip4 (&ctx->dst_addr, &daddr.ip4, ctx->fib_index);

	  if (nat64_db_st_entry_find
	      (db, &daddr, &saddr, 0, 0, ctx->proto, ctx->fib_index, 0))
	    return -1;

	  ctx->out_addr.as_u32 = bibe->out_addr.as_u32;
	  return 1;
	}
    }

  return 0;
}

static int
nat64_in2out_unk_proto_set_cb (ip6_header_t * ip6, ip4_header_t * ip4,
			       void *arg)
{
  nat64_main_t *nm = &nat64_main;
  nat64_in2out_set_ctx_t *s_ctx = arg;
  nat64_db_bib_entry_t *bibe;
  nat64_db_st_entry_t *ste;
  ip46_address_t saddr, daddr, addr;
  u32 sw_if_index, fib_index;
  u8 proto = ip6->protocol;
  int i;
  nat64_db_t *db = &nm->db[s_ctx->thread_index];

  sw_if_index = vnet_buffer (s_ctx->b)->sw_if_index[VLIB_RX];
  fib_index =
    fib_table_get_index_for_sw_if_index (FIB_PROTOCOL_IP6, sw_if_index);

  saddr.as_u64[0] = ip6->src_address.as_u64[0];
  saddr.as_u64[1] = ip6->src_address.as_u64[1];
  daddr.as_u64[0] = ip6->dst_address.as_u64[0];
  daddr.as_u64[1] = ip6->dst_address.as_u64[1];

  ste =
    nat64_db_st_entry_find (db, &saddr, &daddr, 0, 0, proto, fib_index, 1);

  if (ste)
    {
      bibe = nat64_db_bib_entry_by_index (db, proto, ste->bibe_index);
      if (!bibe)
	return -1;
    }
  else
    {
      bibe = nat64_db_bib_entry_find (db, &saddr, 0, proto, fib_index, 1);

      if (!bibe)
	{
	  /* Choose same out address as for TCP/UDP session to same dst */
	  unk_proto_st_walk_ctx_t ctx = {
	    .src_addr.as_u64[0] = ip6->src_address.as_u64[0],
	    .src_addr.as_u64[1] = ip6->src_address.as_u64[1],
	    .dst_addr.as_u64[0] = ip6->dst_address.as_u64[0],
	    .dst_addr.as_u64[1] = ip6->dst_address.as_u64[1],
	    .out_addr.as_u32 = 0,
	    .fib_index = fib_index,
	    .proto = proto,
	    .thread_index = s_ctx->thread_index,
	  };

	  nat64_db_st_walk (db, IP_PROTOCOL_TCP, unk_proto_st_walk, &ctx);

	  if (!ctx.out_addr.as_u32)
	    nat64_db_st_walk (db, IP_PROTOCOL_UDP, unk_proto_st_walk, &ctx);

	  /* Verify if out address is not already in use for protocol */
	  clib_memset (&addr, 0, sizeof (addr));
	  addr.ip4.as_u32 = ctx.out_addr.as_u32;
	  if (nat64_db_bib_entry_find (db, &addr, 0, proto, 0, 0))
	    ctx.out_addr.as_u32 = 0;

	  if (!ctx.out_addr.as_u32)
	    {
	      for (i = 0; i < vec_len (nm->addr_pool); i++)
		{
		  addr.ip4.as_u32 = nm->addr_pool[i].addr.as_u32;
		  if (!nat64_db_bib_entry_find (db, &addr, 0, proto, 0, 0))
		    break;
		}
	    }

	  if (!ctx.out_addr.as_u32)
	    return -1;

	  bibe =
	    nat64_db_bib_entry_create (s_ctx->thread_index, db,
				       &ip6->src_address, &ctx.out_addr,
				       0, 0, fib_index, proto, 0);
	  if (!bibe)
	    return -1;

	  vlib_set_simple_counter (&nm->total_bibs, s_ctx->thread_index, 0,
				   db->bib.bib_entries_num);
	}

      nat64_extract_ip4 (&ip6->dst_address, &daddr.ip4, fib_index);
      ste =
	nat64_db_st_entry_create (s_ctx->thread_index, db, bibe,
				  &ip6->dst_address, &daddr.ip4, 0);
      if (!ste)
	return -1;

      vlib_set_simple_counter (&nm->total_sessions, s_ctx->thread_index, 0,
			       db->st.st_entries_num);
    }

  nat64_session_reset_timeout (ste, s_ctx->vm);

  ip4->src_address.as_u32 = bibe->out_addr.as_u32;
  ip4->dst_address.as_u32 = ste->out_r_addr.as_u32;

  return 0;
}



static int
nat64_in2out_tcp_udp_hairpinning (vlib_main_t * vm, vlib_buffer_t * b,
				  ip6_header_t * ip6, u32 thread_index)
{
  nat64_main_t *nm = &nat64_main;
  nat64_db_bib_entry_t *bibe;
  nat64_db_st_entry_t *ste;
  ip46_address_t saddr, daddr;
  u32 sw_if_index, fib_index;
  udp_header_t *udp = ip6_next_header (ip6);
  tcp_header_t *tcp = ip6_next_header (ip6);
  u8 proto = ip6->protocol;
  u16 sport = udp->src_port;
  u16 dport = udp->dst_port;
  u16 *checksum;
  ip_csum_t csum;
  nat64_db_t *db = &nm->db[thread_index];

  sw_if_index = vnet_buffer (b)->sw_if_index[VLIB_RX];
  fib_index =
    fib_table_get_index_for_sw_if_index (FIB_PROTOCOL_IP6, sw_if_index);

  saddr.as_u64[0] = ip6->src_address.as_u64[0];
  saddr.as_u64[1] = ip6->src_address.as_u64[1];
  daddr.as_u64[0] = ip6->dst_address.as_u64[0];
  daddr.as_u64[1] = ip6->dst_address.as_u64[1];

  if (proto == IP_PROTOCOL_UDP)
    checksum = &udp->checksum;
  else
    checksum = &tcp->checksum;

  csum = ip_csum_sub_even (*checksum, ip6->src_address.as_u64[0]);
  csum = ip_csum_sub_even (csum, ip6->src_address.as_u64[1]);
  csum = ip_csum_sub_even (csum, ip6->dst_address.as_u64[0]);
  csum = ip_csum_sub_even (csum, ip6->dst_address.as_u64[1]);
  csum = ip_csum_sub_even (csum, sport);
  csum = ip_csum_sub_even (csum, dport);

  ste =
    nat64_db_st_entry_find (db, &saddr, &daddr, sport, dport, proto,
			    fib_index, 1);

  if (ste)
    {
      bibe = nat64_db_bib_entry_by_index (db, proto, ste->bibe_index);
      if (!bibe)
	return -1;
    }
  else
    {
      bibe = nat64_db_bib_entry_find (db, &saddr, sport, proto, fib_index, 1);

      if (!bibe)
	{
	  u16 out_port;
	  ip4_address_t out_addr;
	  if (nat64_alloc_out_addr_and_port
	      (fib_index, ip_proto_to_snat_proto (proto), &out_addr,
	       &out_port, thread_index))
	    return -1;

	  bibe =
	    nat64_db_bib_entry_create (thread_index, db, &ip6->src_address,
				       &out_addr, sport, out_port, fib_index,
				       proto, 0);
	  if (!bibe)
	    return -1;

	  vlib_set_simple_counter (&nm->total_bibs, thread_index, 0,
				   db->bib.bib_entries_num);
	}

      nat64_extract_ip4 (&ip6->dst_address, &daddr.ip4, fib_index);
      ste =
	nat64_db_st_entry_create (thread_index, db, bibe, &ip6->dst_address,
				  &daddr.ip4, dport);
      if (!ste)
	return -1;

      vlib_set_simple_counter (&nm->total_sessions, thread_index, 0,
			       db->st.st_entries_num);
    }

  if (proto == IP_PROTOCOL_TCP)
    nat64_tcp_session_set_state (ste, tcp, 1);

  nat64_session_reset_timeout (ste, vm);

  sport = udp->src_port = bibe->out_port;
  nat64_compose_ip6 (&ip6->src_address, &bibe->out_addr, fib_index);

  clib_memset (&daddr, 0, sizeof (daddr));
  daddr.ip4.as_u32 = ste->out_r_addr.as_u32;

  bibe = 0;
  /* *INDENT-OFF* */
  vec_foreach (db, nm->db)
    {
      bibe = nat64_db_bib_entry_find (db, &daddr, dport, proto, 0, 0);

      if (bibe)
	break;
    }
  /* *INDENT-ON* */

  if (!bibe)
    return -1;

  ip6->dst_address.as_u64[0] = bibe->in_addr.as_u64[0];
  ip6->dst_address.as_u64[1] = bibe->in_addr.as_u64[1];
  udp->dst_port = bibe->in_port;

  csum = ip_csum_add_even (csum, ip6->src_address.as_u64[0]);
  csum = ip_csum_add_even (csum, ip6->src_address.as_u64[1]);
  csum = ip_csum_add_even (csum, ip6->dst_address.as_u64[0]);
  csum = ip_csum_add_even (csum, ip6->dst_address.as_u64[1]);
  csum = ip_csum_add_even (csum, udp->src_port);
  csum = ip_csum_add_even (csum, udp->dst_port);
  *checksum = ip_csum_fold (csum);

  return 0;
}

static int
nat64_in2out_icmp_hairpinning (vlib_main_t * vm, vlib_buffer_t * b,
			       ip6_header_t * ip6, u32 thread_index)
{
  nat64_main_t *nm = &nat64_main;
  nat64_db_bib_entry_t *bibe;
  nat64_db_st_entry_t *ste;
  icmp46_header_t *icmp = ip6_next_header (ip6);
  ip6_header_t *inner_ip6;
  ip46_address_t saddr, daddr;
  u32 sw_if_index, fib_index;
  u8 proto;
  udp_header_t *udp;
  tcp_header_t *tcp;
  u16 *checksum, sport, dport;
  ip_csum_t csum;
  nat64_db_t *db = &nm->db[thread_index];

  if (icmp->type == ICMP6_echo_request || icmp->type == ICMP6_echo_reply)
    return -1;

  inner_ip6 = (ip6_header_t *) u8_ptr_add (icmp, 8);

  proto = inner_ip6->protocol;

  if (proto == IP_PROTOCOL_ICMP6)
    return -1;

  sw_if_index = vnet_buffer (b)->sw_if_index[VLIB_RX];
  fib_index =
    fib_table_get_index_for_sw_if_index (FIB_PROTOCOL_IP6, sw_if_index);

  saddr.as_u64[0] = inner_ip6->src_address.as_u64[0];
  saddr.as_u64[1] = inner_ip6->src_address.as_u64[1];
  daddr.as_u64[0] = inner_ip6->dst_address.as_u64[0];
  daddr.as_u64[1] = inner_ip6->dst_address.as_u64[1];

  udp = ip6_next_header (inner_ip6);
  tcp = ip6_next_header (inner_ip6);

  sport = udp->src_port;
  dport = udp->dst_port;

  if (proto == IP_PROTOCOL_UDP)
    checksum = &udp->checksum;
  else
    checksum = &tcp->checksum;

  csum = ip_csum_sub_even (*checksum, inner_ip6->src_address.as_u64[0]);
  csum = ip_csum_sub_even (csum, inner_ip6->src_address.as_u64[1]);
  csum = ip_csum_sub_even (csum, inner_ip6->dst_address.as_u64[0]);
  csum = ip_csum_sub_even (csum, inner_ip6->dst_address.as_u64[1]);
  csum = ip_csum_sub_even (csum, sport);
  csum = ip_csum_sub_even (csum, dport);

  ste =
    nat64_db_st_entry_find (db, &daddr, &saddr, dport, sport, proto,
			    fib_index, 1);
  if (!ste)
    return -1;

  bibe = nat64_db_bib_entry_by_index (db, proto, ste->bibe_index);
  if (!bibe)
    return -1;

  dport = udp->dst_port = bibe->out_port;
  nat64_compose_ip6 (&inner_ip6->dst_address, &bibe->out_addr, fib_index);

  clib_memset (&saddr, 0, sizeof (saddr));
  clib_memset (&daddr, 0, sizeof (daddr));
  saddr.ip4.as_u32 = ste->out_r_addr.as_u32;
  daddr.ip4.as_u32 = bibe->out_addr.as_u32;

  ste = 0;
  /* *INDENT-OFF* */
  vec_foreach (db, nm->db)
    {
      ste = nat64_db_st_entry_find (db, &saddr, &daddr, sport, dport, proto,
                                    0, 0);

      if (ste)
        break;
    }
  /* *INDENT-ON* */

  if (!ste)
    return -1;

  bibe = nat64_db_bib_entry_by_index (db, proto, ste->bibe_index);
  if (!bibe)
    return -1;

  inner_ip6->src_address.as_u64[0] = bibe->in_addr.as_u64[0];
  inner_ip6->src_address.as_u64[1] = bibe->in_addr.as_u64[1];
  udp->src_port = bibe->in_port;

  csum = ip_csum_add_even (csum, inner_ip6->src_address.as_u64[0]);
  csum = ip_csum_add_even (csum, inner_ip6->src_address.as_u64[1]);
  csum = ip_csum_add_even (csum, inner_ip6->dst_address.as_u64[0]);
  csum = ip_csum_add_even (csum, inner_ip6->dst_address.as_u64[1]);
  csum = ip_csum_add_even (csum, udp->src_port);
  csum = ip_csum_add_even (csum, udp->dst_port);
  *checksum = ip_csum_fold (csum);

  if (!vec_len (nm->addr_pool))
    return -1;

  nat64_compose_ip6 (&ip6->src_address, &nm->addr_pool[0].addr, fib_index);
  ip6->dst_address.as_u64[0] = inner_ip6->src_address.as_u64[0];
  ip6->dst_address.as_u64[1] = inner_ip6->src_address.as_u64[1];

  icmp->checksum = 0;
  csum = ip_csum_with_carry (0, ip6->payload_length);
  csum = ip_csum_with_carry (csum, clib_host_to_net_u16 (ip6->protocol));
  csum = ip_csum_with_carry (csum, ip6->src_address.as_u64[0]);
  csum = ip_csum_with_carry (csum, ip6->src_address.as_u64[1]);
  csum = ip_csum_with_carry (csum, ip6->dst_address.as_u64[0]);
  csum = ip_csum_with_carry (csum, ip6->dst_address.as_u64[1]);
  csum =
    ip_incremental_checksum (csum, icmp,
			     clib_net_to_host_u16 (ip6->payload_length));
  icmp->checksum = ~ip_csum_fold (csum);

  return 0;
}

static int
nat64_in2out_unk_proto_hairpinning (vlib_main_t * vm, vlib_buffer_t * b,
				    ip6_header_t * ip6, u32 thread_index)
{
  nat64_main_t *nm = &nat64_main;
  nat64_db_bib_entry_t *bibe;
  nat64_db_st_entry_t *ste;
  ip46_address_t saddr, daddr, addr;
  u32 sw_if_index, fib_index;
  u8 proto = ip6->protocol;
  int i;
  nat64_db_t *db = &nm->db[thread_index];

  sw_if_index = vnet_buffer (b)->sw_if_index[VLIB_RX];
  fib_index =
    fib_table_get_index_for_sw_if_index (FIB_PROTOCOL_IP6, sw_if_index);

  saddr.as_u64[0] = ip6->src_address.as_u64[0];
  saddr.as_u64[1] = ip6->src_address.as_u64[1];
  daddr.as_u64[0] = ip6->dst_address.as_u64[0];
  daddr.as_u64[1] = ip6->dst_address.as_u64[1];

  ste =
    nat64_db_st_entry_find (db, &saddr, &daddr, 0, 0, proto, fib_index, 1);

  if (ste)
    {
      bibe = nat64_db_bib_entry_by_index (db, proto, ste->bibe_index);
      if (!bibe)
	return -1;
    }
  else
    {
      bibe = nat64_db_bib_entry_find (db, &saddr, 0, proto, fib_index, 1);

      if (!bibe)
	{
	  /* Choose same out address as for TCP/UDP session to same dst */
	  unk_proto_st_walk_ctx_t ctx = {
	    .src_addr.as_u64[0] = ip6->src_address.as_u64[0],
	    .src_addr.as_u64[1] = ip6->src_address.as_u64[1],
	    .dst_addr.as_u64[0] = ip6->dst_address.as_u64[0],
	    .dst_addr.as_u64[1] = ip6->dst_address.as_u64[1],
	    .out_addr.as_u32 = 0,
	    .fib_index = fib_index,
	    .proto = proto,
	    .thread_index = thread_index,
	  };

	  nat64_db_st_walk (db, IP_PROTOCOL_TCP, unk_proto_st_walk, &ctx);

	  if (!ctx.out_addr.as_u32)
	    nat64_db_st_walk (db, IP_PROTOCOL_UDP, unk_proto_st_walk, &ctx);

	  /* Verify if out address is not already in use for protocol */
	  clib_memset (&addr, 0, sizeof (addr));
	  addr.ip4.as_u32 = ctx.out_addr.as_u32;
	  if (nat64_db_bib_entry_find (db, &addr, 0, proto, 0, 0))
	    ctx.out_addr.as_u32 = 0;

	  if (!ctx.out_addr.as_u32)
	    {
	      for (i = 0; i < vec_len (nm->addr_pool); i++)
		{
		  addr.ip4.as_u32 = nm->addr_pool[i].addr.as_u32;
		  if (!nat64_db_bib_entry_find (db, &addr, 0, proto, 0, 0))
		    break;
		}
	    }

	  if (!ctx.out_addr.as_u32)
	    return -1;

	  bibe =
	    nat64_db_bib_entry_create (thread_index, db, &ip6->src_address,
				       &ctx.out_addr, 0, 0, fib_index, proto,
				       0);
	  if (!bibe)
	    return -1;

	  vlib_set_simple_counter (&nm->total_bibs, thread_index, 0,
				   db->bib.bib_entries_num);
	}

      nat64_extract_ip4 (&ip6->dst_address, &daddr.ip4, fib_index);
      ste =
	nat64_db_st_entry_create (thread_index, db, bibe, &ip6->dst_address,
				  &daddr.ip4, 0);
      if (!ste)
	return -1;

      vlib_set_simple_counter (&nm->total_sessions, thread_index, 0,
			       db->st.st_entries_num);
    }

  nat64_session_reset_timeout (ste, vm);

  nat64_compose_ip6 (&ip6->src_address, &bibe->out_addr, fib_index);

  clib_memset (&daddr, 0, sizeof (daddr));
  daddr.ip4.as_u32 = ste->out_r_addr.as_u32;

  bibe = 0;
  /* *INDENT-OFF* */
  vec_foreach (db, nm->db)
    {
      bibe = nat64_db_bib_entry_find (db, &daddr, 0, proto, 0, 0);

      if (bibe)
	break;
    }
  /* *INDENT-ON* */

  if (!bibe)
    return -1;

  ip6->dst_address.as_u64[0] = bibe->in_addr.as_u64[0];
  ip6->dst_address.as_u64[1] = bibe->in_addr.as_u64[1];

  return 0;
}

static inline uword
nat64_in2out_node_fn_inline (vlib_main_t * vm, vlib_node_runtime_t * node,
			     vlib_frame_t * frame, u8 is_slow_path)
{
  u32 n_left_from, *from, *to_next;
  nat64_in2out_next_t next_index;
  u32 pkts_processed = 0;
  u32 stats_node_index;
  u32 thread_index = vm->thread_index;
  nat64_main_t *nm = &nat64_main;

  u32 tcp_packets = 0, udp_packets = 0, icmp_packets = 0, other_packets =
    0, fragments = 0;

  stats_node_index =
    is_slow_path ? nm->in2out_slowpath_node_index : nm->in2out_node_index;

  from = vlib_frame_vector_args (frame);
  n_left_from = frame->n_vectors;
  next_index = node->cached_next_index;

  while (n_left_from > 0)
    {
      u32 n_left_to_next;

      vlib_get_next_frame (vm, node, next_index, to_next, n_left_to_next);

      while (n_left_from > 0 && n_left_to_next > 0)
	{
	  u32 bi0;
	  vlib_buffer_t *b0;
	  u32 next0;
	  ip6_header_t *ip60;
	  u16 l4_offset0, frag_offset0;
	  u8 l4_protocol0;
	  u32 proto0;
	  nat64_in2out_set_ctx_t ctx0;
	  u32 sw_if_index0;

	  /* speculatively enqueue b0 to the current next frame */
	  bi0 = from[0];
	  to_next[0] = bi0;
	  from += 1;
	  to_next += 1;
	  n_left_from -= 1;
	  n_left_to_next -= 1;

	  b0 = vlib_get_buffer (vm, bi0);
	  ip60 = vlib_buffer_get_current (b0);

	  ctx0.b = b0;
	  ctx0.vm = vm;
	  ctx0.thread_index = thread_index;

	  next0 = NAT64_IN2OUT_NEXT_IP4_LOOKUP;

	  if (PREDICT_FALSE
	      (ip6_parse
	       (ip60, b0->current_length, &l4_protocol0, &l4_offset0,
		&frag_offset0)))
	    {
	      next0 = NAT64_IN2OUT_NEXT_DROP;
	      b0->error = node->errors[NAT64_IN2OUT_ERROR_UNKNOWN];
	      goto trace0;
	    }

	  sw_if_index0 = vnet_buffer (b0)->sw_if_index[VLIB_RX];

	  if (nat64_not_translate (sw_if_index0, ip60->dst_address))
	    {
	      next0 = NAT64_IN2OUT_NEXT_IP6_LOOKUP;
	      goto trace0;
	    }

	  proto0 = ip_proto_to_snat_proto (l4_protocol0);

	  if (is_slow_path)
	    {
	      if (PREDICT_TRUE (proto0 == ~0))
		{
		  other_packets++;
		  if (is_hairpinning (&ip60->dst_address))
		    {
		      next0 = NAT64_IN2OUT_NEXT_IP6_LOOKUP;
		      if (nat64_in2out_unk_proto_hairpinning
			  (vm, b0, ip60, thread_index))
			{
			  next0 = NAT64_IN2OUT_NEXT_DROP;
			  b0->error =
			    node->errors[NAT64_IN2OUT_ERROR_NO_TRANSLATION];
			}
		      goto trace0;
		    }

		  if (ip6_to_ip4 (b0, nat64_in2out_unk_proto_set_cb, &ctx0))
		    {
		      next0 = NAT64_IN2OUT_NEXT_DROP;
		      b0->error =
			node->errors[NAT64_IN2OUT_ERROR_NO_TRANSLATION];
		      goto trace0;
		    }
		}
	      goto trace0;
	    }
	  else
	    {
	      if (PREDICT_FALSE (proto0 == ~0))
		{
		  next0 = NAT64_IN2OUT_NEXT_SLOWPATH;
		  goto trace0;
		}
	    }

	  if (PREDICT_FALSE
	      (ip60->protocol == IP_PROTOCOL_IPV6_FRAGMENTATION))
	    {
	      next0 = NAT64_IN2OUT_NEXT_REASS;
	      fragments++;
	      goto trace0;
	    }

	  if (proto0 == SNAT_PROTOCOL_ICMP)
	    {
	      icmp_packets++;
	      if (is_hairpinning (&ip60->dst_address))
		{
		  next0 = NAT64_IN2OUT_NEXT_IP6_LOOKUP;
		  if (nat64_in2out_icmp_hairpinning
		      (vm, b0, ip60, thread_index))
		    {
		      next0 = NAT64_IN2OUT_NEXT_DROP;
		      b0->error =
			node->errors[NAT64_IN2OUT_ERROR_NO_TRANSLATION];
		    }
		  goto trace0;
		}

	      if (icmp6_to_icmp
		  (b0, nat64_in2out_icmp_set_cb, &ctx0,
		   nat64_in2out_inner_icmp_set_cb, &ctx0))
		{
		  next0 = NAT64_IN2OUT_NEXT_DROP;
		  b0->error = node->errors[NAT64_IN2OUT_ERROR_NO_TRANSLATION];
		  goto trace0;
		}
	    }
	  else if (proto0 == SNAT_PROTOCOL_TCP || proto0 == SNAT_PROTOCOL_UDP)
	    {
	      if (proto0 == SNAT_PROTOCOL_TCP)
		tcp_packets++;
	      else
		udp_packets++;

	      if (is_hairpinning (&ip60->dst_address))
		{
		  next0 = NAT64_IN2OUT_NEXT_IP6_LOOKUP;
		  if (nat64_in2out_tcp_udp_hairpinning
		      (vm, b0, ip60, thread_index))
		    {
		      next0 = NAT64_IN2OUT_NEXT_DROP;
		      b0->error =
			node->errors[NAT64_IN2OUT_ERROR_NO_TRANSLATION];
		    }
		  goto trace0;
		}

	      if (ip6_to_ip4_tcp_udp
		  (b0, nat64_in2out_tcp_udp_set_cb, &ctx0, 0))
		{
		  next0 = NAT64_IN2OUT_NEXT_DROP;
		  b0->error = node->errors[NAT64_IN2OUT_ERROR_NO_TRANSLATION];
		  goto trace0;
		}
	    }

	trace0:
	  if (PREDICT_FALSE ((node->flags & VLIB_NODE_FLAG_TRACE)
			     && (b0->flags & VLIB_BUFFER_IS_TRACED)))
	    {
	      nat64_in2out_trace_t *t =
		vlib_add_trace (vm, node, b0, sizeof (*t));
	      t->sw_if_index = vnet_buffer (b0)->sw_if_index[VLIB_RX];
	      t->next_index = next0;
	      t->is_slow_path = is_slow_path;
	    }

	  pkts_processed += next0 == NAT64_IN2OUT_NEXT_IP4_LOOKUP;

	  /* verify speculative enqueue, maybe switch current next frame */
	  vlib_validate_buffer_enqueue_x1 (vm, node, next_index, to_next,
					   n_left_to_next, bi0, next0);
	}
      vlib_put_next_frame (vm, node, next_index, n_left_to_next);
    }
  vlib_node_increment_counter (vm, stats_node_index,
			       NAT64_IN2OUT_ERROR_IN2OUT_PACKETS,
			       pkts_processed);
  vlib_node_increment_counter (vm, stats_node_index,
			       NAT64_IN2OUT_ERROR_TCP_PACKETS, tcp_packets);
  vlib_node_increment_counter (vm, stats_node_index,
			       NAT64_IN2OUT_ERROR_UDP_PACKETS, tcp_packets);
  vlib_node_increment_counter (vm, stats_node_index,
			       NAT64_IN2OUT_ERROR_ICMP_PACKETS, icmp_packets);
  vlib_node_increment_counter (vm, stats_node_index,
			       NAT64_IN2OUT_ERROR_OTHER_PACKETS,
			       other_packets);
  vlib_node_increment_counter (vm, stats_node_index,
			       NAT64_IN2OUT_ERROR_FRAGMENTS, fragments);

  return frame->n_vectors;
}

VLIB_NODE_FN (nat64_in2out_node) (vlib_main_t * vm,
				  vlib_node_runtime_t * node,
				  vlib_frame_t * frame)
{
  return nat64_in2out_node_fn_inline (vm, node, frame, 0);
}

/* *INDENT-OFF* */
VLIB_REGISTER_NODE (nat64_in2out_node) = {
  .name = "nat64-in2out",
  .vector_size = sizeof (u32),
  .format_trace = format_nat64_in2out_trace,
  .type = VLIB_NODE_TYPE_INTERNAL,
  .n_errors = ARRAY_LEN (nat64_in2out_error_strings),
  .error_strings = nat64_in2out_error_strings,
  .n_next_nodes = NAT64_IN2OUT_N_NEXT,
  /* edit / add dispositions here */
  .next_nodes = {
    [NAT64_IN2OUT_NEXT_DROP] = "error-drop",
    [NAT64_IN2OUT_NEXT_IP4_LOOKUP] = "ip4-lookup",
    [NAT64_IN2OUT_NEXT_IP6_LOOKUP] = "ip6-lookup",
    [NAT64_IN2OUT_NEXT_SLOWPATH] = "nat64-in2out-slowpath",
    [NAT64_IN2OUT_NEXT_REASS] = "nat64-in2out-reass",
  },
};
/* *INDENT-ON* */

VLIB_NODE_FN (nat64_in2out_slowpath_node) (vlib_main_t * vm,
					   vlib_node_runtime_t * node,
					   vlib_frame_t * frame)
{
  return nat64_in2out_node_fn_inline (vm, node, frame, 1);
}

/* *INDENT-OFF* */
VLIB_REGISTER_NODE (nat64_in2out_slowpath_node) = {
  .name = "nat64-in2out-slowpath",
  .vector_size = sizeof (u32),
  .format_trace = format_nat64_in2out_trace,
  .type = VLIB_NODE_TYPE_INTERNAL,
  .n_errors = ARRAY_LEN (nat64_in2out_error_strings),
  .error_strings = nat64_in2out_error_strings,
  .n_next_nodes = NAT64_IN2OUT_N_NEXT,
  /* edit / add dispositions here */
  .next_nodes = {
    [NAT64_IN2OUT_NEXT_DROP] = "error-drop",
    [NAT64_IN2OUT_NEXT_IP4_LOOKUP] = "ip4-lookup",
    [NAT64_IN2OUT_NEXT_IP6_LOOKUP] = "ip6-lookup",
    [NAT64_IN2OUT_NEXT_SLOWPATH] = "nat64-in2out-slowpath",
    [NAT64_IN2OUT_NEXT_REASS] = "nat64-in2out-reass",
  },
};
/* *INDENT-ON* */

typedef struct nat64_in2out_frag_set_ctx_t_
{
  vlib_main_t *vm;
  u32 sess_index;
  u32 thread_index;
  u16 l4_offset;
  u8 proto;
  u8 first_frag;
} nat64_in2out_frag_set_ctx_t;

static int
nat64_in2out_frag_set_cb (ip6_header_t * ip6, ip4_header_t * ip4, void *arg)
{
  nat64_main_t *nm = &nat64_main;
  nat64_in2out_frag_set_ctx_t *ctx = arg;
  nat64_db_st_entry_t *ste;
  nat64_db_bib_entry_t *bibe;
  udp_header_t *udp;
  nat64_db_t *db = &nm->db[ctx->thread_index];

  ste = nat64_db_st_entry_by_index (db, ctx->proto, ctx->sess_index);
  if (!ste)
    return -1;

  bibe = nat64_db_bib_entry_by_index (db, ctx->proto, ste->bibe_index);
  if (!bibe)
    return -1;

  nat64_session_reset_timeout (ste, ctx->vm);

  if (ctx->first_frag)
    {
      udp = (udp_header_t *) u8_ptr_add (ip6, ctx->l4_offset);

      if (ctx->proto == IP_PROTOCOL_TCP)
	{
	  u16 *checksum;
	  ip_csum_t csum;
	  tcp_header_t *tcp = (tcp_header_t *) udp;

	  nat64_tcp_session_set_state (ste, tcp, 1);
	  checksum = &tcp->checksum;
	  csum = ip_csum_sub_even (*checksum, tcp->src_port);
	  csum = ip_csum_sub_even (csum, ip6->src_address.as_u64[0]);
	  csum = ip_csum_sub_even (csum, ip6->src_address.as_u64[1]);
	  csum = ip_csum_sub_even (csum, ip6->dst_address.as_u64[0]);
	  csum = ip_csum_sub_even (csum, ip6->dst_address.as_u64[1]);
	  csum = ip_csum_add_even (csum, bibe->out_port);
	  csum = ip_csum_add_even (csum, bibe->out_addr.as_u32);
	  csum = ip_csum_add_even (csum, ste->out_r_addr.as_u32);
	  *checksum = ip_csum_fold (csum);
	}

      udp->src_port = bibe->out_port;
    }

  ip4->src_address.as_u32 = bibe->out_addr.as_u32;
  ip4->dst_address.as_u32 = ste->out_r_addr.as_u32;

  return 0;
}

static int
nat64_in2out_frag_hairpinning (vlib_buffer_t * b, ip6_header_t * ip6,
			       nat64_in2out_frag_set_ctx_t * ctx)
{
  nat64_main_t *nm = &nat64_main;
  nat64_db_st_entry_t *ste;
  nat64_db_bib_entry_t *bibe;
  udp_header_t *udp = (udp_header_t *) u8_ptr_add (ip6, ctx->l4_offset);
  tcp_header_t *tcp = (tcp_header_t *) udp;
  u16 sport = udp->src_port;
  u16 dport = udp->dst_port;
  u16 *checksum;
  ip_csum_t csum;
  ip46_address_t daddr;
  nat64_db_t *db = &nm->db[ctx->thread_index];

  if (ctx->first_frag)
    {
      if (ctx->proto == IP_PROTOCOL_UDP)
	checksum = &udp->checksum;
      else
	checksum = &tcp->checksum;

      csum = ip_csum_sub_even (*checksum, ip6->src_address.as_u64[0]);
      csum = ip_csum_sub_even (csum, ip6->src_address.as_u64[1]);
      csum = ip_csum_sub_even (csum, ip6->dst_address.as_u64[0]);
      csum = ip_csum_sub_even (csum, ip6->dst_address.as_u64[1]);
      csum = ip_csum_sub_even (csum, sport);
      csum = ip_csum_sub_even (csum, dport);
    }

  ste = nat64_db_st_entry_by_index (db, ctx->proto, ctx->sess_index);
  if (!ste)
    return -1;

  bibe = nat64_db_bib_entry_by_index (db, ctx->proto, ste->bibe_index);
  if (!bibe)
    return -1;

  if (ctx->proto == IP_PROTOCOL_TCP)
    nat64_tcp_session_set_state (ste, tcp, 1);

  nat64_session_reset_timeout (ste, ctx->vm);

  sport = bibe->out_port;
  dport = ste->r_port;

  nat64_compose_ip6 (&ip6->src_address, &bibe->out_addr, bibe->fib_index);

  clib_memset (&daddr, 0, sizeof (daddr));
  daddr.ip4.as_u32 = ste->out_r_addr.as_u32;

  bibe = 0;
  /* *INDENT-OFF* */
  vec_foreach (db, nm->db)
    {
      bibe = nat64_db_bib_entry_find (db, &daddr, dport, ctx->proto, 0, 0);

      if (bibe)
	break;
    }
  /* *INDENT-ON* */

  if (!bibe)
    return -1;

  ip6->dst_address.as_u64[0] = bibe->in_addr.as_u64[0];
  ip6->dst_address.as_u64[1] = bibe->in_addr.as_u64[1];

  if (ctx->first_frag)
    {
      udp->dst_port = bibe->in_port;
      udp->src_port = sport;
      csum = ip_csum_add_even (csum, ip6->src_address.as_u64[0]);
      csum = ip_csum_add_even (csum, ip6->src_address.as_u64[1]);
      csum = ip_csum_add_even (csum, ip6->dst_address.as_u64[0]);
      csum = ip_csum_add_even (csum, ip6->dst_address.as_u64[1]);
      csum = ip_csum_add_even (csum, udp->src_port);
      csum = ip_csum_add_even (csum, udp->dst_port);
      *checksum = ip_csum_fold (csum);
    }

  return 0;
}

VLIB_NODE_FN (nat64_in2out_reass_node) (vlib_main_t * vm,
					vlib_node_runtime_t * node,
					vlib_frame_t * frame)
{
  u32 n_left_from, *from, *to_next;
  nat64_in2out_next_t next_index;
  u32 pkts_processed = 0, cached_fragments = 0;
  u32 *fragments_to_drop = 0;
  u32 *fragments_to_loopback = 0;
  nat64_main_t *nm = &nat64_main;
  u32 thread_index = vm->thread_index;

  from = vlib_frame_vector_args (frame);
  n_left_from = frame->n_vectors;
  next_index = node->cached_next_index;

  while (n_left_from > 0)
    {
      u32 n_left_to_next;

      vlib_get_next_frame (vm, node, next_index, to_next, n_left_to_next);

      while (n_left_from > 0 && n_left_to_next > 0)
	{
	  u32 bi0;
	  vlib_buffer_t *b0;
	  u32 next0;
	  u8 cached0 = 0;
	  ip6_header_t *ip60;
	  u16 l4_offset0, frag_offset0;
	  u8 l4_protocol0;
	  nat_reass_ip6_t *reass0;
	  ip6_frag_hdr_t *frag0;
	  nat64_db_bib_entry_t *bibe0;
	  nat64_db_st_entry_t *ste0;
	  udp_header_t *udp0;
	  snat_protocol_t proto0;
	  u32 sw_if_index0, fib_index0;
	  ip46_address_t saddr0, daddr0;
	  nat64_in2out_frag_set_ctx_t ctx0;
	  nat64_db_t *db = &nm->db[thread_index];

	  /* speculatively enqueue b0 to the current next frame */
	  bi0 = from[0];
	  to_next[0] = bi0;
	  from += 1;
	  to_next += 1;
	  n_left_from -= 1;
	  n_left_to_next -= 1;

	  b0 = vlib_get_buffer (vm, bi0);
	  next0 = NAT64_IN2OUT_NEXT_IP4_LOOKUP;

	  sw_if_index0 = vnet_buffer (b0)->sw_if_index[VLIB_RX];
	  fib_index0 =
	    fib_table_get_index_for_sw_if_index (FIB_PROTOCOL_IP6,
						 sw_if_index0);

	  ctx0.thread_index = thread_index;

	  if (PREDICT_FALSE (nat_reass_is_drop_frag (1)))
	    {
	      next0 = NAT64_IN2OUT_NEXT_DROP;
	      b0->error = node->errors[NAT64_IN2OUT_ERROR_DROP_FRAGMENT];
	      goto trace0;
	    }

	  ip60 = (ip6_header_t *) vlib_buffer_get_current (b0);

	  if (PREDICT_FALSE
	      (ip6_parse
	       (ip60, b0->current_length, &l4_protocol0, &l4_offset0,
		&frag_offset0)))
	    {
	      next0 = NAT64_IN2OUT_NEXT_DROP;
	      b0->error = node->errors[NAT64_IN2OUT_ERROR_UNKNOWN];
	      goto trace0;
	    }

	  if (PREDICT_FALSE
	      (!(l4_protocol0 == IP_PROTOCOL_TCP
		 || l4_protocol0 == IP_PROTOCOL_UDP)))
	    {
	      next0 = NAT64_IN2OUT_NEXT_DROP;
	      b0->error = node->errors[NAT64_IN2OUT_ERROR_DROP_FRAGMENT];
	      goto trace0;
	    }

	  udp0 = (udp_header_t *) u8_ptr_add (ip60, l4_offset0);
	  frag0 = (ip6_frag_hdr_t *) u8_ptr_add (ip60, frag_offset0);
	  proto0 = ip_proto_to_snat_proto (l4_protocol0);

	  reass0 = nat_ip6_reass_find_or_create (ip60->src_address,
						 ip60->dst_address,
						 frag0->identification,
						 l4_protocol0,
						 1, &fragments_to_drop);

	  if (PREDICT_FALSE (!reass0))
	    {
	      next0 = NAT64_IN2OUT_NEXT_DROP;
	      b0->error = node->errors[NAT64_IN2OUT_ERROR_MAX_REASS];
	      goto trace0;
	    }

	  if (PREDICT_TRUE (ip6_frag_hdr_offset (frag0)))
	    {
	      ctx0.first_frag = 0;
	      if (PREDICT_FALSE (reass0->sess_index == (u32) ~ 0))
		{
		  if (nat_ip6_reass_add_fragment
		      (thread_index, reass0, bi0, &fragments_to_drop))
		    {
		      b0->error = node->errors[NAT64_IN2OUT_ERROR_MAX_FRAG];
		      next0 = NAT64_IN2OUT_NEXT_DROP;
		      goto trace0;
		    }
		  cached0 = 1;
		  goto trace0;
		}
	    }
	  else
	    {
	      ctx0.first_frag = 1;

	      saddr0.as_u64[0] = ip60->src_address.as_u64[0];
	      saddr0.as_u64[1] = ip60->src_address.as_u64[1];
	      daddr0.as_u64[0] = ip60->dst_address.as_u64[0];
	      daddr0.as_u64[1] = ip60->dst_address.as_u64[1];

	      ste0 =
		nat64_db_st_entry_find (db, &saddr0, &daddr0,
					udp0->src_port, udp0->dst_port,
					l4_protocol0, fib_index0, 1);
	      if (!ste0)
		{
		  bibe0 =
		    nat64_db_bib_entry_find (db, &saddr0, udp0->src_port,
					     l4_protocol0, fib_index0, 1);
		  if (!bibe0)
		    {
		      u16 out_port0;
		      ip4_address_t out_addr0;
		      if (nat64_alloc_out_addr_and_port
			  (fib_index0, proto0, &out_addr0, &out_port0,
			   thread_index))
			{
			  next0 = NAT64_IN2OUT_NEXT_DROP;
			  b0->error =
			    node->errors[NAT64_IN2OUT_ERROR_NO_TRANSLATION];
			  goto trace0;
			}

		      bibe0 =
			nat64_db_bib_entry_create (thread_index, db,
						   &ip60->src_address,
						   &out_addr0, udp0->src_port,
						   out_port0, fib_index0,
						   l4_protocol0, 0);
		      if (!bibe0)
			{
			  next0 = NAT64_IN2OUT_NEXT_DROP;
			  b0->error =
			    node->errors[NAT64_IN2OUT_ERROR_NO_TRANSLATION];
			  goto trace0;
			}
		      vlib_set_simple_counter (&nm->total_bibs, thread_index,
					       0, db->bib.bib_entries_num);
		    }
		  nat64_extract_ip4 (&ip60->dst_address, &daddr0.ip4,
				     fib_index0);
		  ste0 =
		    nat64_db_st_entry_create (thread_index, db, bibe0,
					      &ip60->dst_address, &daddr0.ip4,
					      udp0->dst_port);
		  if (!ste0)
		    {
		      next0 = NAT64_IN2OUT_NEXT_DROP;
		      b0->error =
			node->errors[NAT64_IN2OUT_ERROR_NO_TRANSLATION];
		      goto trace0;
		    }

		  vlib_set_simple_counter (&nm->total_sessions, thread_index,
					   0, db->st.st_entries_num);
		}
	      reass0->sess_index = nat64_db_st_entry_get_index (db, ste0);

	      nat_ip6_reass_get_frags (reass0, &fragments_to_loopback);
	    }

	  ctx0.sess_index = reass0->sess_index;
	  ctx0.proto = l4_protocol0;
	  ctx0.vm = vm;
	  ctx0.l4_offset = l4_offset0;

	  if (PREDICT_FALSE (is_hairpinning (&ip60->dst_address)))
	    {
	      next0 = NAT64_IN2OUT_NEXT_IP6_LOOKUP;
	      if (nat64_in2out_frag_hairpinning (b0, ip60, &ctx0))
		{
		  next0 = NAT64_IN2OUT_NEXT_DROP;
		  b0->error = node->errors[NAT64_IN2OUT_ERROR_NO_TRANSLATION];
		}
	      goto trace0;
	    }
	  else
	    {
	      if (ip6_to_ip4_fragmented (b0, nat64_in2out_frag_set_cb, &ctx0))
		{
		  next0 = NAT64_IN2OUT_NEXT_DROP;
		  b0->error = node->errors[NAT64_IN2OUT_ERROR_UNKNOWN];
		  goto trace0;
		}
	    }

	trace0:
	  if (PREDICT_FALSE
	      ((node->flags & VLIB_NODE_FLAG_TRACE)
	       && (b0->flags & VLIB_BUFFER_IS_TRACED)))
	    {
	      nat64_in2out_reass_trace_t *t =
		vlib_add_trace (vm, node, b0, sizeof (*t));
	      t->cached = cached0;
	      t->sw_if_index = sw_if_index0;
	      t->next_index = next0;
	    }

	  if (cached0)
	    {
	      n_left_to_next++;
	      to_next--;
	      cached_fragments++;
	    }
	  else
	    {
	      pkts_processed += next0 != NAT64_IN2OUT_NEXT_DROP;

	      /* verify speculative enqueue, maybe switch current next frame */
	      vlib_validate_buffer_enqueue_x1 (vm, node, next_index,
					       to_next, n_left_to_next,
					       bi0, next0);
	    }

	  if (n_left_from == 0 && vec_len (fragments_to_loopback))
	    {
	      from = vlib_frame_vector_args (frame);
	      u32 len = vec_len (fragments_to_loopback);
	      if (len <= VLIB_FRAME_SIZE)
		{
		  clib_memcpy_fast (from, fragments_to_loopback,
				    sizeof (u32) * len);
		  n_left_from = len;
		  vec_reset_length (fragments_to_loopback);
		}
	      else
		{
		  clib_memcpy_fast (from, fragments_to_loopback +
				    (len - VLIB_FRAME_SIZE),
				    sizeof (u32) * VLIB_FRAME_SIZE);
		  n_left_from = VLIB_FRAME_SIZE;
		  _vec_len (fragments_to_loopback) = len - VLIB_FRAME_SIZE;
		}
	    }
	}

      vlib_put_next_frame (vm, node, next_index, n_left_to_next);
    }

  vlib_node_increment_counter (vm, nm->in2out_reass_node_index,
			       NAT64_IN2OUT_ERROR_PROCESSED_FRAGMENTS,
			       pkts_processed);
  vlib_node_increment_counter (vm, nm->in2out_reass_node_index,
			       NAT64_IN2OUT_ERROR_CACHED_FRAGMENTS,
			       cached_fragments);

  nat_send_all_to_node (vm, fragments_to_drop, node,
			&node->errors[NAT64_IN2OUT_ERROR_DROP_FRAGMENT],
			NAT64_IN2OUT_NEXT_DROP);

  vec_free (fragments_to_drop);
  vec_free (fragments_to_loopback);
  return frame->n_vectors;
}

/* *INDENT-OFF* */
VLIB_REGISTER_NODE (nat64_in2out_reass_node) = {
  .name = "nat64-in2out-reass",
  .vector_size = sizeof (u32),
  .format_trace = format_nat64_in2out_reass_trace,
  .type = VLIB_NODE_TYPE_INTERNAL,
  .n_errors = ARRAY_LEN (nat64_in2out_error_strings),
  .error_strings = nat64_in2out_error_strings,
  .n_next_nodes = NAT64_IN2OUT_N_NEXT,
  /* edit / add dispositions here */
  .next_nodes = {
    [NAT64_IN2OUT_NEXT_DROP] = "error-drop",
    [NAT64_IN2OUT_NEXT_IP4_LOOKUP] = "ip4-lookup",
    [NAT64_IN2OUT_NEXT_IP6_LOOKUP] = "ip6-lookup",
    [NAT64_IN2OUT_NEXT_SLOWPATH] = "nat64-in2out-slowpath",
    [NAT64_IN2OUT_NEXT_REASS] = "nat64-in2out-reass",
  },
};
/* *INDENT-ON* */

#define foreach_nat64_in2out_handoff_error                       \
_(CONGESTION_DROP, "congestion drop")                            \
_(SAME_WORKER, "same worker")                                    \
_(DO_HANDOFF, "do handoff")

typedef enum
{
#define _(sym,str) NAT64_IN2OUT_HANDOFF_ERROR_##sym,
  foreach_nat64_in2out_handoff_error
#undef _
    NAT64_IN2OUT_HANDOFF_N_ERROR,
} nat64_in2out_handoff_error_t;

static char *nat64_in2out_handoff_error_strings[] = {
#define _(sym,string) string,
  foreach_nat64_in2out_handoff_error
#undef _
};

typedef struct
{
  u32 next_worker_index;
} nat64_in2out_handoff_trace_t;

static u8 *
format_nat64_in2out_handoff_trace (u8 * s, va_list * args)
{
  CLIB_UNUSED (vlib_main_t * vm) = va_arg (*args, vlib_main_t *);
  CLIB_UNUSED (vlib_node_t * node) = va_arg (*args, vlib_node_t *);
  nat64_in2out_handoff_trace_t *t =
    va_arg (*args, nat64_in2out_handoff_trace_t *);

  s =
    format (s, "NAT64-IN2OUT-HANDOFF: next-worker %d", t->next_worker_index);

  return s;
}

VLIB_NODE_FN (nat64_in2out_handoff_node) (vlib_main_t * vm,
					  vlib_node_runtime_t * node,
					  vlib_frame_t * frame)
{
  nat64_main_t *nm = &nat64_main;
  vlib_buffer_t *bufs[VLIB_FRAME_SIZE], **b;
  u32 n_enq, n_left_from, *from;
  u16 thread_indices[VLIB_FRAME_SIZE], *ti;
  u32 fq_index;
  u32 thread_index = vm->thread_index;
  u32 do_handoff = 0, same_worker = 0;

  from = vlib_frame_vector_args (frame);
  n_left_from = frame->n_vectors;
  vlib_get_buffers (vm, from, bufs, n_left_from);

  b = bufs;
  ti = thread_indices;

  fq_index = nm->fq_in2out_index;

  while (n_left_from > 0)
    {
      ip6_header_t *ip0;

      ip0 = vlib_buffer_get_current (b[0]);
      ti[0] = nat64_get_worker_in2out (&ip0->src_address);

      if (ti[0] != thread_index)
	do_handoff++;
      else
	same_worker++;

      if (PREDICT_FALSE
	  ((node->flags & VLIB_NODE_FLAG_TRACE)
	   && (b[0]->flags & VLIB_BUFFER_IS_TRACED)))
	{
	  nat64_in2out_handoff_trace_t *t =
	    vlib_add_trace (vm, node, b[0], sizeof (*t));
	  t->next_worker_index = ti[0];
	}

      n_left_from -= 1;
      ti += 1;
      b += 1;
    }

  n_enq =
    vlib_buffer_enqueue_to_thread (vm, fq_index, from, thread_indices,
				   frame->n_vectors, 1);

  if (n_enq < frame->n_vectors)
    vlib_node_increment_counter (vm, node->node_index,
				 NAT64_IN2OUT_HANDOFF_ERROR_CONGESTION_DROP,
				 frame->n_vectors - n_enq);
  vlib_node_increment_counter (vm, node->node_index,
			       NAT64_IN2OUT_HANDOFF_ERROR_SAME_WORKER,
			       same_worker);
  vlib_node_increment_counter (vm, node->node_index,
			       NAT64_IN2OUT_HANDOFF_ERROR_DO_HANDOFF,
			       do_handoff);

  return frame->n_vectors;
}

/* *INDENT-OFF* */
VLIB_REGISTER_NODE (nat64_in2out_handoff_node) = {
  .name = "nat64-in2out-handoff",
  .vector_size = sizeof (u32),
  .format_trace = format_nat64_in2out_handoff_trace,
  .type = VLIB_NODE_TYPE_INTERNAL,
  .n_errors = ARRAY_LEN(nat64_in2out_handoff_error_strings),
  .error_strings = nat64_in2out_handoff_error_strings,

  .n_next_nodes = 1,

  .next_nodes = {
    [0] = "error-drop",
  },
};
/* *INDENT-ON* */

/*
 * fd.io coding-style-patch-verification: ON
 *
 * Local Variables:
 * eval: (c-set-style "gnu")
 * End:
 */
