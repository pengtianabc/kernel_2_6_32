/*
 * VXLAN: Virtual eXtensible Local Area Network
 *
 * Copyright (c) 2012-2013 Vyatta Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * TODO
 *  - IPv6 (not in RFC)
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/module.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/skbuff.h>
#include <linux/rculist.h>
#include <linux/netdevice.h>
#include <linux/in.h>
#include <linux/ip.h>
#include <linux/udp.h>
#include <linux/igmp.h>
#include <linux/etherdevice.h>
#include <linux/if_ether.h>
#include <linux/if_vlan.h>
#include <linux/hash.h>
#include <linux/u64_stats_sync.h>
#include <linux/ethtool.h>
#include <net/arp.h>
#include <net/ndisc.h>
#include <net/ip.h>
#include <net/ip_tunnels.h>
#include <net/icmp.h>
#include <net/udp.h>
#include <net/rtnetlink.h>
#include <net/route.h>
#include <net/dsfield.h>
#include <net/inet_ecn.h>
#include <net/net_namespace.h>
#include <net/netns/generic.h>
#include <net/vxlan.h>

#define VXLAN_VERSION	"0.1"

#define PORT_HASH_BITS	8
#define PORT_HASH_SIZE  (1<<PORT_HASH_BITS)
#define VNI_HASH_BITS	10
#define VNI_HASH_SIZE	(1<<VNI_HASH_BITS)
#define FDB_HASH_BITS	8
#define FDB_HASH_SIZE	(1<<FDB_HASH_BITS)
#define FDB_AGE_DEFAULT 300 /* 5 min */
#define FDB_AGE_INTERVAL (10 * HZ)	/* rescan interval */

#define VXLAN_N_VID	(1u << 24)
#define VXLAN_VID_MASK	(VXLAN_N_VID - 1)
/* IP header + UDP + VXLAN + Ethernet header */
#define VXLAN_HEADROOM (20 + 8 + 8 + 14)
#define VXLAN_HLEN (sizeof(struct udphdr) + sizeof(struct vxlanhdr))

#define VXLAN_FLAGS 0x08000000	/* struct vxlanhdr.vx_flags required value. */

/* VXLAN protocol header */
struct vxlanhdr {
	__be32 vx_flags;
	__be32 vx_vni;
};

/* UDP port for VXLAN traffic.
 * The IANA assigned port is 4789, but the Linux default is 8472
 * for compatibility with early adopters.
 */
static unsigned short vxlan_port __read_mostly = 8472;
module_param_named(udp_port, vxlan_port, ushort, 0444);
MODULE_PARM_DESC(udp_port, "Destination UDP port");

static bool log_ecn_error = true;
module_param(log_ecn_error, bool, 0644);
MODULE_PARM_DESC(log_ecn_error, "Log packets received with corrupted ECN");

static int vxlan_net_id;

static const u8 all_zeros_mac[ETH_ALEN];

/* per-network namespace private data for this module */
struct vxlan_net {
	struct list_head  vxlan_list;
	struct hlist_head sock_list[PORT_HASH_SIZE];
	spinlock_t	  sock_lock;
};

struct vxlan_rdst {
	__be32			 remote_ip;
	__be16			 remote_port;
	u32			 remote_vni;
	u32			 remote_ifindex;
	struct list_head	 list;
	struct rcu_head		 rcu;
};

/* Forwarding table entry */
struct vxlan_fdb {
	struct hlist_node hlist;	/* linked list of entries */
	struct rcu_head	  rcu;
	unsigned long	  updated;	/* jiffies */
	unsigned long	  used;
	struct list_head  remotes;
	u16		  state;	/* see ndm_state */
	u8		  flags;	/* see ndm_flags */
	u8		  eth_addr[ETH_ALEN];
};

/* Per-cpu network traffic stats */
struct vxlan_stats {
	u64			rx_packets;
	u64			rx_bytes;
	u64			tx_packets;
	u64			tx_bytes;
	struct u64_stats_sync	syncp;
};

/* Pseudo network device */
struct vxlan_dev {
	struct hlist_node hlist;	/* vni hash table */
	struct list_head  next;		/* vxlan's per namespace list */
	struct vxlan_sock *vn_sock;	/* listening socket */
	struct net_device *dev;
	struct vxlan_stats __percpu *stats;
	struct vxlan_rdst default_dst;	/* default destination */
	__be32		  saddr;	/* source address */
	__be16		  dst_port;
	__u16		  port_min;	/* source port range */
	__u16		  port_max;
	__u8		  tos;		/* TOS override */
	__u8		  ttl;
	u32		  flags;	/* VXLAN_F_* below */

	struct work_struct sock_work;
	struct work_struct igmp_join;
	struct work_struct igmp_leave;

	unsigned long	  age_interval;
	struct timer_list age_timer;
	spinlock_t	  hash_lock;
	unsigned int	  addrcnt;
	unsigned int	  addrmax;

	struct hlist_head fdb_head[FDB_HASH_SIZE];
};

#define VXLAN_F_LEARN	0x01
#define VXLAN_F_PROXY	0x02
#define VXLAN_F_RSC	0x04
#define VXLAN_F_L2MISS	0x08
#define VXLAN_F_L3MISS	0x10

/* salt for hash table */
static u32 vxlan_salt __read_mostly;
static struct workqueue_struct *vxlan_wq;

static void vxlan_sock_work(struct work_struct *work);

/* Virtual Network hash table head */
static inline struct hlist_head *vni_head(struct vxlan_sock *vs, u32 id)
{
	return &vs->vni_list[hash_32(id, VNI_HASH_BITS)];
}

/* Socket hash table head */
static inline struct hlist_head *vs_head(struct net *net, __be16 port)
{
	struct vxlan_net *vn = net_generic(net, vxlan_net_id);

	return &vn->sock_list[hash_32(ntohs(port), PORT_HASH_BITS)];
}

/* First remote destination for a forwarding entry.
 * Guaranteed to be non-NULL because remotes are never deleted.
 */
static inline struct vxlan_rdst *first_remote_rcu(struct vxlan_fdb *fdb)
{
	return list_entry_rcu(fdb->remotes.next, struct vxlan_rdst, list);
}

static inline struct vxlan_rdst *first_remote_rtnl(struct vxlan_fdb *fdb)
{
	return list_first_entry(&fdb->remotes, struct vxlan_rdst, list);
}

/* Find VXLAN socket based on network namespace and UDP port */
static struct vxlan_sock *vxlan_find_sock(struct net *net, __be16 port)
{
	struct vxlan_sock *vs;
	struct hlist_node *node;

	hlist_for_each_entry_rcu(vs, node, vs_head(net, port), hlist) {
		if (inet_sk(vs->sock->sk)->sport == port)
			return vs;
	}
	return NULL;
}

static struct vxlan_dev *vxlan_vs_find_vni(struct vxlan_sock *vs, u32 id)
{
	struct vxlan_dev *vxlan;
	struct hlist_node *node;

	hlist_for_each_entry_rcu(vxlan, node, vni_head(vs, id), hlist) {
		if (vxlan->default_dst.remote_vni == id)
			return vxlan;
	}

	return NULL;
}

/* Look up VNI in a per net namespace table */
static struct vxlan_dev *vxlan_find_vni(struct net *net, u32 id, __be16 port)
{
	struct vxlan_sock *vs;

	vs = vxlan_find_sock(net, port);
	if (!vs)
		return NULL;

	return vxlan_vs_find_vni(vs, id);
}

/* Fill in neighbour message in skbuff. */
static int vxlan_fdb_info(struct sk_buff *skb, struct vxlan_dev *vxlan,
			  const struct vxlan_fdb *fdb,
			  u32 portid, u32 seq, int type, unsigned int flags,
			  const struct vxlan_rdst *rdst)
{
	unsigned long now = jiffies;
	struct nda_cacheinfo ci;
	struct nlmsghdr *nlh;
	struct ndmsg *ndm;
	bool send_ip, send_eth;

	nlh = nlmsg_put(skb, portid, seq, type, sizeof(*ndm), flags);
	if (nlh == NULL)
		return -EMSGSIZE;

	ndm = nlmsg_data(nlh);
	memset(ndm, 0, sizeof(*ndm));

	send_eth = send_ip = true;

	if (type == RTM_GETNEIGH) {
		ndm->ndm_family	= AF_INET;
		send_ip = rdst->remote_ip != htonl(INADDR_ANY);
		send_eth = !is_zero_ether_addr(fdb->eth_addr);
	} else
		ndm->ndm_family	= AF_BRIDGE;
	ndm->ndm_state = fdb->state;
	ndm->ndm_ifindex = vxlan->dev->ifindex;
	ndm->ndm_flags = fdb->flags;
	ndm->ndm_type = NDA_DST;

	if (send_eth && nla_put(skb, NDA_LLADDR, ETH_ALEN, &fdb->eth_addr))
		goto nla_put_failure;

	if (send_ip && nla_put_be32(skb, NDA_DST, rdst->remote_ip))
		goto nla_put_failure;

	if (rdst->remote_port && rdst->remote_port != vxlan->dst_port &&
	    nla_put_be16(skb, NDA_PORT, rdst->remote_port))
		goto nla_put_failure;
	if (rdst->remote_vni != vxlan->default_dst.remote_vni &&
	    nla_put_u32(skb, NDA_VNI, rdst->remote_vni))
		goto nla_put_failure;
	if (rdst->remote_ifindex &&
	    nla_put_u32(skb, NDA_IFINDEX, rdst->remote_ifindex))
		goto nla_put_failure;

	ci.ndm_used	 = jiffies_to_clock_t(now - fdb->used);
	ci.ndm_confirmed = 0;
	ci.ndm_updated	 = jiffies_to_clock_t(now - fdb->updated);
	ci.ndm_refcnt	 = 0;

	if (nla_put(skb, NDA_CACHEINFO, sizeof(ci), &ci))
		goto nla_put_failure;

	return nlmsg_end(skb, nlh);

nla_put_failure:
	nlmsg_cancel(skb, nlh);
	return -EMSGSIZE;
}

static inline size_t vxlan_nlmsg_size(void)
{
	return NLMSG_ALIGN(sizeof(struct ndmsg))
		+ nla_total_size(ETH_ALEN) /* NDA_LLADDR */
		+ nla_total_size(sizeof(__be32)) /* NDA_DST */
		+ nla_total_size(sizeof(__be16)) /* NDA_PORT */
		+ nla_total_size(sizeof(__be32)) /* NDA_VNI */
		+ nla_total_size(sizeof(__u32)) /* NDA_IFINDEX */
		+ nla_total_size(sizeof(struct nda_cacheinfo));
}

static void vxlan_fdb_notify(struct vxlan_dev *vxlan,
			     struct vxlan_fdb *fdb, int type)
{
	struct net *net = dev_net(vxlan->dev);
	struct sk_buff *skb;
	int err = -ENOBUFS;

	skb = nlmsg_new(vxlan_nlmsg_size(), GFP_ATOMIC);
	if (skb == NULL)
		goto errout;

	err = vxlan_fdb_info(skb, vxlan, fdb, 0, 0, type, 0,
			     first_remote_rtnl(fdb));
	if (err < 0) {
		/* -EMSGSIZE implies BUG in vxlan_nlmsg_size() */
		WARN_ON(err == -EMSGSIZE);
		kfree_skb(skb);
		goto errout;
	}

	rtnl_notify(skb, net, 0, RTNLGRP_NEIGH, NULL, GFP_ATOMIC);
	return;
errout:
	if (err < 0)
		rtnl_set_sk_err(net, RTNLGRP_NEIGH, err);
}

static void vxlan_ip_miss(struct net_device *dev, __be32 ipa)
{
	struct vxlan_dev *vxlan = netdev_priv(dev);
	struct vxlan_fdb f = {
		.state = NUD_STALE,
	};
	struct vxlan_rdst remote = {
		.remote_ip = ipa, /* goes to NDA_DST */
		.remote_vni = VXLAN_N_VID,
	};

	INIT_LIST_HEAD(&f.remotes);
	list_add_rcu(&remote.list, &f.remotes);

	vxlan_fdb_notify(vxlan, &f, RTM_GETNEIGH);
}

static void vxlan_fdb_miss(struct vxlan_dev *vxlan, const u8 eth_addr[ETH_ALEN])
{
	struct vxlan_fdb f = {
		.state = NUD_STALE,
	};

	INIT_LIST_HEAD(&f.remotes);
	memcpy(f.eth_addr, eth_addr, ETH_ALEN);

	vxlan_fdb_notify(vxlan, &f, RTM_GETNEIGH);
}

/* Hash Ethernet address */
static u32 eth_hash(const unsigned char *addr)
{
	u64 value = get_unaligned((u64 *)addr);

	/* only want 6 bytes */
#ifdef __BIG_ENDIAN
	value >>= 16;
#else
	value <<= 16;
#endif
	return hash_64(value, FDB_HASH_BITS);
}

/* Hash chain to use given mac address */
static inline struct hlist_head *vxlan_fdb_head(struct vxlan_dev *vxlan,
						const u8 *mac)
{
	return &vxlan->fdb_head[eth_hash(mac)];
}

/* Look up Ethernet address in forwarding table */
static struct vxlan_fdb *__vxlan_find_mac(struct vxlan_dev *vxlan,
					const u8 *mac)

{
	struct hlist_head *head = vxlan_fdb_head(vxlan, mac);
	struct vxlan_fdb *f;
	struct hlist_node *node;

	hlist_for_each_entry_rcu(f, node, head, hlist) {
		if (compare_ether_addr(mac, f->eth_addr) == 0)
			return f;
	}

	return NULL;
}

static struct vxlan_fdb *vxlan_find_mac(struct vxlan_dev *vxlan,
					const u8 *mac)
{
	struct vxlan_fdb *f;

	f = __vxlan_find_mac(vxlan, mac);
	if (f)
		f->used = jiffies;

	return f;
}

/* caller should hold vxlan->hash_lock */
static struct vxlan_rdst *vxlan_fdb_find_rdst(struct vxlan_fdb *f,
					      __be32 ip, __be16 port,
					      __u32 vni, __u32 ifindex)
{
	struct vxlan_rdst *rd;

	list_for_each_entry(rd, &f->remotes, list) {
		if (rd->remote_ip == ip &&
		    rd->remote_port == port &&
		    rd->remote_vni == vni &&
		    rd->remote_ifindex == ifindex)
			return rd;
	}

	return NULL;
}

/* Replace destination of unicast mac */
static int vxlan_fdb_replace(struct vxlan_fdb *f,
			    __be32 ip, __be16 port, __u32 vni, __u32 ifindex)
{
	struct vxlan_rdst *rd;

	rd = vxlan_fdb_find_rdst(f, ip, port, vni, ifindex);
	if (rd)
		return 0;

	rd = list_first_entry_or_null(&f->remotes, struct vxlan_rdst, list);
	if (!rd)
		return 0;
	rd->remote_ip = ip;
	rd->remote_port = port;
	rd->remote_vni = vni;
	rd->remote_ifindex = ifindex;
	return 1;
}

/* Add/update destinations for multicast */
static int vxlan_fdb_append(struct vxlan_fdb *f,
			    __be32 ip, __be16 port, __u32 vni, __u32 ifindex)
{
	struct vxlan_rdst *rd;

	rd = vxlan_fdb_find_rdst(f, ip, port, vni, ifindex);
	if (rd)
		return 0;

	rd = kmalloc(sizeof(*rd), GFP_ATOMIC);
	if (rd == NULL)
		return -ENOBUFS;
	rd->remote_ip = ip;
	rd->remote_port = port;
	rd->remote_vni = vni;
	rd->remote_ifindex = ifindex;

	list_add_tail_rcu(&rd->list, &f->remotes);

	return 1;
}

/* Add new entry to forwarding table -- assumes lock held */
static int vxlan_fdb_create(struct vxlan_dev *vxlan,
			    const u8 *mac, __be32 ip,
			    __u16 state, __u16 flags,
			    __be16 port, __u32 vni, __u32 ifindex,
			    __u8 ndm_flags)
{
	struct vxlan_fdb *f;
	int notify = 0;

	f = __vxlan_find_mac(vxlan, mac);
	if (f) {
		if (flags & NLM_F_EXCL) {
			netdev_dbg(vxlan->dev,
				   "lost race to create %pM\n", mac);
			return -EEXIST;
		}
		if (f->state != state) {
			f->state = state;
			f->updated = jiffies;
			notify = 1;
		}
		if (f->flags != ndm_flags) {
			f->flags = ndm_flags;
			f->updated = jiffies;
			notify = 1;
		}
		if ((flags & NLM_F_REPLACE)) {
			/* Only change unicasts */
			if (!(is_multicast_ether_addr(f->eth_addr) ||
			     is_zero_ether_addr(f->eth_addr))) {
				int rc = vxlan_fdb_replace(f, ip, port, vni,
							   ifindex);

				if (rc < 0)
					return rc;
				notify |= rc;
			} else
				return -EOPNOTSUPP;
		}
		if ((flags & NLM_F_APPEND) &&
		    (is_multicast_ether_addr(f->eth_addr) ||
		     is_zero_ether_addr(f->eth_addr))) {
			int rc = vxlan_fdb_append(f, ip, port, vni, ifindex);

			if (rc < 0)
				return rc;
			notify |= rc;
		}
	} else {
		if (!(flags & NLM_F_CREATE))
			return -ENOENT;

		if (vxlan->addrmax && vxlan->addrcnt >= vxlan->addrmax)
			return -ENOSPC;

		/* Disallow replace to add a multicast entry */
		if ((flags & NLM_F_REPLACE) &&
		    (is_multicast_ether_addr(mac) || is_zero_ether_addr(mac)))
			return -EOPNOTSUPP;

		netdev_dbg(vxlan->dev, "add %pM -> %pI4\n", mac, &ip);
		f = kmalloc(sizeof(*f), GFP_ATOMIC);
		if (!f)
			return -ENOMEM;

		notify = 1;
		f->state = state;
		f->flags = ndm_flags;
		f->updated = f->used = jiffies;
		INIT_LIST_HEAD(&f->remotes);
		memcpy(f->eth_addr, mac, ETH_ALEN);

		vxlan_fdb_append(f, ip, port, vni, ifindex);

		++vxlan->addrcnt;
		hlist_add_head_rcu(&f->hlist,
				   vxlan_fdb_head(vxlan, mac));
	}

	if (notify)
		vxlan_fdb_notify(vxlan, f, RTM_NEWNEIGH);

	return 0;
}

static void vxlan_fdb_free_rdst(struct rcu_head *head)
{
	struct vxlan_rdst *rd = container_of(head, struct vxlan_rdst, rcu);
	kfree(rd);
}

static void vxlan_fdb_free(struct rcu_head *head)
{
	struct vxlan_fdb *f = container_of(head, struct vxlan_fdb, rcu);
	struct vxlan_rdst *rd, *nd;

	list_for_each_entry_safe(rd, nd, &f->remotes, list)
		kfree(rd);
	kfree(f);
}

static void vxlan_fdb_destroy(struct vxlan_dev *vxlan, struct vxlan_fdb *f)
{
	netdev_dbg(vxlan->dev,
		    "delete %pM\n", f->eth_addr);

	--vxlan->addrcnt;
	vxlan_fdb_notify(vxlan, f, RTM_DELNEIGH);

	hlist_del_rcu(&f->hlist);
	call_rcu(&f->rcu, vxlan_fdb_free);
}

static int vxlan_fdb_parse(struct nlattr *tb[], struct vxlan_dev *vxlan,
			   __be32 *ip, __be16 *port, u32 *vni, u32 *ifindex)
{
	struct net *net = dev_net(vxlan->dev);

	if (tb[NDA_DST]) {
		if (nla_len(tb[NDA_DST]) != sizeof(__be32))
			return -EAFNOSUPPORT;

		*ip = nla_get_be32(tb[NDA_DST]);
	} else {
		*ip = htonl(INADDR_ANY);
	}

	if (tb[NDA_PORT]) {
		if (nla_len(tb[NDA_PORT]) != sizeof(__be16))
			return -EINVAL;
		*port = nla_get_be16(tb[NDA_PORT]);
	} else {
		*port = vxlan->dst_port;
	}

	if (tb[NDA_VNI]) {
		if (nla_len(tb[NDA_VNI]) != sizeof(u32))
			return -EINVAL;
		*vni = nla_get_u32(tb[NDA_VNI]);
	} else {
		*vni = vxlan->default_dst.remote_vni;
	}

	if (tb[NDA_IFINDEX]) {
		struct net_device *tdev;

		if (nla_len(tb[NDA_IFINDEX]) != sizeof(u32))
			return -EINVAL;
		*ifindex = nla_get_u32(tb[NDA_IFINDEX]);
		tdev = dev_get_by_index(net, *ifindex);
		if (!tdev)
			return -EADDRNOTAVAIL;
		dev_put(tdev);
	} else {
		*ifindex = 0;
	}

	return 0;
}

/* Add static entry (via netlink) */
static int vxlan_fdb_add(struct ndmsg *ndm, struct nlattr *tb[],
			 struct net_device *dev,
			 const unsigned char *addr, u16 flags)
{
	struct vxlan_dev *vxlan = netdev_priv(dev);
	/* struct net *net = dev_net(vxlan->dev); */
	__be32 ip;
	__be16 port;
	u32 vni, ifindex;
	int err;

	if (!(ndm->ndm_state & (NUD_PERMANENT|NUD_REACHABLE))) {
		pr_info("RTM_NEWNEIGH with invalid state %#x\n",
			ndm->ndm_state);
		return -EINVAL;
	}

	if (tb[NDA_DST] == NULL)
		return -EINVAL;

	err = vxlan_fdb_parse(tb, vxlan, &ip, &port, &vni, &ifindex);
	if (err)
		return err;

	spin_lock_bh(&vxlan->hash_lock);
	err = vxlan_fdb_create(vxlan, addr, ip, ndm->ndm_state, flags,
			       port, vni, ifindex, ndm->ndm_flags);
	spin_unlock_bh(&vxlan->hash_lock);

	return err;
}

/* Delete entry (via netlink) */
static int vxlan_fdb_delete(struct ndmsg *ndm, struct nlattr *tb[],
			    struct net_device *dev,
			    const unsigned char *addr)
{
	struct vxlan_dev *vxlan = netdev_priv(dev);
	struct vxlan_fdb *f;
	struct vxlan_rdst *rd = NULL;
	__be32 ip;
	__be16 port;
	u32 vni, ifindex;
	int err;

	err = vxlan_fdb_parse(tb, vxlan, &ip, &port, &vni, &ifindex);
	if (err)
		return err;

	err = -ENOENT;

	spin_lock_bh(&vxlan->hash_lock);
	f = vxlan_find_mac(vxlan, addr);
	if (!f)
		goto out;

	if (ip != htonl(INADDR_ANY)) {
		rd = vxlan_fdb_find_rdst(f, ip, port, vni, ifindex);
		if (!rd)
			goto out;
	}

	err = 0;

	/* remove a destination if it's not the only one on the list,
	 * otherwise destroy the fdb entry
	 */
	if (rd && !list_is_singular(&f->remotes)) {
		list_del_rcu(&rd->list);
		call_rcu(&rd->rcu, vxlan_fdb_free_rdst);
		goto out;
	}

	vxlan_fdb_destroy(vxlan, f);

out:
	spin_unlock_bh(&vxlan->hash_lock);

	return err;
}

/* Dump forwarding table */
static int vxlan_fdb_dump(struct sk_buff *skb, struct netlink_callback *cb,
			  struct net_device *dev, int idx)
{
	struct vxlan_dev *vxlan = netdev_priv(dev);
	unsigned int h;

	for (h = 0; h < FDB_HASH_SIZE; ++h) {
		struct vxlan_fdb *f;
		struct hlist_node *n;
		int err;

		hlist_for_each_entry(f, n, &vxlan->fdb_head[h], hlist) {
			struct vxlan_rdst *rd;

			if (idx < cb->args[0])
				goto skip;

			list_for_each_entry_rcu(rd, &f->remotes, list) {
				err = vxlan_fdb_info(skb, vxlan, f,
						     NETLINK_CB(cb->skb).pid,
						     cb->nlh->nlmsg_seq,
						     RTM_NEWNEIGH,
						     NLM_F_MULTI, rd);
				if (err < 0)
					goto out;
			}
skip:
			++idx;
		}
	}
out:
	return idx;
}

/* Watch incoming packets to learn mapping between Ethernet address
 * and Tunnel endpoint.
 * Return true if packet is bogus and should be droppped.
 */
static bool vxlan_snoop(struct net_device *dev,
			__be32 src_ip, const u8 *src_mac)
{
	struct vxlan_dev *vxlan = netdev_priv(dev);
	struct vxlan_fdb *f;

	f = vxlan_find_mac(vxlan, src_mac);
	if (likely(f)) {
		struct vxlan_rdst *rdst = first_remote_rcu(f);

		if (likely(rdst->remote_ip == src_ip))
			return false;

		/* Don't migrate static entries, drop packets */
		if (f->state & NUD_NOARP)
			return true;

		if (net_ratelimit())
			printk(KERN_INFO
				    "%s: %pM migrated from %pI4 to %pI4\n",
				    netdev_name(dev),
				    src_mac, &rdst->remote_ip, &src_ip);

		rdst->remote_ip = src_ip;
		f->updated = jiffies;
		vxlan_fdb_notify(vxlan, f, RTM_NEWNEIGH);
	} else {
		/* learned new entry */
		spin_lock(&vxlan->hash_lock);

		/* close off race between vxlan_flush and incoming packets */
		if (netif_running(dev))
			vxlan_fdb_create(vxlan, src_mac, src_ip,
					 NUD_REACHABLE,
					 NLM_F_EXCL|NLM_F_CREATE,
					 vxlan->dst_port,
					 vxlan->default_dst.remote_vni,
					 0, NTF_SELF);
		spin_unlock(&vxlan->hash_lock);
	}

	return false;
}

/* See if multicast group is already in use by other ID */
static bool vxlan_group_used(struct vxlan_net *vn, __be32 remote_ip)
{
	struct vxlan_dev *vxlan;

	list_for_each_entry(vxlan, &vn->vxlan_list, next) {
		if (!netif_running(vxlan->dev))
			continue;

		if (vxlan->default_dst.remote_ip == remote_ip)
			return true;
	}

	return false;
}

static void vxlan_sock_hold(struct vxlan_sock *vs)
{
	atomic_inc(&vs->refcnt);
}

void vxlan_sock_release(struct vxlan_sock *vs)
{
	struct vxlan_net *vn = net_generic(sock_net(vs->sock->sk), vxlan_net_id);

	if (!atomic_dec_and_test(&vs->refcnt))
		return;

	spin_lock(&vn->sock_lock);
	hlist_del_rcu(&vs->hlist);
	spin_unlock(&vn->sock_lock);

	queue_work(vxlan_wq, &vs->del_work);
}
EXPORT_SYMBOL_GPL(vxlan_sock_release);

/* Callback to update multicast group membership when first VNI on
 * multicast asddress is brought up
 * Done as workqueue because ip_mc_join_group acquires RTNL.
 */
static void vxlan_igmp_join(struct work_struct *work)
{
	struct vxlan_dev *vxlan = container_of(work, struct vxlan_dev, igmp_join);
	struct vxlan_sock *vs = vxlan->vn_sock;
	struct sock *sk = vs->sock->sk;
	struct ip_mreqn mreq = {
		.imr_multiaddr.s_addr	= vxlan->default_dst.remote_ip,
		.imr_ifindex		= vxlan->default_dst.remote_ifindex,
	};

	lock_sock(sk);
	ip_mc_join_group(sk, &mreq);
	release_sock(sk);

	vxlan_sock_release(vs);
	dev_put(vxlan->dev);
}

/* Inverse of vxlan_igmp_join when last VNI is brought down */
static void vxlan_igmp_leave(struct work_struct *work)
{
	struct vxlan_dev *vxlan = container_of(work, struct vxlan_dev, igmp_leave);
	struct vxlan_sock *vs = vxlan->vn_sock;
	struct sock *sk = vs->sock->sk;
	struct ip_mreqn mreq = {
		.imr_multiaddr.s_addr	= vxlan->default_dst.remote_ip,
		.imr_ifindex		= vxlan->default_dst.remote_ifindex,
	};

	lock_sock(sk);
	ip_mc_leave_group(sk, &mreq);
	release_sock(sk);

	vxlan_sock_release(vs);
	dev_put(vxlan->dev);
}

/* Callback from net/ipv4/udp.c to receive packets */
static int vxlan_udp_encap_recv(struct sock *sk, struct sk_buff *skb)
{
	struct vxlan_sock *vs;
	struct vxlanhdr *vxh;
	__be16 port;

	/* Need Vxlan and inner Ethernet header to be present */
	if (!pskb_may_pull(skb, VXLAN_HLEN))
		goto error;

	/* Return packets with reserved bits set */
	vxh = (struct vxlanhdr *)(udp_hdr(skb) + 1);
	if (vxh->vx_flags != htonl(VXLAN_FLAGS) ||
	    (vxh->vx_vni & htonl(0xff))) {
		netdev_dbg(skb->dev, "invalid vxlan flags=%#x vni=%#x\n",
			   ntohl(vxh->vx_flags), ntohl(vxh->vx_vni));
		goto error;
	}

	if (iptunnel_pull_header(skb, VXLAN_HLEN, htons(ETH_P_TEB)))
		goto drop;

	port = inet_sk(sk)->sport;

	vs = vxlan_find_sock(sock_net(sk), port);
	if (!vs)
		goto drop;

	vs->rcv(vs, skb, vxh->vx_vni);
	return 0;

drop:
	/* Consume bad packet */
	kfree_skb(skb);
	return 0;

error:
	/* Return non vxlan pkt */
	return 1;
}

static void vxlan_rcv(struct vxlan_sock *vs,
		      struct sk_buff *skb, __be32 vx_vni)
{
	struct iphdr *oip;
	struct vxlan_dev *vxlan;
	struct vxlan_stats *stats;
	__u32 vni;
	int err;

	vni = ntohl(vx_vni) >> 8;
	/* Is this VNI defined? */
	vxlan = vxlan_vs_find_vni(vs, vni);
	if (!vxlan)
		goto drop;

	skb_reset_mac_header(skb);
	skb->protocol = eth_type_trans(skb, vxlan->dev);

	/* Ignore packet loops (and multicast echo) */
	if (compare_ether_addr(eth_hdr(skb)->h_source,
			       vxlan->dev->dev_addr) == 0)
		goto drop;

	/* Re-examine inner Ethernet packet */
	oip = ip_hdr(skb);
	if ((vxlan->flags & VXLAN_F_LEARN) &&
	    vxlan_snoop(skb->dev, oip->saddr, eth_hdr(skb)->h_source))
		goto drop;

	skb_reset_network_header(skb);

	/* If the NIC driver gave us an encapsulated packet with
	 * CHECKSUM_UNNECESSARY and Rx checksum feature is enabled,
	 * leave the CHECKSUM_UNNECESSARY, the device checksummed it
	 * for us. Otherwise force the upper layers to verify it.
	 */
	if (skb->ip_summed != CHECKSUM_UNNECESSARY || !skb->encapsulation ||
	    !(vxlan->dev->features & NETIF_F_RXCSUM))
		skb->ip_summed = CHECKSUM_NONE;

	skb->encapsulation = 0;

	err = INET_ECN_set_ce(skb);
	if (unlikely(err)) {
		if (log_ecn_error)
			net_info_ratelimited("non-ECT from %pI4 with TOS=%#x\n",
					     &oip->saddr, oip->tos);
		if (err > 1) {
			++vxlan->dev->stats.rx_frame_errors;
			++vxlan->dev->stats.rx_errors;
			goto drop;
		}
	}

	stats = this_cpu_ptr(vxlan->stats);
	u64_stats_update_begin(&stats->syncp);
	stats->rx_packets++;
	stats->rx_bytes += skb->len;
	u64_stats_update_end(&stats->syncp);

	netif_rx(skb);

	return;
drop:
	/* Consume bad packet */
	kfree_skb(skb);
}

static int arp_reduce(struct net_device *dev, struct sk_buff *skb)
{
	struct vxlan_dev *vxlan = netdev_priv(dev);
	struct arphdr *parp;
	u8 *arpptr, *sha;
	__be32 sip, tip;
	struct neighbour *n;

	if (dev->flags & IFF_NOARP)
		goto out;

	if (!pskb_may_pull(skb, arp_hdr_len(dev))) {
		dev->stats.tx_dropped++;
		goto out;
	}
	parp = arp_hdr(skb);

	if ((parp->ar_hrd != htons(ARPHRD_ETHER) &&
	     parp->ar_hrd != htons(ARPHRD_IEEE802)) ||
	    parp->ar_pro != htons(ETH_P_IP) ||
	    parp->ar_op != htons(ARPOP_REQUEST) ||
	    parp->ar_hln != dev->addr_len ||
	    parp->ar_pln != 4)
		goto out;
	arpptr = (u8 *)parp + sizeof(struct arphdr);
	sha = arpptr;
	arpptr += dev->addr_len;	/* sha */
	memcpy(&sip, arpptr, sizeof(sip));
	arpptr += sizeof(sip);
	arpptr += dev->addr_len;	/* tha */
	memcpy(&tip, arpptr, sizeof(tip));

	if (ipv4_is_loopback(tip) ||
	    ipv4_is_multicast(tip))
		goto out;

	n = neigh_lookup(&arp_tbl, &tip, dev);

	if (n) {
		struct vxlan_fdb *f;
		struct sk_buff	*reply;

		if (!(n->nud_state & NUD_CONNECTED)) {
			neigh_release(n);
			goto out;
		}

		f = vxlan_find_mac(vxlan, n->ha);
		if (f && first_remote_rcu(f)->remote_ip == htonl(INADDR_ANY)) {
			/* bridge-local neighbor */
			neigh_release(n);
			goto out;
		}

		reply = arp_create(ARPOP_REPLY, ETH_P_ARP, sip, dev, tip, sha,
				n->ha, sha);

		neigh_release(n);

		skb_reset_mac_header(reply);
		__skb_pull(reply, skb_network_offset(reply));
		reply->ip_summed = CHECKSUM_UNNECESSARY;
		reply->pkt_type = PACKET_HOST;

		if (netif_rx_ni(reply) == NET_RX_DROP)
			dev->stats.rx_dropped++;
	} else if (vxlan->flags & VXLAN_F_L3MISS)
		vxlan_ip_miss(dev, tip);
out:
	consume_skb(skb);
	return NETDEV_TX_OK;
}

static bool route_shortcircuit(struct net_device *dev, struct sk_buff *skb)
{
	struct vxlan_dev *vxlan = netdev_priv(dev);
	struct neighbour *n;
	struct iphdr *pip;

	if (is_multicast_ether_addr(eth_hdr(skb)->h_dest))
		return false;

	n = NULL;
	switch (ntohs(eth_hdr(skb)->h_proto)) {
	case ETH_P_IP:
		if (!pskb_may_pull(skb, sizeof(struct iphdr)))
			return false;
		pip = ip_hdr(skb);
		n = neigh_lookup(&arp_tbl, &pip->daddr, dev);
		break;
	default:
		return false;
	}

	if (n) {
		bool diff;

		diff = compare_ether_addr(eth_hdr(skb)->h_dest, n->ha) != 0;
		if (diff) {
			memcpy(eth_hdr(skb)->h_source, eth_hdr(skb)->h_dest,
				dev->addr_len);
			memcpy(eth_hdr(skb)->h_dest, n->ha, dev->addr_len);
		}
		neigh_release(n);
		return diff;
	} else if (vxlan->flags & VXLAN_F_L3MISS)
		vxlan_ip_miss(dev, pip->daddr);
	return false;
}

static void vxlan_sock_put(struct sk_buff *skb)
{
	sock_put(skb->sk);
}

/* On transmit, associate with the tunnel socket */
static void vxlan_set_owner(struct sock *sk, struct sk_buff *skb)
{
	skb_orphan(skb);
	sock_hold(sk);
	skb->sk = sk;
	skb->destructor = vxlan_sock_put;
}

/* Compute source port for outgoing packet
 *   first choice to use L4 flow hash since it will spread
 *     better and maybe available from hardware
 *   secondary choice is to use jhash on the Ethernet header
 */
__be16 vxlan_src_port(__u16 port_min, __u16 port_max, struct sk_buff *skb)
{
	unsigned int range = (port_max - port_min) + 1;
	u32 hash;

	hash = jhash(skb->data, 2 * ETH_ALEN,
		     (__force u32) skb->protocol);

	return htons((((u64) hash * range) >> 32) + port_min);
}
EXPORT_SYMBOL_GPL(vxlan_src_port);

static int handle_offloads(struct sk_buff *skb)
{
	if (skb_is_gso(skb)) {
		int err = skb_unclone(skb, GFP_ATOMIC);
		if (unlikely(err))
			return err;

		skb_shinfo(skb)->gso_type |= SKB_GSO_UDP_TUNNEL;
	} else if (skb->ip_summed != CHECKSUM_PARTIAL)
		skb->ip_summed = CHECKSUM_NONE;

	return 0;
}

int vxlan_xmit_skb(struct net *net, struct vxlan_sock *vs,
		   struct rtable *rt, struct sk_buff *skb,
		   __be32 src, __be32 dst, __u8 tos, __u8 ttl, __be16 df,
		   __be16 src_port, __be16 dst_port, __be32 vni)
{
	struct vxlanhdr *vxh;
	struct udphdr *uh;
	int min_headroom;
	int err;
	const struct iphdr *inner_ip;

	if (!skb->encapsulation) {
		/* skb_reset_inner_headers(skb); */
		skb->encapsulation = 1;
	}

	min_headroom = LL_RESERVED_SPACE(rt->u.dst.dev) + rt->u.dst.header_len
			+ VXLAN_HLEN + sizeof(struct iphdr)
			+ (vlan_tx_tag_present(skb) ? VLAN_HLEN : 0);

	/* Need space for new headers (invalidates iph ptr) */
	err = skb_cow_head(skb, min_headroom);
	if (unlikely(err))
		return err;

	if (vlan_tx_tag_present(skb)) {
		if (WARN_ON(!__vlan_put_tag(skb,
					    vlan_tx_tag_get(skb))))
			return -ENOMEM;

		skb->vlan_tci = 0;
	}

	inner_ip = ip_hdr(skb);

	vxh = (struct vxlanhdr *) __skb_push(skb, sizeof(*vxh));
	vxh->vx_flags = htonl(VXLAN_FLAGS);
	vxh->vx_vni = vni;

	__skb_push(skb, sizeof(*uh));
	skb_reset_transport_header(skb);
	uh = udp_hdr(skb);

	uh->dest = dst_port;
	uh->source = src_port;

	uh->len = htons(skb->len);
	uh->check = 0;

	vxlan_set_owner(vs->sock->sk, skb);

	err = handle_offloads(skb);
	if (err)
		return err;

	return iptunnel_xmit(net, rt, skb, src, dst,
			IPPROTO_UDP, tos, ttl, df, inner_ip);
}
EXPORT_SYMBOL_GPL(vxlan_xmit_skb);

/* Bypass encapsulation if the destination is local */
static void vxlan_encap_bypass(struct sk_buff *skb, struct vxlan_dev *src_vxlan,
			       struct vxlan_dev *dst_vxlan)
{
	struct vxlan_stats *tx_stats = this_cpu_ptr(src_vxlan->stats);
	struct vxlan_stats *rx_stats = this_cpu_ptr(dst_vxlan->stats);

	skb->pkt_type = PACKET_HOST;
	skb->encapsulation = 0;
	skb->dev = dst_vxlan->dev;
	__skb_pull(skb, skb_network_offset(skb));

	if (dst_vxlan->flags & VXLAN_F_LEARN)
		vxlan_snoop(skb->dev, htonl(INADDR_LOOPBACK),
			    eth_hdr(skb)->h_source);

	u64_stats_update_begin(&tx_stats->syncp);
	tx_stats->tx_packets++;
	tx_stats->tx_bytes += skb->len;
	u64_stats_update_end(&tx_stats->syncp);

	if (netif_rx(skb) == NET_RX_SUCCESS) {
		u64_stats_update_begin(&rx_stats->syncp);
		rx_stats->rx_packets++;
		rx_stats->rx_bytes += skb->len;
		u64_stats_update_end(&rx_stats->syncp);
	} else {
		skb->dev->stats.rx_dropped++;
	}
}

static void vxlan_xmit_one(struct sk_buff *skb, struct net_device *dev,
			   struct vxlan_rdst *rdst, bool did_rsc)
{
	struct vxlan_dev *vxlan = netdev_priv(dev);
	struct rtable *rt;
	const struct iphdr *old_iph;
	struct flowi fl4;
	__be32 dst;
	__be16 src_port, dst_port;
	u32 vni;
	__be16 df = 0;
	__u8 tos, ttl;
	int err;

	dst_port = rdst->remote_port ? rdst->remote_port : vxlan->dst_port;
	vni = rdst->remote_vni;
	dst = rdst->remote_ip;

	if (!dst) {
		if (did_rsc) {
			/* short-circuited back to local bridge */
			vxlan_encap_bypass(skb, vxlan, vxlan);
			return;
		}
		goto drop;
	}

	old_iph = ip_hdr(skb);

	ttl = vxlan->ttl;
	if (!ttl && IN_MULTICAST(ntohl(dst)))
		ttl = 1;

	tos = vxlan->tos;
	if (tos == 1)
		tos = ip_tunnel_get_dsfield(old_iph, skb);

	src_port = vxlan_src_port(vxlan->port_min, vxlan->port_max, skb);

	memset(&fl4, 0, sizeof(fl4));
	fl4.oif = rdst->remote_ifindex;
	fl4.proto = IPPROTO_UDP;
	fl4.fl4_dst = dst;
	fl4.fl4_src = vxlan->saddr;
	fl4.fl4_tos = RT_TOS(tos);
	err = ip_route_output_key(dev_net(dev), &rt, &fl4);
	if (err) {
		netdev_dbg(dev, "no route to %pI4\n", &dst);
		dev->stats.tx_carrier_errors++;
		goto tx_error;
	}

	if (rt->u.dst.dev == dev) {
		netdev_dbg(dev, "circular route to %pI4\n", &dst);
		dev->stats.collisions++;
		goto rt_tx_error;
	}

	/* Bypass encapsulation if the destination is local */
	if (rt->rt_flags & RTCF_LOCAL &&
	    !(rt->rt_flags & (RTCF_BROADCAST | RTCF_MULTICAST))) {
		struct vxlan_dev *dst_vxlan;

		ip_rt_put(rt);
		dst_vxlan = vxlan_find_vni(dev_net(dev), vni, dst_port);
		if (!dst_vxlan)
			goto tx_error;
		vxlan_encap_bypass(skb, vxlan, dst_vxlan);
		return;
	}

	tos = ip_tunnel_ecn_encap(tos, old_iph, skb);
	ttl = ttl ? : dst_metric(&rt->u.dst, RTAX_HOPLIMIT);

	err = vxlan_xmit_skb(dev_net(dev), vxlan->vn_sock, rt, skb,
			     fl4.fl4_src, dst, tos, ttl, df,
			     src_port, dst_port, htonl(vni << 8));

	if (err < 0)
		goto rt_tx_error;

	if (err > 0) {
		struct vxlan_stats *stats = this_cpu_ptr(vxlan->stats);

		u64_stats_update_begin(&stats->syncp);
		stats->tx_packets++;
		stats->tx_bytes += err;
		u64_stats_update_end(&stats->syncp);
	} else {
		dev->stats.tx_errors++;
		dev->stats.tx_aborted_errors++;
	}
	return;

drop:
	dev->stats.tx_dropped++;
	goto tx_free;

rt_tx_error:
	ip_rt_put(rt);
tx_error:
	dev->stats.tx_errors++;
tx_free:
	dev_kfree_skb(skb);
}

/* Transmit local packets over Vxlan
 *
 * Outer IP header inherits ECN and DF from inner header.
 * Outer UDP destination is the VXLAN assigned port.
 *           source port is based on hash of flow
 */
static netdev_tx_t vxlan_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct vxlan_dev *vxlan = netdev_priv(dev);
	struct ethhdr *eth;
	bool did_rsc = false;
	struct vxlan_rdst *rdst;
	struct vxlan_fdb *f;

	skb_reset_mac_header(skb);
	eth = eth_hdr(skb);

	if ((vxlan->flags & VXLAN_F_PROXY) && ntohs(eth->h_proto) == ETH_P_ARP)
		return arp_reduce(dev, skb);

	f = vxlan_find_mac(vxlan, eth->h_dest);
	did_rsc = false;

	if (f && (f->flags & NTF_ROUTER) && (vxlan->flags & VXLAN_F_RSC) &&
	    ntohs(eth->h_proto) == ETH_P_IP) {
		did_rsc = route_shortcircuit(dev, skb);
		if (did_rsc)
			f = vxlan_find_mac(vxlan, eth->h_dest);
	}

	if (f == NULL) {
		f = vxlan_find_mac(vxlan, all_zeros_mac);
		if (f == NULL) {
			if ((vxlan->flags & VXLAN_F_L2MISS) &&
			    !is_multicast_ether_addr(eth->h_dest))
				vxlan_fdb_miss(vxlan, eth->h_dest);

			dev->stats.tx_dropped++;
			dev_kfree_skb(skb);
			return NETDEV_TX_OK;
		}
	}

	list_for_each_entry_rcu(rdst, &f->remotes, list) {
		struct sk_buff *skb1;

		skb1 = skb_clone(skb, GFP_ATOMIC);
		if (skb1)
			vxlan_xmit_one(skb1, dev, rdst, did_rsc);
	}

	dev_kfree_skb(skb);
	return NETDEV_TX_OK;
}

/* Walk the forwarding table and purge stale entries */
static void vxlan_cleanup(unsigned long arg)
{
	struct vxlan_dev *vxlan = (struct vxlan_dev *) arg;
	unsigned long next_timer = jiffies + FDB_AGE_INTERVAL;
	unsigned int h;

	if (!netif_running(vxlan->dev))
		return;

	spin_lock_bh(&vxlan->hash_lock);
	for (h = 0; h < FDB_HASH_SIZE; ++h) {
		struct hlist_node *p, *n;
		hlist_for_each_safe(p, n, &vxlan->fdb_head[h]) {
			struct vxlan_fdb *f
				= container_of(p, struct vxlan_fdb, hlist);
			unsigned long timeout;

			if (f->state & NUD_PERMANENT)
				continue;

			timeout = f->used + vxlan->age_interval * HZ;
			if (time_before_eq(timeout, jiffies)) {
				netdev_dbg(vxlan->dev,
					   "garbage collect %pM\n",
					   f->eth_addr);
				f->state = NUD_STALE;
				vxlan_fdb_destroy(vxlan, f);
			} else if (time_before(timeout, next_timer))
				next_timer = timeout;
		}
	}
	spin_unlock_bh(&vxlan->hash_lock);

	mod_timer(&vxlan->age_timer, next_timer);
}

static void vxlan_vs_add_dev(struct vxlan_sock *vs, struct vxlan_dev *vxlan)
{
	__u32 vni = vxlan->default_dst.remote_vni;

	vxlan->vn_sock = vs;
	hlist_add_head_rcu(&vxlan->hlist, vni_head(vs, vni));
}

/* Setup stats when device is created */
static int vxlan_init(struct net_device *dev)
{
	struct vxlan_dev *vxlan = netdev_priv(dev);
	struct vxlan_net *vn = net_generic(dev_net(dev), vxlan_net_id);
	struct vxlan_sock *vs;

	vxlan->stats = alloc_percpu(struct vxlan_stats);
	if (!vxlan->stats)
		return -ENOMEM;

	spin_lock(&vn->sock_lock);
	vs = vxlan_find_sock(dev_net(dev), vxlan->dst_port);
	if (vs) {
		/* If we have a socket with same port already, reuse it */
		atomic_inc(&vs->refcnt);
		vxlan_vs_add_dev(vs, vxlan);
	} else {
		/* otherwise make new socket outside of RTNL */
		dev_hold(dev);
		queue_work(vxlan_wq, &vxlan->sock_work);
	}
	spin_unlock(&vn->sock_lock);

	return 0;
}

static void vxlan_fdb_delete_default(struct vxlan_dev *vxlan)
{
	struct vxlan_fdb *f;

	spin_lock_bh(&vxlan->hash_lock);
	f = __vxlan_find_mac(vxlan, all_zeros_mac);
	if (f)
		vxlan_fdb_destroy(vxlan, f);
	spin_unlock_bh(&vxlan->hash_lock);
}

static void vxlan_uninit(struct net_device *dev)
{
	struct vxlan_dev *vxlan = netdev_priv(dev);
	struct vxlan_sock *vs = vxlan->vn_sock;

	vxlan_fdb_delete_default(vxlan);

	if (vs)
		vxlan_sock_release(vs);
	free_percpu(vxlan->stats);
}

/* Start ageing timer and join group when device is brought up */
static int vxlan_open(struct net_device *dev)
{
	struct vxlan_net *vn = net_generic(dev_net(dev), vxlan_net_id);
	struct vxlan_dev *vxlan = netdev_priv(dev);
	struct vxlan_sock *vs = vxlan->vn_sock;

	/* socket hasn't been created */
	if (!vs)
		return -ENOTCONN;

	if (IN_MULTICAST(ntohl(vxlan->default_dst.remote_ip)) &&
	    vxlan_group_used(vn, vxlan->default_dst.remote_ip)) {
		vxlan_sock_hold(vs);
		dev_hold(dev);
		queue_work(vxlan_wq, &vxlan->igmp_join);
	}

	if (vxlan->age_interval)
		mod_timer(&vxlan->age_timer, jiffies + FDB_AGE_INTERVAL);

	return 0;
}

/* Purge the forwarding table */
static void vxlan_flush(struct vxlan_dev *vxlan)
{
	unsigned int h;

	spin_lock_bh(&vxlan->hash_lock);
	for (h = 0; h < FDB_HASH_SIZE; ++h) {
		struct hlist_node *p, *n;
		hlist_for_each_safe(p, n, &vxlan->fdb_head[h]) {
			struct vxlan_fdb *f
				= container_of(p, struct vxlan_fdb, hlist);
			/* the all_zeros_mac entry is deleted at vxlan_uninit */
			if (!is_zero_ether_addr(f->eth_addr))
				vxlan_fdb_destroy(vxlan, f);
		}
	}
	spin_unlock_bh(&vxlan->hash_lock);
}

/* Cleanup timer and forwarding table on shutdown */
static int vxlan_stop(struct net_device *dev)
{
	struct vxlan_net *vn = net_generic(dev_net(dev), vxlan_net_id);
	struct vxlan_dev *vxlan = netdev_priv(dev);
	struct vxlan_sock *vs = vxlan->vn_sock;

	if (vs && IN_MULTICAST(ntohl(vxlan->default_dst.remote_ip)) &&
	    ! vxlan_group_used(vn, vxlan->default_dst.remote_ip)) {
		vxlan_sock_hold(vs);
		dev_hold(dev);
		queue_work(vxlan_wq, &vxlan->igmp_leave);
	}

	del_timer_sync(&vxlan->age_timer);

	vxlan_flush(vxlan);

	return 0;
}

/* Merge per-cpu statistics */
static struct net_device_stats *vxlan_stats(struct net_device *dev)
{
	struct net_device_stats *stats = &dev->stats;
	struct vxlan_dev *vxlan = netdev_priv(dev);
	struct vxlan_stats tmp, sum = { 0 };
	unsigned int cpu;

	for_each_possible_cpu(cpu) {
		unsigned int start;
		const struct vxlan_stats *stats
			= per_cpu_ptr(vxlan->stats, cpu);

		do {
			start = u64_stats_fetch_begin_bh(&stats->syncp);
			memcpy(&tmp, stats, sizeof(tmp));
		} while (u64_stats_fetch_retry_bh(&stats->syncp, start));

		sum.tx_bytes   += tmp.tx_bytes;
		sum.tx_packets += tmp.tx_packets;
		sum.rx_bytes   += tmp.rx_bytes;
		sum.rx_packets += tmp.rx_packets;
	}

	stats->tx_bytes   = sum.tx_bytes;
	stats->tx_packets = sum.tx_packets;
	stats->rx_bytes   = sum.rx_bytes;
	stats->rx_packets = sum.rx_packets;

	stats->multicast = dev->stats.multicast;
	stats->rx_length_errors = dev->stats.rx_length_errors;
	stats->rx_frame_errors = dev->stats.rx_frame_errors;
	stats->rx_errors = dev->stats.rx_errors;

	stats->tx_dropped = dev->stats.tx_dropped;
	stats->tx_carrier_errors  = dev->stats.tx_carrier_errors;
	stats->tx_aborted_errors  = dev->stats.tx_aborted_errors;
	stats->collisions  = dev->stats.collisions;
	stats->tx_errors = dev->stats.tx_errors;

	return stats;
}

/* Stub, nothing needs to be done. */
static void vxlan_set_multicast_list(struct net_device *dev)
{
}

static const struct net_device_ops vxlan_netdev_ops = {
	.ndo_init		= vxlan_init,
	.ndo_uninit		= vxlan_uninit,
	.ndo_open		= vxlan_open,
	.ndo_stop		= vxlan_stop,
	.ndo_start_xmit		= vxlan_xmit,
	.ndo_get_stats		= vxlan_stats,
	.ndo_set_rx_mode	= vxlan_set_multicast_list,
	.ndo_change_mtu		= eth_change_mtu,
	.ndo_validate_addr	= eth_validate_addr,
	.ndo_set_mac_address	= eth_mac_addr,
};

/* Info for udev, that this is a virtual tunnel endpoint */
static struct device_type vxlan_type = {
	.name = "vxlan",
};

static void vxlan_get_drvinfo(struct net_device *netdev,
			      struct ethtool_drvinfo *drvinfo)
{
	strlcpy(drvinfo->version, VXLAN_VERSION, sizeof(drvinfo->version));
	strlcpy(drvinfo->driver, "vxlan", sizeof(drvinfo->driver));
}

static const struct ethtool_ops vxlan_ethtool_ops = {
	.get_drvinfo	= vxlan_get_drvinfo,
	.get_link	= ethtool_op_get_link,
	.set_tso	= ethtool_op_set_tso,
	.get_tso	= ethtool_op_get_tso,
};

/* Initialize the device structure. */
static void vxlan_setup(struct net_device *dev)
{
	struct vxlan_dev *vxlan = netdev_priv(dev);
	unsigned int h;
	int low, high;

	eth_hw_addr_random(dev);
	ether_setup(dev);
	dev->hard_header_len = ETH_HLEN + VXLAN_HEADROOM;

	dev->netdev_ops = &vxlan_netdev_ops;
	dev->destructor = free_netdev;
	SET_NETDEV_DEVTYPE(dev, &vxlan_type);

	dev->tx_queue_len = 0;
	dev->features	|= NETIF_F_LLTX;
	dev->features	|= NETIF_F_NETNS_LOCAL;
	dev->features	|= NETIF_F_SG | NETIF_F_HW_CSUM;
	dev->features   |= NETIF_F_RXCSUM;
	dev->features   |= NETIF_F_GSO_SOFTWARE;

	dev->vlan_features = dev->features;
	dev->features |= NETIF_F_HW_VLAN_TX | NETIF_F_HW_VLAN_RX;
	dev->priv_flags	&= ~IFF_XMIT_DST_RELEASE;
	netdev_extended(dev)->ext_priv_flags |= IFF_LIVE_ADDR_CHANGE;
	netdev_extended(dev)->ndo_fdb_add = vxlan_fdb_add;
	netdev_extended(dev)->ndo_fdb_del = vxlan_fdb_delete;
	netdev_extended(dev)->ndo_fdb_dump = vxlan_fdb_dump;

	INIT_LIST_HEAD(&vxlan->next);
	spin_lock_init(&vxlan->hash_lock);
	INIT_WORK(&vxlan->igmp_join, vxlan_igmp_join);
	INIT_WORK(&vxlan->igmp_leave, vxlan_igmp_leave);
	INIT_WORK(&vxlan->sock_work, vxlan_sock_work);

	init_timer_deferrable(&vxlan->age_timer);
	vxlan->age_timer.function = vxlan_cleanup;
	vxlan->age_timer.data = (unsigned long) vxlan;

	inet_get_local_port_range(&low, &high);
	vxlan->port_min = low;
	vxlan->port_max = high;
	vxlan->dst_port = htons(vxlan_port);

	vxlan->dev = dev;

	for (h = 0; h < FDB_HASH_SIZE; ++h)
		INIT_HLIST_HEAD(&vxlan->fdb_head[h]);
}

static const struct nla_policy vxlan_policy[IFLA_VXLAN_MAX + 1] = {
	[IFLA_VXLAN_ID]		= { .type = NLA_U32 },
	[IFLA_VXLAN_GROUP]	= { .len = FIELD_SIZEOF(struct iphdr, daddr) },
	[IFLA_VXLAN_LINK]	= { .type = NLA_U32 },
	[IFLA_VXLAN_LOCAL]	= { .len = FIELD_SIZEOF(struct iphdr, saddr) },
	[IFLA_VXLAN_TOS]	= { .type = NLA_U8 },
	[IFLA_VXLAN_TTL]	= { .type = NLA_U8 },
	[IFLA_VXLAN_LEARNING]	= { .type = NLA_U8 },
	[IFLA_VXLAN_AGEING]	= { .type = NLA_U32 },
	[IFLA_VXLAN_LIMIT]	= { .type = NLA_U32 },
	[IFLA_VXLAN_PORT_RANGE] = { .len  = sizeof(struct ifla_vxlan_port_range) },
	[IFLA_VXLAN_PROXY]	= { .type = NLA_U8 },
	[IFLA_VXLAN_RSC]	= { .type = NLA_U8 },
	[IFLA_VXLAN_L2MISS]	= { .type = NLA_U8 },
	[IFLA_VXLAN_L3MISS]	= { .type = NLA_U8 },
	[IFLA_VXLAN_PORT]	= { .type = NLA_U16 },
};

static int vxlan_validate(struct nlattr *tb[], struct nlattr *data[])
{
	if (tb[IFLA_ADDRESS]) {
		if (nla_len(tb[IFLA_ADDRESS]) != ETH_ALEN) {
			pr_debug("invalid link address (not ethernet)\n");
			return -EINVAL;
		}

		if (!is_valid_ether_addr(nla_data(tb[IFLA_ADDRESS]))) {
			pr_debug("invalid all zero ethernet address\n");
			return -EADDRNOTAVAIL;
		}
	}

	if (!data)
		return -EINVAL;

	if (data[IFLA_VXLAN_ID]) {
		__u32 id = nla_get_u32(data[IFLA_VXLAN_ID]);
		if (id >= VXLAN_VID_MASK)
			return -ERANGE;
	}

	if (data[IFLA_VXLAN_PORT_RANGE]) {
		const struct ifla_vxlan_port_range *p
			= nla_data(data[IFLA_VXLAN_PORT_RANGE]);

		if (ntohs(p->high) < ntohs(p->low)) {
			pr_debug("port range %u .. %u not valid\n",
				 ntohs(p->low), ntohs(p->high));
			return -EINVAL;
		}
	}

	return 0;
}

static void vxlan_del_work(struct work_struct *work)
{
	struct vxlan_sock *vs = container_of(work, struct vxlan_sock, del_work);

	sk_release_kernel(vs->sock->sk);
	kfree_rcu(vs, rcu);
}

static struct vxlan_sock *vxlan_socket_create(struct net *net, __be16 port,
					      vxlan_rcv_t *rcv, void *data)
{
	struct vxlan_net *vn = net_generic(net, vxlan_net_id);
	struct vxlan_sock *vs;
	struct sock *sk;
	struct sockaddr_in vxlan_addr = {
		.sin_family = AF_INET,
		.sin_addr.s_addr = htonl(INADDR_ANY),
		.sin_port = port,
	};
	int rc;
	unsigned int h;

	vs = kmalloc(sizeof(*vs), GFP_KERNEL);
	if (!vs) {
		pr_debug("memory alocation failure\n");
		return ERR_PTR(-ENOMEM);
	}

	for (h = 0; h < VNI_HASH_SIZE; ++h)
		INIT_HLIST_HEAD(&vs->vni_list[h]);

	INIT_WORK(&vs->del_work, vxlan_del_work);

	/* Create UDP socket for encapsulation receive. */
	rc = sock_create_kern(AF_INET, SOCK_DGRAM, IPPROTO_UDP, &vs->sock);
	if (rc < 0) {
		pr_debug("UDP socket create failed\n");
		kfree(vs);
		return ERR_PTR(rc);
	}

	/* Put in proper namespace */
	sk = vs->sock->sk;
	sk_change_net(sk, net);

	rc = kernel_bind(vs->sock, (struct sockaddr *) &vxlan_addr,
			 sizeof(vxlan_addr));
	if (rc < 0) {
		pr_debug("bind for UDP socket %pI4:%u (%d)\n",
			 &vxlan_addr.sin_addr, ntohs(vxlan_addr.sin_port), rc);
		sk_release_kernel(sk);
		kfree(vs);
		return ERR_PTR(rc);
	}
	atomic_set(&vs->refcnt, 1);
	vs->rcv = rcv;
	vs->data = data;

	/* Disable multicast loopback */
	inet_sk(sk)->mc_loop = 0;
	spin_lock(&vn->sock_lock);
	hlist_add_head_rcu(&vs->hlist, vs_head(net, port));
	spin_unlock(&vn->sock_lock);

	/* Mark socket as an encapsulation socket. */
	udp_sk(sk)->encap_type = 1;
	udp_sk(sk)->encap_rcv = vxlan_udp_encap_recv;
	return vs;
}

struct vxlan_sock *vxlan_sock_add(struct net *net, __be16 port,
				  vxlan_rcv_t *rcv, void *data,
				  bool no_share)
{
	struct vxlan_net *vn = net_generic(net, vxlan_net_id);
	struct vxlan_sock *vs;

	vs = vxlan_socket_create(net, port, rcv, data);
	if (!IS_ERR(vs))
		return vs;

	if (no_share)	/* Return error if sharing is not allowed. */
		return vs;

	spin_lock(&vn->sock_lock);
	vs = vxlan_find_sock(net, port);
	if (vs) {
		if (vs->rcv == rcv)
			atomic_inc(&vs->refcnt);
		else
			vs = ERR_PTR(-EBUSY);
	}
	spin_unlock(&vn->sock_lock);

	if (!vs)
		vs = ERR_PTR(-EINVAL);

	return vs;
}
EXPORT_SYMBOL_GPL(vxlan_sock_add);

/* Scheduled at device creation to bind to a socket */
static void vxlan_sock_work(struct work_struct *work)
{
	struct vxlan_dev *vxlan = container_of(work, struct vxlan_dev, sock_work);
	struct net *net = dev_net(vxlan->dev);
	struct vxlan_net *vn = net_generic(net, vxlan_net_id);
	__be16 port = vxlan->dst_port;
	struct vxlan_sock *nvs;

	nvs = vxlan_sock_add(net, port, vxlan_rcv, NULL, false);
	spin_lock(&vn->sock_lock);
	if (!IS_ERR(nvs))
		vxlan_vs_add_dev(nvs, vxlan);
	spin_unlock(&vn->sock_lock);

	dev_put(vxlan->dev);
}

static int vxlan_newlink(struct net_device *dev,
			 struct nlattr *tb[], struct nlattr *data[])
{
	struct net *net = dev_net(dev);
	struct vxlan_net *vn = net_generic(net, vxlan_net_id);
	struct vxlan_dev *vxlan = netdev_priv(dev);
	struct vxlan_rdst *dst = &vxlan->default_dst;
	__u32 vni;
	int err;

	if (!data[IFLA_VXLAN_ID])
		return -EINVAL;

	vni = nla_get_u32(data[IFLA_VXLAN_ID]);
	dst->remote_vni = vni;

	if (data[IFLA_VXLAN_GROUP])
		dst->remote_ip = nla_get_be32(data[IFLA_VXLAN_GROUP]);

	if (data[IFLA_VXLAN_LOCAL])
		vxlan->saddr = nla_get_be32(data[IFLA_VXLAN_LOCAL]);

	if (data[IFLA_VXLAN_LINK] &&
	    (dst->remote_ifindex = nla_get_u32(data[IFLA_VXLAN_LINK]))) {
		struct net_device *lowerdev
			 = __dev_get_by_index(net, dst->remote_ifindex);

		if (!lowerdev) {
			pr_info("ifindex %d does not exist\n", dst->remote_ifindex);
			return -ENODEV;
		}

		if (!tb[IFLA_MTU])
			dev->mtu = lowerdev->mtu - VXLAN_HEADROOM;

		/* update header length based on lower device */
		dev->hard_header_len = lowerdev->hard_header_len +
				       VXLAN_HEADROOM;
	}

	if (data[IFLA_VXLAN_TOS])
		vxlan->tos  = nla_get_u8(data[IFLA_VXLAN_TOS]);

	if (data[IFLA_VXLAN_TTL])
		vxlan->ttl = nla_get_u8(data[IFLA_VXLAN_TTL]);

	if (!data[IFLA_VXLAN_LEARNING] || nla_get_u8(data[IFLA_VXLAN_LEARNING]))
		vxlan->flags |= VXLAN_F_LEARN;

	if (data[IFLA_VXLAN_AGEING])
		vxlan->age_interval = nla_get_u32(data[IFLA_VXLAN_AGEING]);
	else
		vxlan->age_interval = FDB_AGE_DEFAULT;

	if (data[IFLA_VXLAN_PROXY] && nla_get_u8(data[IFLA_VXLAN_PROXY]))
		vxlan->flags |= VXLAN_F_PROXY;

	if (data[IFLA_VXLAN_RSC] && nla_get_u8(data[IFLA_VXLAN_RSC]))
		vxlan->flags |= VXLAN_F_RSC;

	if (data[IFLA_VXLAN_L2MISS] && nla_get_u8(data[IFLA_VXLAN_L2MISS]))
		vxlan->flags |= VXLAN_F_L2MISS;

	if (data[IFLA_VXLAN_L3MISS] && nla_get_u8(data[IFLA_VXLAN_L3MISS]))
		vxlan->flags |= VXLAN_F_L3MISS;

	if (data[IFLA_VXLAN_LIMIT])
		vxlan->addrmax = nla_get_u32(data[IFLA_VXLAN_LIMIT]);

	if (data[IFLA_VXLAN_PORT_RANGE]) {
		const struct ifla_vxlan_port_range *p
			= nla_data(data[IFLA_VXLAN_PORT_RANGE]);
		vxlan->port_min = ntohs(p->low);
		vxlan->port_max = ntohs(p->high);
	}

	if (data[IFLA_VXLAN_PORT])
		vxlan->dst_port = nla_get_be16(data[IFLA_VXLAN_PORT]);

	if (vxlan_find_vni(net, vni, vxlan->dst_port)) {
		pr_info("duplicate VNI %u\n", vni);
		return -EEXIST;
	}

	SET_ETHTOOL_OPS(dev, &vxlan_ethtool_ops);

	/* create an fdb entry for a valid default destination */
	if (vxlan->default_dst.remote_ip != htonl(INADDR_ANY)) {
		err = vxlan_fdb_create(vxlan, all_zeros_mac,
				       vxlan->default_dst.remote_ip,
				       NUD_REACHABLE|NUD_PERMANENT,
				       NLM_F_EXCL|NLM_F_CREATE,
				       vxlan->dst_port,
				       vxlan->default_dst.remote_vni,
				       vxlan->default_dst.remote_ifindex,
				       NTF_SELF);
		if (err)
			return err;
	}

	err = register_netdevice(dev);
	if (err) {
		vxlan_fdb_delete_default(vxlan);
		return err;
	}

	list_add(&vxlan->next, &vn->vxlan_list);

	return 0;
}

static void vxlan_dellink(struct net_device *dev)
{
	struct vxlan_net *vn = net_generic(dev_net(dev), vxlan_net_id);
	struct vxlan_dev *vxlan = netdev_priv(dev);

	spin_lock(&vn->sock_lock);
	if (!hlist_unhashed(&vxlan->hlist))
		hlist_del_rcu(&vxlan->hlist);
	spin_unlock(&vn->sock_lock);

	list_del(&vxlan->next);
	unregister_netdevice(dev);
}

static size_t vxlan_get_size(const struct net_device *dev)
{

	return nla_total_size(sizeof(__u32)) +	/* IFLA_VXLAN_ID */
		nla_total_size(sizeof(__be32)) +/* IFLA_VXLAN_GROUP */
		nla_total_size(sizeof(__u32)) +	/* IFLA_VXLAN_LINK */
		nla_total_size(sizeof(__be32))+	/* IFLA_VXLAN_LOCAL */
		nla_total_size(sizeof(__u8)) +	/* IFLA_VXLAN_TTL */
		nla_total_size(sizeof(__u8)) +	/* IFLA_VXLAN_TOS */
		nla_total_size(sizeof(__u8)) +	/* IFLA_VXLAN_LEARNING */
		nla_total_size(sizeof(__u8)) +	/* IFLA_VXLAN_PROXY */
		nla_total_size(sizeof(__u8)) +	/* IFLA_VXLAN_RSC */
		nla_total_size(sizeof(__u8)) +	/* IFLA_VXLAN_L2MISS */
		nla_total_size(sizeof(__u8)) +	/* IFLA_VXLAN_L3MISS */
		nla_total_size(sizeof(__u32)) +	/* IFLA_VXLAN_AGEING */
		nla_total_size(sizeof(__u32)) +	/* IFLA_VXLAN_LIMIT */
		nla_total_size(sizeof(struct ifla_vxlan_port_range)) +
		nla_total_size(sizeof(__be16))+ /* IFLA_VXLAN_PORT */
		0;
}

static int vxlan_fill_info(struct sk_buff *skb, const struct net_device *dev)
{
	const struct vxlan_dev *vxlan = netdev_priv(dev);
	const struct vxlan_rdst *dst = &vxlan->default_dst;
	struct ifla_vxlan_port_range ports = {
		.low =  htons(vxlan->port_min),
		.high = htons(vxlan->port_max),
	};

	if (nla_put_u32(skb, IFLA_VXLAN_ID, dst->remote_vni))
		goto nla_put_failure;

	if (dst->remote_ip && nla_put_be32(skb, IFLA_VXLAN_GROUP, dst->remote_ip))
		goto nla_put_failure;

	if (dst->remote_ifindex && nla_put_u32(skb, IFLA_VXLAN_LINK, dst->remote_ifindex))
		goto nla_put_failure;

	if (vxlan->saddr && nla_put_be32(skb, IFLA_VXLAN_LOCAL, vxlan->saddr))
		goto nla_put_failure;

	if (nla_put_u8(skb, IFLA_VXLAN_TTL, vxlan->ttl) ||
	    nla_put_u8(skb, IFLA_VXLAN_TOS, vxlan->tos) ||
	    nla_put_u8(skb, IFLA_VXLAN_LEARNING,
			!!(vxlan->flags & VXLAN_F_LEARN)) ||
	    nla_put_u8(skb, IFLA_VXLAN_PROXY,
			!!(vxlan->flags & VXLAN_F_PROXY)) ||
	    nla_put_u8(skb, IFLA_VXLAN_RSC, !!(vxlan->flags & VXLAN_F_RSC)) ||
	    nla_put_u8(skb, IFLA_VXLAN_L2MISS,
			!!(vxlan->flags & VXLAN_F_L2MISS)) ||
	    nla_put_u8(skb, IFLA_VXLAN_L3MISS,
			!!(vxlan->flags & VXLAN_F_L3MISS)) ||
	    nla_put_u32(skb, IFLA_VXLAN_AGEING, vxlan->age_interval) ||
	    nla_put_u32(skb, IFLA_VXLAN_LIMIT, vxlan->addrmax) ||
	    nla_put_be16(skb, IFLA_VXLAN_PORT, vxlan->dst_port))
		goto nla_put_failure;

	if (nla_put(skb, IFLA_VXLAN_PORT_RANGE, sizeof(ports), &ports))
		goto nla_put_failure;

	return 0;

nla_put_failure:
	return -EMSGSIZE;
}

static struct rtnl_link_ops vxlan_link_ops __read_mostly = {
	.kind		= "vxlan",
	.maxtype	= IFLA_VXLAN_MAX,
	.policy		= vxlan_policy,
	.priv_size	= sizeof(struct vxlan_dev),
	.setup		= vxlan_setup,
	.validate	= vxlan_validate,
	.newlink	= vxlan_newlink,
	.dellink	= vxlan_dellink,
	.get_size	= vxlan_get_size,
	.fill_info	= vxlan_fill_info,
};

static __net_init int vxlan_init_net(struct net *net)
{
	struct vxlan_net *vn;
	int rc;
	unsigned int h;

	vn = kzalloc(sizeof(struct vxlan_net), GFP_KERNEL);
	if (vn == NULL)
		return -ENOMEM;

	rc = net_assign_generic(net, vxlan_net_id, vn);
	if (rc < 0) {
		kfree(vn);
		return rc;
	}

	INIT_LIST_HEAD(&vn->vxlan_list);
	spin_lock_init(&vn->sock_lock);

	for (h = 0; h < PORT_HASH_SIZE; ++h)
		INIT_HLIST_HEAD(&vn->sock_list[h]);

	return 0;
}

static __net_exit void vxlan_exit_net(struct net *net)
{
	struct vxlan_net *vn = net_generic(net, vxlan_net_id);
	struct vxlan_dev *vxlan;
	LIST_HEAD(list);

	rtnl_lock();
	list_for_each_entry(vxlan, &vn->vxlan_list, next)
		unregister_netdevice_queue(vxlan->dev, &list);
	unregister_netdevice_many(&list);
	rtnl_unlock();
	kfree(vn);
}

static struct pernet_operations vxlan_net_ops = {
	.init = vxlan_init_net,
	.exit = vxlan_exit_net,
};

static int __init vxlan_init_module(void)
{
	int rc;

	vxlan_wq = create_singlethread_workqueue("vxlan");
	if (!vxlan_wq)
		return -ENOMEM;

	get_random_bytes(&vxlan_salt, sizeof(vxlan_salt));

	rc = register_pernet_gen_device(&vxlan_net_id, &vxlan_net_ops);
	if (rc)
		goto out1;

	rc = rtnl_link_register(&vxlan_link_ops);
	if (rc)
		goto out2;

	return 0;

out2:
	unregister_pernet_gen_device(vxlan_net_id, &vxlan_net_ops);
out1:
	destroy_workqueue(vxlan_wq);
	return rc;
}
late_initcall(vxlan_init_module);

static void __exit vxlan_cleanup_module(void)
{
	rtnl_link_unregister(&vxlan_link_ops);
	destroy_workqueue(vxlan_wq);
	unregister_pernet_gen_device(vxlan_net_id, &vxlan_net_ops);
	rcu_barrier();
}
module_exit(vxlan_cleanup_module);

MODULE_LICENSE("GPL");
MODULE_VERSION(VXLAN_VERSION);
MODULE_AUTHOR("Stephen Hemminger <stephen@networkplumber.org>");
MODULE_ALIAS_RTNL_LINK("vxlan");
