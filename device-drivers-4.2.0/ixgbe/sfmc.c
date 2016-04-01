
#include <linux/moduleparam.h>
#include <linux/etherdevice.h>
#include <linux/if_ether.h> 
#include <net/netevent.h>
#include <net/arp.h>
#include <net/neighbour.h>
#include <net/ip_fib.h>
#include <net/switchdev.h>
#include <uapi/linux/rtnetlink.h>

#include "sfmc.h"

/* For ixgbe */
#include "ixgbe.h"
#define SFMC_NETDEV_PRIV       ixgbe_adapter

#undef pr_fmt
#define pr_fmt(fmt) KBUILD_MODNAME "-sfmc: " fmt "\n"

#undef pr_debug
#define pr_debug(fmt, ...) \
	printk(KERN_INFO pr_fmt("%s: "fmt) , __func__, ##__VA_ARGS__)


static int madcap_enable __read_mostly = 0;
module_param_named (madcap_enable, madcap_enable, int, 0444);
MODULE_PARM_DESC (madcap_enable, "if 1, madcap offload is enabled.");

static bool netevent_registered = false;


/* MadCap locator-lookup table structure. this is hash table. */
struct sfmc_table {
	struct hlist_node	hlist;	/* sfmc->sfmc_table[] */
	struct rcu_head		rcu;
	struct sfmc		*sfmc;
	unsigned long		updated;

	struct madcap_obj_entry	oe;
};


struct sfmc_fib {
	struct list_head	list;	/* sfmc->fib_list */
	struct rcu_head		rcu;

	struct sfmc 	*sfmc;		/* parent */

	patricia_node_t	*pn;		/* patricia node of this fib */
	prefix_t	*prefix;	/* prefix of this fib	*/

	__be32		network;	/* destination network */
	u8		len;		/* destination network prefix length */
	enum rt_scope_t	scope;		/* fib_info->fib_scope */

	__be32		gateway;	/* gateway address	*/
	u8		mac[ETH_ALEN];	/* gateway ma address	*/
	u8		nud_state;	/* neighbour state */

	struct work_struct ll_work;	/* arp resolve for link local route */
};


/* prototypes */
static void sfmc_ll_neigh_work (struct work_struct *work);

static void sfmc_neigh_write (struct sfmc_fib *sf, struct neighbour *n);
static int sfmc_neigh_resolve (struct sfmc *sfmc, struct sfmc_fib *sf);




static inline struct sfmc *
netdev_get_sfmc (struct net_device *dev)
{
	struct SFMC_NETDEV_PRIV *priv;

	priv = netdev_priv (dev);
	return &priv->sfmc;
}

/* sfmc table operations */

static inline struct hlist_head *
sfmc_table_head (struct sfmc * sfmc, u64 key)
{
	return &sfmc->sfmc_table[hash_64 (key, SFMC_HASH_BITS)];
}

static struct sfmc_table *
sfmc_table_add (struct sfmc *sfmc, struct madcap_obj_entry *oe)
{
	struct sfmc_table *st;

	st = (struct sfmc_table *) kmalloc (sizeof (*st), GFP_KERNEL);
	if (!st)
		return NULL;

	memset (st, 0, sizeof (*st));

	st->sfmc	= sfmc;
	st->updated	= jiffies;
	st->oe		= *oe;

	hlist_add_head_rcu (&st->hlist, sfmc_table_head (sfmc, oe->id));

	return st;
}

static void
sfmc_table_delete (struct sfmc_table *st)
{
	hlist_del_rcu (&st->hlist);
	kfree_rcu (st, rcu);
}
       
static void
sfmc_table_destroy (struct sfmc *sfmc)
{
	unsigned int n;

	for (n = 0; n < SFMC_HASH_SIZE; n++) {
		struct hlist_node *ptr, *tmp;

		hlist_for_each_safe (ptr, tmp, &sfmc->sfmc_table[n]) {
			struct sfmc_table *st;

			st = container_of (ptr, struct sfmc_table, hlist);
			sfmc_table_delete (st);
		}
	}
}

static struct sfmc_table *
sfmc_table_find (struct sfmc *sfmc, u64 id)
{
	struct hlist_head *head = sfmc_table_head (sfmc, id);
	struct sfmc_table *st;

	hlist_for_each_entry_rcu (st, head, hlist) {
		if (id == st->oe.id)
			return st;
	}

	return NULL;
}

/* sfmc fib operations */
static struct sfmc_fib *
sfmc_fib_find_exact (struct sfmc *sfmc, __be32 network, u8 len)
{
	prefix_t prefix;
	patricia_node_t *pn;

	dst2prefix (network, len, &prefix);

	pn = patricia_search_exact (sfmc->fib_tree, &prefix);

	if (pn)
		return pn->data;

	return NULL;
}

static struct sfmc_fib *
sfmc_fib_find_best (struct sfmc *sfmc, __be32 network, u8 len)
{
	prefix_t prefix;
	patricia_node_t *pn;

	dst2prefix (network, len, &prefix);

	pn = patricia_search_best (sfmc->fib_tree, &prefix);

	if (pn)
		return pn->data;

	return NULL;
}

static struct sfmc_fib *
sfmc_fib_create (struct sfmc *sfmc, __be32 network, u8 len, __be32 gateway,
		 enum rt_scope_t scope, int gfp)
{
	struct sfmc_fib *sf;

	sf = (struct sfmc_fib *) kmalloc (sizeof (struct sfmc_fib), gfp);
	if (!sf)
		return NULL;

	memset (sf, 0, sizeof (*sf));
	sf->sfmc	= sfmc;
	sf->network	= network;
	sf->len		= len;
	sf->gateway	= gateway;
	sf->scope	= scope;
	INIT_LIST_HEAD (&sf->list);
	INIT_WORK (&sf->ll_work, sfmc_ll_neigh_work);

	return sf;
}

static struct sfmc_fib *
sfmc_fib_insert (struct sfmc *sfmc, struct sfmc_fib *sf)
{
	prefix_t *prefix;
	patricia_node_t *pn;

	prefix = kmalloc (sizeof (prefix_t), GFP_KERNEL);
	memset (prefix, 0, sizeof (*prefix));
	dst2prefix (sf->network, sf->len, prefix);
	
	pn = patricia_lookup (sfmc->fib_tree, prefix);
	if (pn->data != NULL) {
		pr_debug ("insert fib exist %pI4/%d", &sf->network, sf->len);
		kfree (prefix);
		return pn->data;
	}

	pn->data	= sf;
	sf->pn		= pn;
	sf->prefix	= prefix;

	list_add_rcu (&sf->list, &sfmc->fib_list);

	pr_debug ("insert fib %pI4/%d->%pI4",
		  &sf->network, sf->len, &sf->gateway);

	return sf;
}

static struct sfmc_fib *
sfmc_fib_add (struct sfmc *sfmc, __be32 network, u8 len, __be32 gateway,
	      enum rt_scope_t scope)
{
	struct sfmc_fib *sf, *tmp;

	sf = sfmc_fib_create (sfmc, network, len, gateway, scope, GFP_KERNEL);
	if (!sf)
		return NULL;

	tmp = sfmc_fib_insert (sfmc, sf);
	if (!tmp) {
		kfree (sf);
		return NULL;
	}

	return sf;
}

static void
sfmc_fib_delete (struct sfmc_fib *sf)
{
	if (!sf)
		return;

	pr_debug ("delete fib %pI4/%d->%pI4",
		  &sf->network, sf->len, &sf->gateway);

	patricia_remove (sf->sfmc->fib_tree, sf->pn);
	list_del_rcu (&sf->list);
	kfree_rcu (sf, rcu);
}

static int
sfmc_fib_del (struct sfmc *sfmc, __be32 network, u8 len)
{
	struct sfmc_fib *sf;

	sf = sfmc_fib_find_exact (sfmc, network, len);
	if (!sf)
		return -ENOENT;

	sfmc_fib_delete (sf);

	return 0;
}

static void
sfmc_fib_destroy (struct sfmc *sfmc)
{
	struct list_head *p, *tmp;
	struct sfmc_fib *sf;

	list_for_each_safe (p, tmp, &sfmc->fib_list) {
		sf = list_entry (p, struct sfmc_fib, list);
		sfmc_fib_delete (sf);
	}

	kfree (sfmc->fib_tree);
}


/* madcap_ops functions */

static int
sfmc_acquire_dev (struct net_device *dev, struct net_device *vdev)
{
	int n;
	struct sfmc *sfmc = netdev_get_sfmc (dev);

	for (n = 0; n < SFMC_VDEV_MAX; n++) {
		if (sfmc->vdev[n] == vdev)
			return -EEXIST;
	}

	for (n = 0; n < SFMC_VDEV_MAX; n++) {
		if (sfmc->vdev[n] == NULL) {
			sfmc->vdev[n] = vdev;
			return 0;
		}
	}

	return -ENOMEM;
}

static int
sfmc_release_dev (struct net_device *dev, struct net_device *vdev)
{
	int n;
	struct sfmc *sfmc = netdev_get_sfmc (dev);

	for (n = 0; n < SFMC_VDEV_MAX; n++) {
		if (sfmc->vdev[n] == vdev) {
			sfmc->vdev[n] = NULL;
			return 0;
		}
	}

	return -ENOENT;
}

static int
sfmc_llt_cfg (struct net_device *dev, struct madcap_obj *obj)
{
	struct sfmc *sfmc = netdev_get_sfmc (dev);
	struct madcap_obj_config *oc = MADCAP_OBJ_CONFIG (obj);

	if (memcmp (oc, &sfmc->oc, sizeof (*oc) != 0)) {
		/* offset or length is changed. drop all table entry. */
		sfmc_table_destroy (sfmc);
		sfmc->oc = *oc;
	}

	return 0;
}

static struct madcap_obj *
sfmc_llt_config_get (struct net_device *dev)
{
	struct sfmc *sfmc = netdev_get_sfmc (dev);
	return MADCAP_OBJ (sfmc->oc);
}

static int
sfmc_llt_entry_add (struct net_device *dev, struct madcap_obj *obj)
{
	struct sfmc_table *st;
	struct sfmc *sfmc = netdev_get_sfmc (dev);
	struct madcap_obj_entry *oe = MADCAP_OBJ_ENTRY (obj);
	
	st = sfmc_table_find (sfmc, oe->id);
	if (st)
		return -EEXIST;

	st = sfmc_table_add (sfmc, oe);
	if (!st)
		return -ENOMEM;

	return 0;
}

static int
sfmc_llt_entry_del (struct net_device *dev, struct madcap_obj *obj)
{
	struct sfmc_table *st;
	struct sfmc *sfmc = netdev_get_sfmc (dev);
	struct madcap_obj_entry *oe = MADCAP_OBJ_ENTRY (obj);
	
	st = sfmc_table_find (sfmc, oe->id);
	if (!st)
		return -ENOENT;

	sfmc_table_delete (st);
	
	return 0;
}

static struct madcap_obj *
sfmc_llt_entry_dump (struct net_device *dev, struct netlink_callback *cb)
{
	int idx, cnt;
	unsigned int n;
	struct sfmc *sfmc = netdev_get_sfmc (dev);
	struct sfmc_table *st;
	struct madcap_obj_entry *oe;

	idx = cb->args[0];
	oe = NULL;

	for (n = 0, cnt = 0; n < SFMC_HASH_SIZE; n++) {
		hlist_for_each_entry_rcu (st, &sfmc->sfmc_table[n], hlist) {
			if (idx > cnt) {
				cnt++;
				continue;
			}

			oe = &st->oe;
			goto out;
		}
	}

out:
	cb->args[0] = cnt + 1;
	return MADCAP_OBJ (*oe);
}

static int
sfmc_udp_cfg (struct net_device *dev, struct madcap_obj *obj)
{
	struct madcap_obj_udp *ou;
	struct sfmc *sfmc = netdev_get_sfmc (dev);

	ou = MADCAP_OBJ_UDP (obj);
	sfmc->ou = *ou;

	return 0;
}

static struct madcap_obj *
sfmc_udp_config_get (struct net_device *dev)
{
	struct sfmc *sfmc = netdev_get_sfmc (dev);
	return MADCAP_OBJ(sfmc->ou);
}


static struct madcap_ops sfmc_madcap_ops = {
	.mco_acquire_dev	= sfmc_acquire_dev,
	.mco_release_dev	= sfmc_release_dev,
	.mco_llt_cfg		= sfmc_llt_cfg,
	.mco_llt_config_get	= sfmc_llt_config_get,
	.mco_llt_entry_add	= sfmc_llt_entry_add,
	.mco_llt_entry_del	= sfmc_llt_entry_del,
	.mco_llt_entry_dump	= sfmc_llt_entry_dump,
	.mco_udp_cfg		= sfmc_udp_cfg,
	.mco_udp_config_get	= sfmc_udp_config_get,
};


/* Encap in accordance with madcap */

static inline __u64
extract_id_from_packet (struct sk_buff *skb, struct madcap_obj_config *oc)
{
        int i;
	__u64 id;

	id = *((__u64 *)(skb->data + oc->offset));

	for (i = 0; i < (64 - oc->length); i++)
		id >>= 1;

	return id;
}


static inline u16
ipchecksum(const void * data, u16 len, u32 sum)
{
	const u8 *addr = data;
	u32 i;

	/* Checksum all the pairs of bytes first... */
	for (i = 0; i < (len & ~1U); i += 2) {
		sum += (u16)ntohs(*((u16 *)(addr + i)));
		if (sum > 0xFFFF)
			sum -= 0xFFFF;
	}
	/*
         * If there's a single byte left over, checksum it, too.
         * Network byte order is big-endian, so the remaining byte is
         * the high byte.
         */

	if (i < len) {
		sum += addr[i] << 8;
		if (sum > 0xFFFF)
			sum -= 0xFFFF;
	}

	sum = ~sum & 0xFFFF;
	return htons (sum);
}

int
sfmc_encap_packet (struct sk_buff *skb, struct net_device *dev)
{
	int n;
	__u64 id;
	struct sfmc *sfmc = netdev_get_sfmc (dev);
	struct sfmc_table *st;
	struct sfmc_fib *sf;
	struct iphdr *iph;
	struct udphdr *uh;
	struct ethhdr *eth;
	struct dst_entry *dst;

	if (!madcap_enable)
		return 0;

	/* check: is this packet from acquiring device.
	 * In madcap mode, ip_route_output_key is not needed, so
	 * original destination of first routing lookup for the inner
	 * packet is preserved.
	 */
	dst = skb_dst (skb);
	if (!dst)
		return 0;

	for (n = 0; n < SFMC_VDEV_MAX; n++) {
		if (sfmc->vdev[n] == dst->dev)
			goto encap;
	}

	return 0;

encap:

	/* lookup destination node from locator-lookup-table */
	id = extract_id_from_packet (skb, &sfmc->oc);
	st = sfmc_table_find (sfmc, id);
	st = (st) ? st : sfmc_table_find (sfmc, id);
	if (!st) {
		pr_debug ("locator lookup table not found\n");
		return -ENOENT;
	}

	/* lookup ipv4 route and neighbour for dst node */
	sf = sfmc_fib_find_best (sfmc, st->oe.dst, 32);
	if (!sf) {
		pr_debug ("no fib entry for %pI4", &st->oe.dst);
		return -ENOENT;
	}
	if (sf->scope == RT_SCOPE_LINK && sf->gateway != st->oe.dst) {
		/* this is connected route. add host route and wait
		 * for neighbor resolution */
		struct sfmc_fib *llsf;
		pr_debug ("create link local fib %pI4", &st->oe.dst);
		llsf = sfmc_fib_create (sfmc, st->oe.dst, 32,
					st->oe.dst, RT_SCOPE_LINK,
					GFP_ATOMIC);
		queue_work (sfmc->sfmc_wq, &llsf->ll_work);
		return -ENOENT;
	}

	if (!(sf->nud_state & NUD_VALID)) {
		pr_debug ("neighstate is not VALID for %pI4", &sf->gateway);
		return -ENOENT;
	}

	/* ok, destination node is found, ip route is found and
	 * neighbour state is valid. start to encap the pcaket! */

	/* encap udp */
	if (sfmc->ou.encap_enable) {
		uh = (struct udphdr *) __skb_push (skb, sizeof (*uh));
		uh->dest	= sfmc->ou.dst_port;
		uh->source	= sfmc->ou.src_port;
		uh->len		= htons (skb->len);
		uh->check	= 0;	/* XXX */
		skb_set_transport_header (skb, 0);
	}

	/* encap ip */
	iph = (struct iphdr *) __skb_push (skb, sizeof (*iph));
	iph->version	= 4;
	iph->ihl	= sizeof (*iph) >> 2;
	iph->frag_off	= 0;
	iph->id		= 0;
	iph->protocol	= sfmc->oc.proto;
	iph->tos	= 0;
	iph->ttl	= 64;
	iph->tot_len	= htons (skb->len);
	iph->daddr	= st->oe.dst;
	iph->saddr	= sfmc->oc.src;
	iph->check	= 0;
	iph->check	= ipchecksum (skb->data, sizeof (*iph), 0);

	skb_set_network_header (skb, 0);

	/* add ethernet header */
	eth = (struct ethhdr *) __skb_push (skb, sizeof (*eth));
	memcpy (eth->h_dest, sf->mac, ETH_ALEN);
	memcpy (eth->h_source, dev->perm_addr, ETH_ALEN);
	eth->h_proto = htons (ETH_P_IP);
	skb_set_mac_header (skb, 0);

	return 0;
}


/* switchdev ops */

static int
sfmc_port_attr_set (struct net_device *dev, struct switchdev_attr *attr)
{
	return -ENOTSUPP;
}

static int
sfmc_port_attr_get (struct net_device *dev, struct switchdev_attr *attr)
{
	struct sfmc *sfmc = netdev_get_sfmc (dev);

	switch (attr->id) {
	case SWITCHDEV_ATTR_PORT_PARENT_ID:
		/* only this is needed by switchdev_get_dev_by_nhs() for
		 * switchdev_fib_ipv4_add() */
		memcpy (&attr->u.ppid.id, &sfmc->id, attr->u.ppid.id_len);
		break;
	default :
		return -EOPNOTSUPP;
	}

	return 0;
}

static int
sfmc_port_obj_add (struct net_device *dev, struct switchdev_obj *obj)
{
	int err = 0;
	__be32 gateway, network;
	struct sfmc *sfmc = netdev_get_sfmc (dev);
	struct sfmc_fib *sf;
	struct switchdev_obj_ipv4_fib *fib;

	switch (obj->trans) {
	case SWITCHDEV_TRANS_PREPARE:
	case SWITCHDEV_TRANS_ABORT:
		/* nothing to do for prepare phase because I'm fishy
		   software emulation :) */
		return 0;
	case SWITCHDEV_TRANS_COMMIT:
		break;
	default:
		pr_debug ("unknown switchdev trans %d", obj->trans);
		return -ENOTSUPP;
	}

	switch (obj->id) {
	case SWITCHDEV_OBJ_IPV4_FIB:
		fib = &obj->u.ipv4_fib;
		gateway = fib->fi->fib_nh->nh_gw;
		network = htonl (fib->dst);
		if (gateway == 0 && fib->fi->fib_scope != RT_SCOPE_LINK) {
			pr_debug ("no install %pI4/%d->%pI4",
				  &network, fib->dst_len, &gateway);
			err = 0;
			break;
		}

		sf = sfmc_fib_add (sfmc, network, fib->dst_len, gateway,
				   fib->fi->fib_scope);
		if (!sf) {
			pr_debug ("sf is null");
			return -ENOMEM;
		}

		sfmc_neigh_resolve (sfmc, sf);

		err = 0;
		break;

	default:
		err = -EOPNOTSUPP;
		break;
	}

	return err;
}

static int
sfmc_port_obj_del (struct net_device *dev, struct switchdev_obj *obj)
{
	int err = 0;
	struct sfmc *sfmc = netdev_get_sfmc (dev);
	struct switchdev_obj_ipv4_fib *fib;

	pr_debug ("switchdev_ops_obj_del is called!!");

	switch (obj->id) {
	case SWITCHDEV_OBJ_IPV4_FIB:
		fib = &obj->u.ipv4_fib;
		err = sfmc_fib_del (sfmc, htonl (fib->dst), fib->dst_len);
		break;

	default:
		err = -EOPNOTSUPP;
		break;
	}

	return err;
}

static const struct switchdev_ops sfmc_switchdev_ops = {
	.switchdev_port_attr_get	= sfmc_port_attr_get,
	.switchdev_port_attr_set	= sfmc_port_attr_set,
	.switchdev_port_obj_add		= sfmc_port_obj_add,
	.switchdev_port_obj_del		= sfmc_port_obj_del,
};


/* neighbour update handler */

static void
sfmc_neigh_write (struct sfmc_fib *sf, struct neighbour *n)
{
	memcpy (sf->mac, n->ha, ETH_ALEN);
	sf->nud_state = n->nud_state;

	pr_debug ("%pI4->%02x:%02x:%02x:%02x:%02x:%02x, %s",
		  &sf->gateway,
		  n->ha[0],n->ha[1],n->ha[2],
		  n->ha[3],n->ha[3],n->ha[5],
		  (sf->nud_state & NUD_VALID) ? "valid" : "no-valid");
}

static int
sfmc_neigh_resolve (struct sfmc *sfmc, struct sfmc_fib *sf)
{
	int err = 0;
	__be32 ip_addr = sf->gateway;
	struct neighbour *n;

	n = __ipv4_neigh_lookup (sfmc->dev, (__force u32)ip_addr);
	if (!n) {
		n = neigh_create (&arp_tbl, &ip_addr, sfmc->dev);
		if (IS_ERR (n))
			return IS_ERR (n);
	}

	if (n->nud_state & NUD_VALID)
		sfmc_neigh_write (sf, n);
	else
		neigh_event_send (n, NULL);

	neigh_release (n);

	return err;
}

static void
sfmc_neigh_update (struct net_device *dev, struct neighbour *n)
{
	__be32 ip_addr = *(__be32 *) n->primary_key;
	struct sfmc *sfmc = netdev_get_sfmc (dev);
	struct sfmc_fib *sf;

	list_for_each_entry_rcu (sf, &sfmc->fib_list, list) {
		if (sf->gateway == ip_addr) {
			sfmc_neigh_write (sf, n);
		}
	}
}

static int
sfmc_neigh_update_event (struct notifier_block *unused, unsigned long event,
			 void *ptr)
{
	struct net_device *dev;
	struct neighbour *n = ptr;

	switch (event) {
	case NETEVENT_NEIGH_UPDATE:
		if (n->tbl != &arp_tbl)
			return NOTIFY_DONE;
		dev = n->dev;

		if (dev->switchdev_ops != &sfmc_switchdev_ops)
			return NOTIFY_DONE;

		sfmc_neigh_update (dev, n);
		break;
	}

	return NOTIFY_DONE;
}

static struct notifier_block sfmc_netevent_nb __read_mostly = {
	.notifier_call = sfmc_neigh_update_event,
};

static void
sfmc_ll_neigh_work (struct work_struct *work)
{
	/* arp resolution for link local (connected) host. */

	struct sfmc_fib *sf, *tmp;

	/* this sf is just after sfmc_fib_create. */
	sf = container_of (work, struct sfmc_fib, ll_work);

	tmp = sfmc_fib_insert (sf->sfmc, sf);
	if (!tmp) {
		/* the fib for this host entry is already created. */
		kfree (sf);
	}

	sfmc_neigh_resolve (sf->sfmc, sf);
}

int
sfmc_init (struct sfmc *sfmc, struct net_device *dev)
{
	int n, err;

	memset (sfmc, 0, sizeof (*sfmc));

	sfmc->dev = dev;
	rwlock_init (&sfmc->lock);

	/* init hash table for madcap_obj_entry */
	for (n = 0; n < SFMC_HASH_SIZE; n++)
		INIT_HLIST_HEAD (&sfmc->sfmc_table[n]);

	/* init fib tree for ip routing */
	INIT_LIST_HEAD (&sfmc->fib_list);
	sfmc->fib_tree = New_Patricia (32);

	/* init work queue for link local neighbor resolution */
	sfmc->sfmc_wq = alloc_workqueue ("sfmc-ll-work-%s", 0, 0, dev->name);
	if (!sfmc->sfmc_wq) {
		pr_err ("failed to allocate work queue");
		return -ENOMEM;
	}

	/* initialize switchdev_ops */
	memcpy (&sfmc->id, dev->perm_addr, ETH_ALEN);
	dev->switchdev_ops = &sfmc_switchdev_ops;
	pr_info ("%s sfmc switchdev id %016llx", dev->name, sfmc->id);

	/* regsiter madcap ops */
	if (madcap_enable) {
		err = madcap_register_device (dev, &sfmc_madcap_ops);
		if (err < 0) {
			netdev_err (dev, "failed to register madcap_ops.\n");
			return err;
		}
	}

	/* register neighbour handle notifier */
	if (!netevent_registered) {
		register_netevent_notifier (&sfmc_netevent_nb);
		netevent_registered = true;
	}

	return 0;
}

int
sfmc_exit (struct sfmc *sfmc)
{
	if (netevent_registered) {
		unregister_netevent_notifier (&sfmc_netevent_nb);
		netevent_registered = false;
	}

	sfmc_table_destroy (sfmc);
	sfmc_fib_destroy (sfmc);
	destroy_workqueue (sfmc->sfmc_wq);

	if (madcap_enable)
		madcap_unregister_device (sfmc->dev);

	return 0;
}
