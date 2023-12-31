#include <linux/in.h>
#include <linux/bpf.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_endian.h>

#include <stddef.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/tcp.h>

#define max(a,b)             \
({                           \
    __typeof__ (a) _a = (a); \
    __typeof__ (b) _b = (b); \
    _a > _b ? _a : _b;       \
})

/* Allow users of header file to redefine VLAN max depth */
#ifndef VLAN_MAX_DEPTH
#define VLAN_MAX_DEPTH 4
#endif

#ifndef MAX_CLIENTS
#define MAX_CLIENTS 4
#endif

#ifndef MAX_SERVERS
#define MAX_SERVERS 3
#endif

#ifndef MAX_PER_SERVER
#define MAX_PER_SERVER 3
#endif

#define LB_LISTENER_PORT 8080
#define MIN_SERVER_PORT 4171
#define MAX_SERVER_PORT (4170+MAX_SERVERS) 

struct eth_addr {
	__u8 addr[ETH_ALEN];
};

struct eth_conn {
	struct eth_addr src;
	struct eth_addr dst;
};

struct connection {
	__u32 src_port;
	__u32 dst_port;
	__u32 src_ip;
	__u32 dst_ip;
};

struct reroute {
	struct connection original_conn; 
	struct eth_conn original_eth; 
	__u32 original_index; 
    __s32 seq_offset; 
	__s32 ack_offset;
 	__u32 rematch_flag;
    struct connection new_conn;
	struct eth_conn new_eth;
	__u32 new_index;
};

struct numbers {
	__u32 seq_no;
	__u32 ack_no;
	__u32 init_seq;
	__u32 init_ack;
	struct eth_conn cur_eth;
};

struct server {
	__u32 port;
	__u32 ip;
};

struct availability {
	struct connection conns[MAX_PER_SERVER];
	__u32 valid[MAX_PER_SERVER];
};

// /* Define maps */
struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, 2*MAX_CLIENTS);
	__type(key, struct connection);
	__type(value, struct reroute);
	__uint(pinning, LIBBPF_PIN_BY_NAME);
} conn_map SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, MAX_CLIENTS + (MAX_SERVERS*MAX_PER_SERVER));
	__type(key, struct connection);
	__type(value, struct numbers);
	__uint(pinning, LIBBPF_PIN_BY_NAME);
} numbers_map SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, MAX_SERVERS);
	__type(key, struct server);
	__type(value, struct availability);
	__uint(pinning, LIBBPF_PIN_BY_NAME);
} available_map SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, MAX_CLIENTS);
	__type(key, struct server);
	__type(value, __u32);
	__uint(pinning, LIBBPF_PIN_BY_NAME);
} state_map SEC(".maps");

struct hdr_cursor {
	void *pos;
};

/*
 *	struct vlan_hdr - vlan header
 *	@h_vlan_TCI: priority and VLAN ID
 *	@h_vlan_encapsulated_proto: packet type ID or len
 */
struct vlan_hdr {
	__be16	h_vlan_TCI;
	__be16	h_vlan_encapsulated_proto;
};


static __always_inline int proto_is_vlan(__u16 h_proto)
{
	return !!(h_proto == bpf_htons(ETH_P_8021Q) ||
		  h_proto == bpf_htons(ETH_P_8021AD));
}

/* Notice, parse_ethhdr() will skip VLAN tags, by advancing nh->pos and returns
 * next header EtherType, BUT the ethhdr pointer supplied still points to the
 * Ethernet header. Thus, caller can look at eth->h_proto to see if this was a
 * VLAN tagged packet.
 */
static __always_inline int parse_ethhdr(struct hdr_cursor *nh, void *data_end,
					struct ethhdr **ethhdr)
{
	struct ethhdr *eth = nh->pos;
	struct vlan_hdr *vlh;
	__u16 h_proto;
	int i;

	if (eth + 1 > data_end)
		return -1;

	nh->pos = eth + 1;
	*ethhdr = eth;
	vlh = nh->pos;
	h_proto = eth->h_proto;

	/* Use loop unrolling to avoid the verifier restriction on loops;
	 * support up to VLAN_MAX_DEPTH layers of VLAN encapsulation.
	 */
	#pragma unroll
	for (i = 0; i < VLAN_MAX_DEPTH; i++) {
		if (!proto_is_vlan(h_proto))
			break;

		if (vlh + 1 > data_end)
			break;

		h_proto = vlh->h_vlan_encapsulated_proto;
		vlh++;
	}

	nh->pos = vlh;
	return h_proto; /* network-byte-order */
}

static __always_inline int parse_iphdr(struct hdr_cursor *nh,
				       void *data_end,
				       struct iphdr **iphdr)
{
	struct iphdr *iph = nh->pos;
	int hdrsize;

	if (iph + 1 > data_end)
		return -1;

	hdrsize = iph->ihl * 4;

	/* Variable-length IPv4 header, need to use byte-based arithmetic */
	if (nh->pos + hdrsize > data_end)
		return -1;

	nh->pos += hdrsize;
	*iphdr = iph;

	return iph->protocol;
}

/*
 * parse_tcphdr: parse and return the length of the tcp header
 */
static __always_inline int parse_tcphdr(struct hdr_cursor *nh,
					void *data_end,
					struct tcphdr **tcphdr)
{
	int len;
	struct tcphdr *h = nh->pos;

	if (h + 1 > data_end)
		return -1;

	len = h->doff * 4;
	if ((void *) h + len > data_end)
		return -1;

	nh->pos  = h + 1;
	*tcphdr = h;

	return len;
}

static __always_inline __u16 csum_reduce_helper(__u32 csum)
{
	csum = ((csum & 0xffff0000) >> 16) + (csum & 0xffff);
	csum = ((csum & 0xffff0000) >> 16) + (csum & 0xffff);
	return csum;
}

static inline unsigned short generic_checksum(unsigned short *buf, void *data_end, unsigned long sum, int max) 
{
	int flag = 0;
    for (int i = 0; i < max; i += 2) {
		if ((void *)(buf + 1) > data_end)
			flag = 1;
	    	break;
        sum += *buf;
        buf++;
    }
	if (!flag) {
		if((void *)buf +1 <= data_end) {
			sum +=  bpf_htons((*((unsigned char *)buf)) << 8);
    	}
	}
    sum = (sum & 0xffff) + (sum >> 16);
    sum = (sum & 0xffff) + (sum >> 16);
    return ~sum;
}

static inline __u16 l4_checksum(struct iphdr *iph, void *l4, void *data_end)
{
    __u32 csum = 0;
    csum += *(((__u16 *) &(iph->saddr))+0); // 1st 2 bytes
    csum += *(((__u16 *) &(iph->saddr))+1); // 2nd 2 bytes
    csum += *(((__u16 *) &(iph->daddr))+0); // 1st 2 bytes
    csum += *(((__u16 *) &(iph->daddr))+1); // 2nd 2 bytes
    csum += bpf_htons((__u16)iph->protocol); // protocol is a u8
    csum += bpf_htons((__u16)(data_end - (void *)l4)); 
    return generic_checksum((unsigned short *) l4, data_end, csum, 1480);
}

static inline void perform_checksums(struct tcphdr *tcph, struct iphdr *iph, void *data_end)
{
	iph->check = 0;
	iph->check = ~csum_reduce_helper(bpf_csum_diff(0, 0, (__be32 *)iph, sizeof(struct iphdr), 0));
	tcph->check = 0;
	tcph->check = l4_checksum(iph, tcph, data_end);
}


static inline struct connection create_conn_struct(struct tcphdr **tcph, struct iphdr **iph)
{
	struct connection conn;
	conn.src_port = bpf_ntohs((*tcph)->source);
	conn.dst_port = bpf_ntohs((*tcph)->dest);
	//bpf_printk("CONN - src port: %u, dst port: %u", conn.src_port, conn.dst_port);
	conn.src_ip = bpf_ntohl((*iph)->saddr);
	conn.dst_ip = bpf_ntohl((*iph)->daddr);
	//bpf_printk("CONN - ip saddr: %u, ip daddr: %u", conn.src_ip, conn.dst_ip);
	return conn;
}

static inline int from_client(struct connection *conn)
{
	if (conn->dst_port == LB_LISTENER_PORT) {
			return 1;
	}
	return 0;
}

static inline void modify_seq_ack(struct tcphdr **tcph_ptr, signed int seq_off, signed int ack_off) {
	struct tcphdr *tcph = *(tcph_ptr);
	__u32 cur_seq = bpf_ntohl(tcph->seq);
	__u32 cur_ack = bpf_ntohl(tcph->ack_seq);

	__u32 new_seq = cur_seq - seq_off;
	tcph->seq = bpf_htonl(new_seq);
	__u32 new_ack_seq = cur_ack - ack_off; 
	tcph->ack_seq = bpf_htonl(new_ack_seq);
}

static inline struct connection create_reverse_conn(struct connection *conn) 
{
	struct connection rev_conn;
	rev_conn.src_ip = conn->dst_ip;
	rev_conn.dst_ip = conn->src_ip;
	rev_conn.src_port = conn->dst_port;
	rev_conn.dst_port = conn->src_port;
	return rev_conn;
}

static inline struct server create_server_struct(struct connection *conn)
{
	// This function assumes the server is at the destination of the input connection
	struct server server;
	server.port = conn->dst_port;
	server.ip = conn->dst_ip;
	return server;
}

static inline int set_conn_available(struct server *server_ptr, struct reroute *reroute_ptr) 
{
	struct availability *availability_ptr = bpf_map_lookup_elem(&available_map, server_ptr);
	if (!availability_ptr) {
		bpf_printk("could not find avaialability in order to invalidate reroute.original");
		bpf_printk("ABORT PACKET");
		return -1;
	} else {
		__u32 index = reroute_ptr->original_index;
		if (index >= MAX_PER_SERVER) {
			bpf_printk("index: %u", index);
			bpf_printk("ABORT PACKET");
			return -1;
		} else {
			bpf_printk("index: %u", index);
			availability_ptr->valid[index] = 0;
		}

		//need to update available_map with new availability information
		if (bpf_map_update_elem(&available_map, server_ptr, availability_ptr, 0) < 0) {
			bpf_printk("unable to update available_map to invalidate old conn");
			bpf_printk("ABORT PACKET");
			return -1;
		}
	}
	return 0;
}

static inline int update_state(struct reroute *reroute_ptr, __u32 *value)
{
	struct server client;
	client.ip = reroute_ptr->original_conn.dst_ip;
	client.port = reroute_ptr->original_conn.dst_port;
	if (bpf_map_update_elem(&state_map, &client, value, 0) < 0) {
		bpf_printk("STATE - unable to change state to 1 for original_conn.dst: %u\n", reroute_ptr->original_conn.dst_port);
		return -1;
	}
	return 0;
}

SEC("xdp_tcp")
int  xdp_prog_tcp(struct xdp_md *ctx)
{
	void *data_end = (void *)(long)ctx->data_end;
	void *data = (void *)(long)ctx->data;
	struct ethhdr *ethh;
	struct iphdr *iph;
	struct tcphdr *tcph;
	__u32 action = XDP_PASS; /* Default action */
	struct hdr_cursor nh;
	int eth_type, ip_type;

	nh.pos = data;

	// bpf_printk("*** start of a new packet ***");
	// Begin initial checks
	eth_type = parse_ethhdr(&nh, data_end, &ethh);
	if (eth_type < 0) {
		goto OUT;
	}

	if (eth_type == bpf_htons(ETH_P_IP)) {
		ip_type = parse_iphdr(&nh, data_end, &iph);
	} else {
		goto OUT;
	}
	
	if (ip_type != IPPROTO_TCP) {
		goto OUT;
	}

    int tcph_len = parse_tcphdr(&nh, data_end, &tcph);
	if (tcph_len < 0) {
		goto OUT;
	}

	__u32 seq_no = bpf_htonl(tcph->seq);
	__u32 ack_seq = bpf_ntohl(tcph->ack_seq);
    
	int payload_len = bpf_ntohs(iph->tot_len) - (sizeof(struct iphdr) + tcph_len);
	struct connection conn = create_conn_struct(&tcph, &iph);

	// Query map for possible routing
	struct reroute *reroute_ptr = bpf_map_lookup_elem(&conn_map, &conn);
	if (!reroute_ptr) {
		
		if (from_client(&conn) && payload_len > 0) {
			action = XDP_ABORTED;
			bpf_printk("detected request packet that arrived before reroute init (SRC: %u, DST: %u)", conn.src_port, conn.dst_port);
			goto OUT;
		}
		
		// Introduce the seq and ack into NUMBERS_STRUCT for respective CONN
		if (tcph->ack && from_client(&conn)) {
			bpf_printk("CONN - src port: %u, dst port: %u", conn.src_port, conn.dst_port);
			bpf_printk("CONN - ip saddr: %u, ip daddr: %u", conn.src_ip, conn.dst_ip);

			struct numbers nums;
			nums.seq_no = seq_no;
			nums.ack_no = ack_seq;
			nums.init_seq = nums.seq_no;
			nums.init_ack = nums.ack_no;
			__builtin_memcpy(nums.cur_eth.src.addr, ethh->h_source, sizeof(struct eth_addr));
			__builtin_memcpy(nums.cur_eth.dst.addr, ethh->h_dest, sizeof(struct eth_addr));

			if (bpf_map_update_elem(&numbers_map, &conn, &nums, 0) < 0) {
				bpf_printk("Unable to introduce (conn.src: %u, conn.dst: %u) to numbers_map\n", conn.src_port, conn.dst_port);
				return 0;
			}

		} else if (tcph->syn && tcph->ack) {
			struct connection rev_conn = create_reverse_conn(&conn);
			struct eth_conn rev_cur;
			__builtin_memcpy(rev_cur.src.addr, ethh->h_dest, sizeof(struct eth_addr));
			__builtin_memcpy(rev_cur.dst.addr, ethh->h_source, sizeof(struct eth_addr));

			struct numbers nums;
			nums.seq_no =  ack_seq;
			nums.ack_no =  seq_no + 1;
			nums.init_seq = nums.seq_no;
			nums.init_ack = nums.ack_no;
			nums.cur_eth = rev_cur;

			if (bpf_map_update_elem(&numbers_map, &rev_conn, &nums, 0) < 0) {
				bpf_printk("Unable to introduce (conn.src: %u, conn.dst: %u) to numbers_map\n", conn.src_port, conn.dst_port);
				action = XDP_ABORTED;
				goto OUT;
			}
		}
		goto OUT;

	} else {

		if (tcph->rst && from_client(&conn)) {
			
			struct numbers *numbers_elem_ptr = bpf_map_lookup_elem(&numbers_map, &conn);
			if (!numbers_elem_ptr) {
				bpf_printk("could not find numbers elem in numbers map");
				bpf_printk("ABORT PACKET");
				action = XDP_ABORTED;
				goto OUT;
			} else {
				
				if (bpf_map_delete_elem(&numbers_map, &conn)) {
					bpf_printk("unable to delete numbers from numbers map for conn");
				}
				tcph->seq = bpf_htonl(numbers_elem_ptr->init_seq);
				tcph->ack_seq = bpf_htonl(numbers_elem_ptr->init_ack);

				perform_checksums(tcph, iph, data_end);
			}

			//create the worker struct
			//grab avaialability from available map
			//set valid[reroute.index] = 0 (not in use anymore)
			struct server server = create_server_struct(&reroute_ptr->original_conn);
			if (set_conn_available(&server, reroute_ptr) < 0) {
					action = XDP_ABORTED;
					goto OUT;
			}

			if (bpf_map_delete_elem(&conn_map, &conn)) {
				bpf_printk("unable to delete client_conn from conn map");
				bpf_printk("ABORT PACKET");
				action = XDP_ABORTED;
				goto OUT;
			}

			struct connection rev_original_conn = create_reverse_conn(&reroute_ptr->original_conn);
			if (bpf_map_delete_elem(&conn_map, &rev_original_conn)) {
				bpf_printk("unable to delete rev(original_conn) from conn map");
				bpf_printk("ABORT PACKET");
				action = XDP_ABORTED;
				goto OUT;
			}			
		
			goto OUT;
		}

		// Update NUMBERS When receiving PSH from server - (need to include payload in ack/seq)
		if (tcph->psh && from_client(&conn) == 0) {
			// rev_conn direction: server <--- middlebox
			struct connection rev_conn = create_reverse_conn(&conn);
			struct numbers *nums_ptr = bpf_map_lookup_elem(&numbers_map, &rev_conn);
			if (!nums_ptr) {
				bpf_printk("NUMBERS - Unable to retrieve numbers for (conn.src %u, conn.dst %u)\n", conn.src_port, conn.dst_port);
				action = XDP_ABORTED;
				goto OUT;
			} else {
				nums_ptr->seq_no = ack_seq;
				nums_ptr->ack_no = seq_no + payload_len;
				if (bpf_map_update_elem(&numbers_map, &rev_conn, nums_ptr, 0) < 0) {
					bpf_printk("NUMBERS - Unable to update numbers for (conn.src %u, conn.dst %u)\n", conn.src_port, conn.dst_port);
				}
			}
			
			// rev_client_conn: client ---> middlebox
			struct connection rev_client_conn = create_reverse_conn(&reroute_ptr->original_conn);
			struct numbers *client_nums_ptr = bpf_map_lookup_elem(&numbers_map, &rev_client_conn);
			if (!client_nums_ptr) {
				bpf_printk("NUMBERS - Unable to retrieve numbers for (conn.src %u, conn.dst %u)\n", rev_client_conn.src_port, rev_client_conn.dst_port);
				action = XDP_ABORTED;
				goto OUT;
			} else {
				client_nums_ptr->seq_no = ack_seq - reroute_ptr->ack_offset;
				client_nums_ptr->ack_no = seq_no - reroute_ptr->seq_offset + payload_len;
				if (bpf_map_update_elem(&numbers_map, &rev_client_conn, client_nums_ptr, 0) < 0) {
					bpf_printk("NUMBERS - Unable to update numbers for (conn.src %u, conn.dst %u)\n", rev_client_conn.src_port, rev_client_conn.dst_port);
				}
			}
		}

		//Check if rematch is needed
		if (reroute_ptr->rematch_flag == 1) {
			struct server client;
			client.ip = conn.src_ip;
			client.port = conn.src_port;
			__u32 *state_ptr = bpf_map_lookup_elem(&state_map, &client);
			if (!state_ptr) {
				bpf_printk("REMATCH - unable to retrieve state from map with conn.src %u\n", conn.src_port);
				action = XDP_ABORTED;
				goto OUT;
			}
			__u32 state = *state_ptr;
			//bpf_printk("REMATCH - state: %u\n", state);

			if (state) {
				// Safe to initiate rematching process
				// State == 2 symbolises that it was previously in resp recv state
				// Hence, now it is safely transitioned to req recv state for first time in transaction
				struct server server = create_server_struct(&reroute_ptr->original_conn);
				if (set_conn_available(&server, reroute_ptr) < 0) {
					action = XDP_ABORTED;
					goto OUT;
				}

				struct numbers *nums_ptr = bpf_map_lookup_elem(&numbers_map, &conn);
				if (!nums_ptr) {
					bpf_printk("NUMBERS - Unable to retrieve numbers for (conn.src %u, conn.dst %u)\n", conn.src_port, conn.dst_port);
					action = XDP_ABORTED;
					goto OUT;
				}

				struct numbers *server_nums_ptr = bpf_map_lookup_elem(&numbers_map, &reroute_ptr->new_conn);
				if (!server_nums_ptr) {
					bpf_printk("NUMBERS - Unable to retrieve numbers for (conn.src %u, conn.dst %u)\n", reroute_ptr->new_conn.src_port, reroute_ptr->new_conn.dst_port);
					action = XDP_ABORTED;
					goto OUT;
				}

	 			// rev_server direction: server ---> middlebox
				struct connection rev_server = create_reverse_conn(&reroute_ptr->original_conn);
				if (bpf_map_delete_elem(&conn_map, &rev_server) < 0) {
					bpf_printk("REMATCH - Unable to delete reroute object for (conn.src %u, conn.dst %u)\n", rev_server.src_port, rev_server.dst_port);
					action = XDP_ABORTED;
					goto OUT;
				}

				__s32 c_seq_offset = nums_ptr->seq_no - server_nums_ptr->seq_no;
				__s32 c_ack_offset = nums_ptr->ack_no - server_nums_ptr->ack_no;

				__s32 s_seq_offset = server_nums_ptr->ack_no - nums_ptr->ack_no;
				__s32 s_ack_offset = server_nums_ptr->seq_no - nums_ptr->seq_no;

				// First correct client->LB reroute
				reroute_ptr->original_conn = reroute_ptr->new_conn;
				reroute_ptr->original_index = reroute_ptr->new_index;
				reroute_ptr->original_eth = reroute_ptr->new_eth;
				reroute_ptr->seq_offset = c_seq_offset;
				reroute_ptr->ack_offset = c_ack_offset;
				reroute_ptr->rematch_flag = 0;

				if (bpf_map_update_elem(&conn_map, &conn, reroute_ptr, 0) < 0) {
					bpf_printk("REMATCH - Unable to upate reroute object for (conn.src %u, conn.dst %u)\n", conn.src_port, conn.dst_port);
					action = XDP_ABORTED;
					goto OUT;
				}

				struct reroute rev_reroute = *(reroute_ptr);
				rev_reroute.original_conn = create_reverse_conn(&conn);
				__builtin_memcpy(rev_reroute.original_eth.src.addr, ethh->h_dest, ETH_ALEN);
				__builtin_memcpy(rev_reroute.original_eth.dst.addr, ethh->h_source, ETH_ALEN);
				rev_reroute.seq_offset = s_seq_offset;
				rev_reroute.ack_offset = s_ack_offset;
				rev_reroute.original_index = 0;
				rev_reroute.new_index = 0;
				__builtin_memcpy(rev_reroute.new_eth.src.addr, ethh->h_dest, ETH_ALEN);
				__builtin_memcpy(rev_reroute.new_eth.dst.addr, ethh->h_source, ETH_ALEN);
				rev_reroute.new_conn = rev_reroute.original_conn;

				struct connection rev_new_server = create_reverse_conn(&reroute_ptr->new_conn);

				if (bpf_map_update_elem(&conn_map, &rev_new_server, &rev_reroute, 0) < 0) {
					//bpf_printk("REMATCH - Unable to upate reroute object for (conn.src %u, conn.dst %u)\n", rev_new_server.src_port, rev_new_server.dst_port);
					action = XDP_ABORTED;
					goto OUT;
				}			
			}
		}

		// Update state to be zero
		if (payload_len > 0 && from_client(&conn)) {
			__u32 zero = 0;
			if (update_state(reroute_ptr, &zero) < 0) {
				action = XDP_ABORTED;
				goto OUT;
			}
		} else if (payload_len > 0) {
			__u32 one = 1;
			if (update_state(reroute_ptr, &one) < 0) {
				action = XDP_ABORTED;
				goto OUT;
			}
		}

		modify_seq_ack(&tcph, reroute_ptr->seq_offset, reroute_ptr->ack_offset);
		tcph->source = bpf_htons(reroute_ptr->original_conn.src_port);
		tcph->dest = bpf_htons(reroute_ptr->original_conn.dst_port);
		
		iph->saddr = bpf_htonl(reroute_ptr->original_conn.src_ip);
		iph->daddr = bpf_htonl(reroute_ptr->original_conn.dst_ip);

		__builtin_memcpy(ethh->h_source, reroute_ptr->original_eth.src.addr, sizeof(struct eth_addr));
		__builtin_memcpy(ethh->h_dest, reroute_ptr->original_eth.dst.addr, sizeof(struct eth_addr));
			
		perform_checksums(tcph, iph, data_end);
		action = XDP_TX;
	}
OUT:
	//bpf_printk("*** end of a packet ***");
	return action;
}

char _license[] SEC("license") = "GPL";