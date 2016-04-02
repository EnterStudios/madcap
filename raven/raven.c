/*
 * raven is a madcap capable dummy driver.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/rculist.h>
#include <linux/hash.h>
#include <linux/rwlock.h>
#include <linux/etherdevice.h>
#include <net/net_namespace.h>
#include <net/rtnetlink.h>
#include <net/ip_tunnels.h>

#include <madcap.h>
#include <raven.h>

#ifndef DEBUG
#define DEBUG
#endif

/* common prefix used by pr_<> macros */
#undef pr_fmt
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt "\n"

#ifdef DEBUG
#undef pr_debug
#define pr_debug(fmt, ...) \
        printk(KERN_INFO "%s: " pr_fmt(fmt), __func__, ##__VA_ARGS__)
#endif


#define RAVEN_VERSION	"0.0.0"
MODULE_VERSION (RAVEN_VERSION);
MODULE_LICENSE ("GPL");
MODULE_AUTHOR ("upa@haeena.net");
MODULE_DESCRIPTION ("raven, madcap capable dummy driver");
MODULE_ALIAS_RTNL_LINK ("raven");

static int drop_mode __read_mostly = 0;
module_param_named (drop_mode, drop_mode, int, 0444);
MODULE_PARM_DESC (drop_mode, "if 1, tx packet is dropped immediately.");

static int madcap_enable __read_mostly = 0;
module_param_named (madcap_enable, madcap_enable, int, 0444);
MODULE_PARM_DESC (madcap_enable, "if 1, raven_proc_read returns TX path clock "
		  "on madcap/raven offloaded version.");

static u32 raven_salt __read_mostly;


struct raven_table {
	struct hlist_node	hlist;	/* raven_dev->raven_table[] */
	struct rcu_head		rcu;
	struct net_device	*dev;
	unsigned long		updated;	/* jiffies */

	struct madcap_obj_entry	oe;
};

struct raven_dev {
	struct list_head	list;
	struct rcu_head		rcu;
	struct net_device	*dev;

	struct net_device	*vdev;	/* overlay virtual device acquiring 
					 * this raven device */
	struct net_device	*pdev;	/* physicl device to xmit encapsulated
					 * packet */

#define RAVEN_HASH_BITS	8
#define RAVEN_HASH_SIZE	(1 << RAVEN_HASH_BITS)
	struct hlist_head	raven_table[RAVEN_HASH_SIZE]; /* hash table */
	rwlock_t		lock;	/* table lock */

	struct madcap_obj_udp	 ou;	/* enable udp encap */
	struct madcap_obj_config oc;	/* offset and length */
};


#ifdef OVBENCH
#include <linux/ovbench.h>

/* ovbench recent packet timestamp information. */
static __u8	ovbench_type;
static __u8	ovbench_encaped;
static __u64	ovbench_timestamp[OVBENCH_TIMESTAMPNUM];

static int	proc_red = 0;	/* first read, 0. then 1. */

#define ts(start, end) (end - start)

static inline void
copy_ovbench_params (struct sk_buff *skb, struct raven_dev *rdev)
{
	int n;

	if (SKB_OVBENCH (skb)) {
		ovbench_type = skb->ovbench_type;
		ovbench_encaped = skb->ovbench_encaped;
		for (n = 0; n < OVBENCH_TIMESTAMPNUM; n++)
			ovbench_timestamp[n] = skb->ovbench_timestamp[n];
	}
}

static ssize_t
raven_proc_read_madcap_disabled (struct file *fp, char __user *buf,
				 size_t size, loff_t *off)
{
	char line[512];

	if (proc_red) {
		proc_red = 0;
		return 0;
	} else
		proc_red = 1;

	switch (ovbench_type) {
	case OVTYPE_NOENCAP :
		snprintf (line, sizeof (line),
			  "encap:             noencap\n"
			  "inner-tx:          %llu\n"
			  "total-tx:          %llu\n",
			  ts (ip_local_out_sk_in, raven_xmit_in),
			  ts (ip_local_out_sk_in, raven_xmit_in)
			);
		break;

	case OVTYPE_IPIP :
		snprintf (line, sizeof (line),
			  "encap:             ipip\n"
			  "inner-tx:          %llu\n"
			  "protocol-path:     %llu\n"
			  "routing-lookup:    %llu\n"
			  "build-outer-ip:    %llu\n"
			  "outer-tx:          %llu\n"
			  "total-tx:          %llu\n",
			  ts (ip_local_out_sk_in, ipip_tunnel_xmit_in),
			  ts (ipip_tunnel_xmit_in, iptunnel_xmit_in),
			  ts (ip_routing_start, ip_routing_end),
			  ts (iptunnel_xmit_in, ip_local_out_sk_in_encaped),
			  ts (ip_local_out_sk_in_encaped, raven_xmit_in),
			  ts (ip_local_out_sk_in, raven_xmit_in)
			);

		break;

	case OVTYPE_GRE :
		snprintf (line, sizeof (line),
			  "encap:             gre\n"
			  "inner-tx:          %llu\n"
			  "protocol-path:     %llu\n"
			  "build-gre:         %llu\n"
			  "routing-lookup:    %llu\n"
			  "build-outer-ip:    %llu\n"
			  "outer-tx:          %llu\n"
			  "total-tx:          %llu\n",
			  ts (ip_local_out_sk_in, ipgre_xmit_in),
			  ts (ipgre_xmit_in, iptunnel_xmit_in),
			  ts (gre_xmit_in, gre_encap_end),
			  ts (ip_routing_start, ip_routing_end),
			  ts (iptunnel_xmit_in, ip_local_out_sk_in_encaped),
			  ts (ip_local_out_sk_in_encaped, raven_xmit_in),
			  ts (ip_local_out_sk_in, raven_xmit_in)
			);
		break;
	case OVTYPE_GRETAP :
		snprintf (line, sizeof (line),
			  "encap:             gretap\n"
			  "inner-tx:          %llu\n"
			  "protocol-path:     %llu\n"
			  "build-gre:         %llu\n"
			  "routing-lookup:    %llu\n"
			  "build-outer-ip:    %llu\n"
			  "outer-tx:          %llu\n"
			  "total-tx:          %llu\n",
			  ts (ip_local_out_sk_in, gre_tap_xmit_in),
			  ts (gre_tap_xmit_in, iptunnel_xmit_in),
			  ts (gre_xmit_in, gre_encap_end),
			  ts (ip_routing_start, ip_routing_end),
			  ts (iptunnel_xmit_in, ip_local_out_sk_in_encaped),
			  ts (ip_local_out_sk_in_encaped, raven_xmit_in),
			  ts (ip_local_out_sk_in, raven_xmit_in)
			);
		break;
	case OVTYPE_VXLAN :
		snprintf (line, sizeof (line),
			  "encap:                 vxlan\n"
			  "inner-tx:              %llu\n"
			  "protocol-path:         %llu\n"
			  "routing-lookup:        %llu\n"
			  "build-vxlan:           %llu\n"
			  "build-udp:             %llu\n"
			  "build-outer-ip:        %llu\n"
			  "outer-tx:              %llu\n"
			  "total-tx:              %llu\n",
			  ts (ip_local_out_sk_in, vxlan_xmit_in),
			  ts (vxlan_xmit_in, iptunnel_xmit_in),
			  ts (ip_routing_start, ip_routing_end),
			  ts (vxlan_xmit_skb_in, udp_tunnel_xmit_skb_in),
			  ts (udp_tunnel_xmit_skb_in, iptunnel_xmit_in),
			  ts (iptunnel_xmit_in, ip_local_out_sk_in_encaped),
			  ts (ip_local_out_sk_in_encaped, raven_xmit_in),
			  ts (ip_local_out_sk_in, raven_xmit_in)
			);
		break;
	case OVTYPE_NSH :
		snprintf (line, sizeof (line),
			  "encap:                 nsh\n"
			  "inner-tx:              %llu\n"
			  "protocol-path:         %llu\n"
			  "build-nsh:             %llu\n"
			  "routing-lookup:        %llu\n"
			  "build-vxlan:           %llu\n"
			  "build-udp:             %llu\n"
			  "build-outer-ip:        %llu\n"
			  "outer-tx:              %llu\n"
			  "total-tx:              %llu\n",
			  ts (ip_local_out_sk_in, nsh_xmit_in),
			  ts (nsh_xmit_in, iptunnel_xmit_in),
			  ts (nsh_xmit_lookup_end, nsh_xmit_vxlan_in),
			  ts (ip_routing_start, ip_routing_end),
			  ts (nsh_xmit_vxlan_skb_in, udp_tunnel_xmit_skb_in),
			  ts (udp_tunnel_xmit_skb_in, iptunnel_xmit_in),
			  ts (iptunnel_xmit_in, ip_local_out_sk_in_encaped),
			  ts (ip_local_out_sk_in_encaped, raven_xmit_in),
			  ts (ip_local_out_sk_in, raven_xmit_in)
			);
		break;
	default :
		snprintf (line, sizeof (line),
			  "unknown-ovbench-type %u\n", ovbench_type);
		break;
	}

	if (copy_to_user (buf, line, strlen (line))) {
		pr_debug ("copy_to_user failed");
		return -EFAULT;
	}

	return strlen (line);
}

static ssize_t
raven_proc_read_madcap_enabled (struct file *fp, char __user *buf,
				size_t size, loff_t *off)
{
	char line[256];

	if (proc_red) {
		proc_red = 0;
		return 0;
	} else
		proc_red = 1;

	/*
	 * dev_queue_xmit_in is overwritten in
	 * - 1st: enqueueing packet to protocol pseudo device.
	 * - 2nd: enqueueing packet to underlay raven device.
	 * because skb->ovbench_encaped is set to 1 by iptunnel_xmit,
	 * but in madcap mode, packet does not across iptunnel_xmit.
	 * As a result, dev_queue_xmit_in has final timestamp when packet
	 * is enqueued to raven device by madcap_queue_xmit().
	 */
	switch (ovbench_type) {
	case OVTYPE_NOENCAP :
		snprintf (line, sizeof (line),
			  "encap:            noencap\n"
			  "inner-tx:         %llu\n"
			  "total-tx:         %llu\n",
			  ts (ip_local_out_sk_in, raven_xmit_in),
			  ts (ip_local_out_sk_in, raven_xmit_in)
			);
		break;

	case OVTYPE_IPIP :
		snprintf (line, sizeof (line),
			  "encap:             ipip\n"
			  "inner-tx:          %llu\n"
			  "protocol-path:     %llu\n"
			  "outer-tx:          %llu\n"
			  "total-tx:          %llu\n",
			  ts (ip_local_out_sk_in, ipip_tunnel_xmit_in),
			  ts (ipip_tunnel_xmit_in, dev_queue_xmit_in),
			  ts (dev_queue_xmit_in, raven_xmit_in),
			  ts (ip_local_out_sk_in, raven_xmit_in)
			);

		break;

	case OVTYPE_GRE :
		snprintf (line, sizeof (line),
			  "encap:             gre\n"
			  "inner-tx:          %llu\n"
			  "protocol-path:     %llu\n"
			  "build-gre:         %llu\n"
			  "outer-tx:          %llu\n"
			  "total-tx:          %llu\n",
			  ts (ip_local_out_sk_in, ipgre_xmit_in),
			  ts (ipgre_xmit_in, dev_queue_xmit_in),
			  ts (gre_xmit_in, gre_encap_end),
			  ts (dev_queue_xmit_in, raven_xmit_in),
			  ts (ip_local_out_sk_in, raven_xmit_in)
			);
		break;
	case OVTYPE_GRETAP :
		snprintf (line, sizeof (line),
			  "encap:             gretap\n"
			  "inner-tx:          %llu\n"
			  "protocol-path:     %llu\n"
			  "build-gre:         %llu\n"
			  "outer-tx:          %llu\n"
			  "total-tx:          %llu\n",
			  ts (ip_local_out_sk_in, gre_tap_xmit_in),
			  ts (gre_tap_xmit_in, dev_queue_xmit_in),
			  ts (gre_xmit_in, gre_encap_end),
			  ts (dev_queue_xmit_in, raven_xmit_in),
			  ts (ip_local_out_sk_in, raven_xmit_in)
			);
		break;
	case OVTYPE_VXLAN :
		/* XXX: in madcap mode, xmit path is,
		 * vxlan_xmit -> vxlan_xmit_madcap (build vxlan hdr) -> 
		 * madcap_queue_xmit.
		 * vxlan_xmit only does protocol dpened process.
		 * skb_reset_mac_header, ARP reduce etc.
		 *
		 * vxlan_xmit_madcap_in is same field of vxlan_xmit_skb_in.
		 */
		snprintf (line, sizeof (line),
			  "encap:                 vxlan\n"
			  "inner-tx:              %llu\n"
			  "protocol-path:         %llu\n"
			  "build-vxlan:           %llu\n"
			  "outer-tx:              %llu\n"
			  "total-tx:              %llu\n",
			  ts (ip_local_out_sk_in, vxlan_xmit_in),
			  ts (vxlan_xmit_in, dev_queue_xmit_in),
			  ts (vxlan_xmit_skb_in, dev_queue_xmit_in),
			  ts (dev_queue_xmit_in, raven_xmit_in),
			  ts (ip_local_out_sk_in, raven_xmit_in)
			);
		break;
	case OVTYPE_NSH :
		/* XXX: in madcap mode, xmit path is,
		 * nsh_xmit (lookup, build nsh)
		 * -> nsh_xmit_vxlan_madcap (build vxlan header).
		 *
		 * nsh_xmit_vxlan_madcap_in is same field of nsh_xmit_vxlan.
		 */
		snprintf (line, sizeof (line),
			  "encap:                 nsh\n"
			  "inner-tx:              %llu\n"
			  "protocol-path:         %llu\n"
			  "build-nsh:             %llu\n"
			  "build-vxlan:           %llu\n"
			  "outer-tx:              %llu\n"
			  "total-tx:              %llu\n",
			  ts (ip_local_out_sk_in, nsh_xmit_in),
			  ts (nsh_xmit_in, dev_queue_xmit_in),
			  ts (nsh_xmit_lookup_end, nsh_xmit_vxlan_in),
			  ts (nsh_xmit_vxlan_in, dev_queue_xmit_in),
			  ts (dev_queue_xmit_in, raven_xmit_in),
			  ts (ip_local_out_sk_in, raven_xmit_in)
			);
		break;
	default :
		snprintf (line, sizeof (line),
			  "unknown-ovbench-type %u\n", ovbench_type);
		break;
	}

	if (copy_to_user (buf, line, strlen (line))) {
		pr_debug ("copy_to_user failed");
		return -EFAULT;
	}

	return strlen (line);
}

static ssize_t
raven_proc_read (struct file *fp, char __user *buf, size_t size, loff_t *off)
{
	if (madcap_enable)
		return raven_proc_read_madcap_enabled (fp, buf, size, off);
	else
		return raven_proc_read_madcap_disabled (fp, buf, size, off);
}

static const struct file_operations raven_file_fops = {
	.owner	= THIS_MODULE,
	.read	= raven_proc_read,
};
#endif



struct raven_net {
	struct list_head	dev_list;	/* per netns device list */
};

static int raven_net_id;

static inline struct hlist_head *
raven_table_head (struct raven_dev *rdev, u64 key)
{
	return &rdev->raven_table[hash_64 (key, RAVEN_HASH_BITS)];
}

struct raven_table *
raven_table_add (struct raven_dev *rdev, struct madcap_obj_entry *oe)
{
	struct raven_table *rt;

	rt = (struct raven_table *) kmalloc (sizeof (*rt), GFP_KERNEL);

	if (!rt)
		return NULL;

	memset (rt, 0, sizeof (*rt));

	rt->dev = rdev->dev;
	rt->updated = jiffies;
	rt->oe = *oe;

	hlist_add_head_rcu (&rt->hlist, raven_table_head (rdev, oe->id));

	return rt;
}

static void
raven_table_free (struct rcu_head *head)
{
	struct raven_table *rt = container_of (head, struct raven_table, rcu);
	kfree(rt);
}

static void
raven_table_delete (struct raven_table *rt)
{
	hlist_del_rcu (&rt->hlist);
	call_rcu (&rt->rcu, raven_table_free);
}

static void
raven_table_destroy (struct raven_dev *rdev)
{
	unsigned int n;

	for (n = 0; n < RAVEN_HASH_SIZE; n++) {
		struct hlist_node *ptr, *tmp;

		hlist_for_each_safe (ptr, tmp, &rdev->raven_table[n]) {
			struct raven_table *rt;

			rt = container_of (ptr, struct raven_table, hlist);
			raven_table_delete (rt);
		}
	}
}

static struct raven_table *
raven_table_find (struct raven_dev *rdev, u64 id)
{
	struct hlist_head *head = raven_table_head (rdev, id);
	struct raven_table *rt;

	hlist_for_each_entry_rcu (rt, head, hlist) {
		if (id == rt->oe.id)
			return rt;
	}

	return NULL;
}

static int
raven_acquire_dev (struct net_device *dev, struct net_device *vdev)
{
	struct raven_dev *rdev = netdev_priv (dev);

	if (rdev->vdev) {
		pr_debug ("%s: %s is already acquired by %s", __func__,
			  dev->name, rdev->vdev->name);
		return -EINVAL;
	}

	rdev->vdev = vdev;
	/* XXX: start dev? cleanup dev? */

	return 0;
}

static int
raven_release_dev (struct net_device *dev, struct net_device *vdev)
{
	struct raven_dev *rdev = netdev_priv (dev);

	if (rdev->vdev != vdev)
		return -EINVAL;

	rdev->vdev = NULL;
	/* stop dev? */

	return 0;
}

static int
raven_llt_cfg (struct net_device *dev, struct madcap_obj *obj)
{
	struct raven_dev *rdev = netdev_priv (dev);
	struct madcap_obj_config *oc = MADCAP_OBJ_CONFIG (obj);

	if (memcmp (oc, &rdev->oc, sizeof (*oc)) != 0) {
		/* offset or length is changed. drop all table entry. */
		write_lock_bh (&rdev->lock);
		raven_table_destroy (rdev);
		rdev->oc = *oc;
		write_unlock_bh (&rdev->lock);
	}

	return 0;
}

static struct madcap_obj *
raven_llt_config_get (struct net_device *dev)
{
	struct raven_dev *rdev = netdev_priv (dev);
	return MADCAP_OBJ (rdev->oc);
}

static int
raven_llt_entry_add (struct net_device *dev, struct madcap_obj *obj)
{
	struct raven_dev *rdev = netdev_priv (dev);
	struct madcap_obj_entry *obj_ent = MADCAP_OBJ_ENTRY (obj);
	struct raven_table *rt;

	rt = raven_table_add (rdev, obj_ent);
	if (!rt)
		return -ENOMEM;

	return 0;
}

static int
raven_llt_entry_del (struct net_device *dev, struct madcap_obj *obj)
{
	struct raven_table *rt;
	struct raven_dev *rdev = netdev_priv (dev);
	struct madcap_obj_entry *obj_ent = MADCAP_OBJ_ENTRY (obj);

	rt = raven_table_find (rdev, obj_ent->id);

	if (!rt)
		return -ENOENT;

	raven_table_delete (rt);

	return 0;
}

static struct madcap_obj *
raven_llt_entry_dump (struct net_device *dev, struct netlink_callback *cb)
{
	int idx, cnt;
	unsigned int n;
	struct raven_dev *rdev = netdev_priv (dev);
	struct raven_table *rt;
	struct madcap_obj_entry *oe;

	idx = cb->args[0];
	oe = NULL;

	for (n = 0, cnt = 0; n < RAVEN_HASH_SIZE; n++) {
		hlist_for_each_entry_rcu (rt, &rdev->raven_table[n], hlist) {
			if (idx > cnt) {
				cnt++;
				continue;
			}

			oe = &rt->oe;
			goto out;
		}
	}

out:
	cb->args[0] = cnt + 1;
	return MADCAP_OBJ (*oe);
}

static int
raven_udp_cfg (struct net_device *dev, struct madcap_obj *obj)
{
	struct madcap_obj_udp *ou;
	struct raven_dev *rdev = netdev_priv (dev);

	ou = MADCAP_OBJ_UDP (obj);

	rdev->ou = *ou;
	return 0;
}

static struct madcap_obj *
raven_udp_config_get (struct net_device *dev)
{
	struct raven_dev *rdev = netdev_priv (dev);
	return &rdev->ou.obj;
}

static struct madcap_ops raven_madcap_ops = {
	.mco_acquire_dev	= raven_acquire_dev,
	.mco_release_dev	= raven_release_dev,
	.mco_llt_cfg		= raven_llt_cfg,
	.mco_llt_config_get	= raven_llt_config_get,
	.mco_llt_entry_add	= raven_llt_entry_add,
	.mco_llt_entry_del	= raven_llt_entry_del,
	.mco_llt_entry_dump	= raven_llt_entry_dump,
	.mco_udp_cfg		= raven_udp_cfg,
	.mco_udp_config_get	= raven_udp_config_get,
};


static inline __u64
extract_id_from_packet (struct sk_buff *skb, struct madcap_obj_config *oc)
{
	int i;
	__u64 id;

	id = *((__u64 *)(skb->data + oc->offset));

	/* XXX: why id >>= (64 - oc->length) does not work? */
	for (i = 0; i < (64 - oc->length); i++)
		id >>= 1;

	return id;
}

static netdev_tx_t
raven_xmit (struct sk_buff *skb, struct net_device *dev)
{
	/* As a dummy driver for measurement, raven device updates
	 * counters and drops packet immediately.
	 * 
	 * ToDo:
	 * emulate tonic device behavior in software layer.
	 * - encapsulate packet in accordance with the locator-lookup-table.
	 * - transmit encapsulated packet via physical NIC.
	 * - Should I implement this to existing drivers for normal NIC?
	 */

	int err, headroom;
	__u64 id;
	struct raven_table *rt;
	struct raven_dev *rdev = netdev_priv (dev);
	struct pcpu_sw_netstats *tx_stats;
	struct flowi4 fl4;
	struct rtable *irt;

#ifdef OVBENCH
	skb->raven_xmit_in = rdtsc ();
#endif

	if (drop_mode)
		goto out;

	skb_scrub_packet(skb, false);

	/* find destination address */
	id = extract_id_from_packet (skb, &rdev->oc);
	rt = raven_table_find (rdev, id);

	if (!rt) {
		/* find default destination, id 0 */
		rt = raven_table_find (rdev, 0);
	}
	if (!rt) {
		pr_debug ("no dst entry for id %llu", id);
		goto tx_err;
	}

	/* rouitng lookup */
	memset (&fl4, 0, sizeof (fl4));
	fl4.daddr = rt->oe.dst;
	fl4.saddr = rdev->oc.src;
	irt = ip_route_output_key (dev_net (dev), &fl4);
	if (IS_ERR (irt)) {
		pr_debug ("%s, no route to %pI4", dev->name, &fl4.daddr);
		kfree_skb (skb);
		return -ENOMEM;
	}

	/* build udp header */

	headroom = rdev->ou.encap_enable ? 14 + 20 + 16 : 14 + 20;
	err = skb_cow_head (skb, headroom);
	if (unlikely (err)) {
		kfree_skb (skb);
		return -ENOMEM;
	}

	if (rdev->ou.encap_enable) {
		struct udphdr *uh;
		uh = (struct udphdr *) __skb_push (skb, sizeof (*uh));
		skb_reset_transport_header (skb);

		uh->dest	= rdev->ou.dst_port;
		uh->source	= rdev->ou.src_port;
		uh->len		= htons (skb->len);
		uh->check	= 0;	/* XXX: */
	}

	err = iptunnel_xmit (skb->sk, irt, skb, fl4.saddr, fl4.daddr,
			     rdev->oc.proto, 0, 16, 0, false);
	if (err < 0)
		goto tx_err;

out:
	tx_stats = this_cpu_ptr (dev->tstats);
	u64_stats_update_begin (&tx_stats->syncp);
	tx_stats->tx_packets++;
	tx_stats->tx_bytes += skb->len;
	u64_stats_update_end (&tx_stats->syncp);

	if (drop_mode) {
#ifdef OVBENCH
		if (0 < skb->ovbench_type && skb->ovbench_type < 7) {
			copy_ovbench_params (skb, rdev);
		}
#endif

		kfree_skb (skb);
		return NETDEV_TX_OK;
	}

	return NETDEV_TX_OK;

tx_err:
	dev->stats.tx_errors++;

	return NETDEV_TX_OK;
}

static int
raven_change_mtu (struct net_device *dev, int new_mtu)
{
#define MIN_MTU	46
#define MAX_MTU	65535

	if (!(new_mtu >= MIN_MTU && new_mtu <= MAX_MTU))
		return -EINVAL;

	dev->mtu = new_mtu;
	return 0;
}

static int
raven_init (struct net_device *dev)
{
	dev->tstats = netdev_alloc_pcpu_stats (struct pcpu_sw_netstats);
	if (!dev->tstats)
		return -ENOMEM;

	return 0;
}

static void
raven_uninit (struct net_device *dev)
{
	free_percpu (dev->tstats);
}

static const struct net_device_ops raven_netdev_ops = {
	.ndo_init		= raven_init,
	.ndo_uninit		= raven_uninit,
	.ndo_start_xmit		= raven_xmit,
	.ndo_get_stats64	= ip_tunnel_get_stats64,	/* XXX */
	.ndo_change_mtu		= raven_change_mtu,
	.ndo_validate_addr	= eth_validate_addr,
	.ndo_set_mac_address	= eth_mac_addr,
};

static int
raven_newlink (struct net *net, struct net_device *dev,
	       struct nlattr *tb[], struct nlattr *data[])
{
	int err;
	u32 ifindex;
	struct net_device *pdev = NULL;
	struct raven_net *rnet = net_generic (net, raven_net_id);
	struct raven_dev *rdev = netdev_priv (dev);

	if (data && data[IFLA_RAVEN_PHYSICAL_DEV]) {
		ifindex = nla_get_u32 (data[IFLA_RAVEN_PHYSICAL_DEV]);
		pdev = __dev_get_by_index (net, ifindex);
		if (!pdev) {
			pr_debug ("%s: no device found, index %u",
				  __func__, ifindex);
		}
		rdev->pdev = pdev;
	}

	err = register_netdevice (dev);
	if (err) {
		netdev_err (dev, "failed to register netdevice.\n");
		return err;
	}

	list_add_tail_rcu (&rdev->list, &rnet->dev_list);

	err = madcap_register_device (dev, &raven_madcap_ops);
	if (err < 0) {
		netdev_err (dev, "failed to register madcap_ops.\n");
		return err;
	}

	return 0;
}

static void
raven_dellink (struct net_device *dev, struct list_head *head)
{
	struct raven_dev *rdev = netdev_priv (dev);

	write_lock_bh (&rdev->lock);
	raven_table_destroy (rdev);
	write_unlock_bh (&rdev->lock);

	list_del_rcu (&rdev->list);
	madcap_unregister_device (dev);
	unregister_netdevice_queue (dev, head);
}

static void
raven_setup (struct net_device *dev)
{
	int n;
	struct raven_dev *rdev = netdev_priv (dev);

	eth_hw_addr_random (dev);
	ether_setup (dev);
	dev->netdev_ops = &raven_netdev_ops;
	dev->destructor = free_netdev;

	/* XXX: qlene 0 causes special data path shortcut on __dev_queue_xmit. 
	 * More considerlation is needed for the research view.
	 */
	dev->tx_queue_len = 0;	
	dev->features	|= NETIF_F_LLTX;
	dev->features	|= NETIF_F_NETNS_LOCAL;
	dev->priv_flags	|= IFF_LIVE_ADDR_CHANGE;	/* XXX: phydev? */
	netif_keep_dst (dev);

	INIT_LIST_HEAD (&rdev->list);
	rwlock_init (&rdev->lock);
	rdev->dev = dev;
	rdev->vdev = NULL;	/* alloced by mco_acquire_dev */

	rdev->ou.obj.id = MADCAP_OBJ_ID_UDP;
	rdev->oc.obj.id = MADCAP_OBJ_ID_LLT_CONFIG;

	for (n = 0; n < RAVEN_HASH_SIZE; n++)
		INIT_HLIST_HEAD (&rdev->raven_table[n]);
}

static size_t
raven_get_size (const struct net_device *dev)
{
	/* IFLA_RAVEN_PHYSICAL_DEV */
	return nla_total_size (sizeof (__u32)) + 0;
}

static int
raven_fill_info (struct sk_buff *skb, const struct net_device *dev)
{
	__u32 ifindex = 0;
	const struct raven_dev *rdev = netdev_priv (dev);

	if (rdev->pdev)
		ifindex = rdev->pdev->ifindex;

	if (nla_put_u32 (skb, IFLA_RAVEN_PHYSICAL_DEV, ifindex))
		return -EMSGSIZE;

	return 0;
}

static struct rtnl_link_ops raven_link_ops __read_mostly = {
	.kind		= "raven",
	.maxtype	= IFLA_RAVEN_MAX,
	.priv_size	= sizeof (struct raven_dev),
	.setup		= raven_setup,
	.newlink	= raven_newlink,
	.dellink	= raven_dellink,
	.get_size	= raven_get_size,
	.fill_info	= raven_fill_info,
};

static __net_init int
raven_init_net (struct net *net)
{
	struct raven_net *rnet = net_generic (net, raven_net_id);

	INIT_LIST_HEAD (&rnet->dev_list);
	return 0;
}

static void __net_exit
raven_exit_net (struct net *net)
{
	struct raven_net *rnet = net_generic (net, raven_net_id);
	struct raven_dev *rdev, *next;
	LIST_HEAD (list);

	rtnl_lock ();
	list_for_each_entry_safe (rdev, next, &rnet->dev_list, list) {
		unregister_netdevice_queue (rdev->dev, &list);
	}
	rtnl_unlock ();

	return;
}

static struct pernet_operations raven_net_ops = {
	.init  	= raven_init_net,
	.exit	= raven_exit_net,
	.id	= &raven_net_id,
	.size	= sizeof (struct raven_net),
};


static __init int
raven_init_module (void)
{
	int rc;
#ifdef OVBENCH
	struct proc_dir_entry *ent;
#endif

	get_random_bytes (&raven_salt, sizeof (raven_salt));

	rc = register_pernet_subsys (&raven_net_ops);
	if (rc < 0)
		goto netns_failed;

	rc = rtnl_link_register (&raven_link_ops);
	if (rc < 0)
		goto rtnl_failed;

#ifdef OVBENCH
#define PROC_NAME	"driver/raven"
        ent = proc_create(PROC_NAME, S_IRUGO | S_IWUGO | S_IXUGO,
			  NULL, &raven_file_fops);
	if (ent == NULL) {
		rtnl_link_unregister (&raven_link_ops);
		unregister_pernet_subsys (&raven_net_ops);
		return -ENOMEM;
	}
#endif

	pr_info ("raven (%s) is loaded.", RAVEN_VERSION);

	if (drop_mode)
		pr_info ("drop mode on");
	if (madcap_enable)
		pr_info ("madcap mode on");

	return 0;

rtnl_failed:
	unregister_pernet_subsys (&raven_net_ops);
netns_failed:
	return rc;
}
module_init (raven_init_module)

static void __exit
raven_exit_module (void)
{
	rtnl_link_unregister (&raven_link_ops);
	unregister_pernet_subsys (&raven_net_ops);

#ifdef OVBENCH
	remove_proc_entry (PROC_NAME, NULL);
#endif
	pr_info ("raven (%s) is unloaded.", RAVEN_VERSION);
}
module_exit (raven_exit_module);
