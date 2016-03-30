
#include <linux/etherdevice.h>
#include <linux/if_ether.h> 
#include <linux/if_arp.h>
#include <net/arp.h>
#include <net/switchdev.h>
#include <net/ip_fib.h>

#include "sfmc.h"

/* For e1000 */
#include "e1000.h"
#define SFMC_NETDEV_PRIV       e1000_adapter

/* MadCap locator-lookup table structure. this is hash table. */
struct sfmc_table {
	struct hlist_node	hlist;	/* sfmc->sfmc_table[] */
	struct rcu_head		rcu;
	struct sfmc		*sfmc;
	unsigned long		updated;

	struct madcap_obj_entry	oe;
};


/* FIB structure and ARP handling */

enum {
	ARP_PROBE,
	ARP_REACHABLE,
	ARP_REPROBE,
};
/* PROBE    -> repeating arp req until recv arp rep.
 * RECHABLE -> set when recv arp rep. decrement ttl.
 * REPROBE  -> set when ttl of REACHABLE becomes 0.
 * 	if ttl % INTERVAL = 0, send arp req.
 * 	if arp ttl becomes 0, state is set to PROBE.
 *
 * if recv arp repp, check all sfmc_fib.
 */

#define ARP_PROBE_INTERVAL	3	/* interval of arp req */
#define ARP_REACH_LIFETIME	60	/* decrement in each sec */
#define ARP_PROBE_LIFETIME	60	/* lifetime with arp req */

struct sfmc_fib {
	struct list_head	list;	/* sfmc->fib_list */
	struct rcu_head		rcu;

	patricia_node_t	*pn;		/* patricia node of this fib */
	prefix_t	*prefix;	/* prefix of this fib	*/

	__be32		network;	/* destination network */
	u8		len;		/* destination network prefix length */

	__be32		gateway;	/* gateway address	*/
	u8		mac[ETH_ALEN];	/* gateway ma address	*/

	int		arp_state;	/* arp state */
	int		arp_ttl;	/* lifetime */
};


struct arpbody {
	unsigned char ar_sha[ETH_ALEN];
	unsigned char ar_sip[4];
	unsigned char ar_tha[ETH_ALEN];
	unsigned char ar_tip[4];
};

#define maccopy(a, b)					\
	do {						\
		b[0] = a[0]; b[1] = a[1]; b[2] = a[2];	\
		b[3] = a[3]; b[4] = a[4]; b[5] = a[5];	\
	} while (0)

#define macbcast(b)					\
	do {						\
		b[0] = 0xFF; b[1] = 0xFF; b[2] = 0xFF;	\
		b[3] = 0xFF; b[4] = 0xFF; b[5] = 0xFF;	\
	} while (0)


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
sfmc_fib_add (struct sfmc *sfmc, __be32 network, u8 len, __be32 gateway)
{
	prefix_t *prefix;
	patricia_node_t *pn;
	struct sfmc_fib *sf;

	prefix = kmalloc (sizeof (prefix_t), GFP_KERNEL);
	memset (prefix, 0, sizeof (*prefix));
	dst2prefix (network, len, prefix);

	pn = patricia_lookup (sfmc->fib_tree, prefix);
	if (pn->data != NULL) {
		kfree (prefix);
		return pn->data;
	}

	sf = (struct sfmc_fib *) kmalloc (sizeof (struct sfmc_fib),
					  GFP_KERNEL);
	if (!sf)
		return NULL;

	memset (sf, 0, sizeof (*sf));

	sf->pn		= pn;
	sf->prefix	= prefix;
	sf->network	= network;
	sf->len		= len;
	sf->gateway	= gateway;
	sf->arp_state	= ARP_PROBE;
	sf->arp_ttl	= ARP_PROBE_LIFETIME;
	INIT_LIST_HEAD (&sf->list);

	pn->data = sf;
	list_add_rcu (&sf->list, &sfmc->fib_list);

	pr_debug ("add fib %pI4/%d -> %pI4\n", &network, len, &gateway);

	return sf;
}

static void
sfmc_fib_delete (struct sfmc_fib *sf)
{
	if (!sf)
		return;

	pr_debug ("add fib %pI4/%d -> %pI4\n",
		  &sf->network, sf->len, &sf->gateway);

	list_del_rcu (&sf->list);
	kfree (sf->prefix);
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
patricia_destroy_fib (void * data)
{
	sfmc_fib_delete ((struct sfmc_fib *) data);
}

static void
sfmc_fib_destroy (struct sfmc *sfmc)
{
	Destroy_Patricia (sfmc->fib_tree, patricia_destroy_fib);
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
	if (!st)
		return -EEXIST;

	st = sfmc_table_add (sfmc, oe);
	if (!st)
		return -ENOENT;

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

	/* check: is this packet from acquiring device ? */
	for (n = 0; n < SFMC_VDEV_MAX; n++) {
		if (sfmc->vdev[n] == skb->dev)
			goto encap;
	}
	return 0;

encap:
	id = extract_id_from_packet (skb, &sfmc->oc);
	st = sfmc_table_find (sfmc, id);

	/* find default destination */
	st = (st) ? st : sfmc_table_find (sfmc, id);

	if (!st) {
		pr_debug ("locator lookup table not found\n");
		return -ENOENT;
	}

	/* encap udp */
	if (sfmc->ou.encap_enable) {

		uh = (struct udphdr *) __skb_push (skb, sizeof (*uh));
		skb_reset_transport_header (skb);
		uh->dest	= sfmc->ou.dst_port;
		uh->source	= sfmc->ou.src_port;
		uh->len		= htons (skb->len);
		uh->check	= 0;	/* XXX */
	}

	/* encap ip */
	iph = (struct iphdr *) __skb_push (skb, sizeof (*iph));
	skb_reset_network_header (skb);

	iph->version	= 4;
	iph->ihl	= sizeof (*iph) >> 2;
	iph->frag_off	= 0;
	iph->id		= 0;
	iph->protocol	= sfmc->oc.proto;
	iph->tos	= 0;
	iph->ttl	= 64;
	iph->daddr	= st->oe.dst;
	iph->saddr	= sfmc->oc.src;
	iph->check	= ipchecksum (skb->data, sizeof (*iph), 0);

	/* arp resolve and set ethernet header */
	sf = sfmc_fib_find_best (sfmc, st->oe.dst, 32);
	if (!sf) {
		pr_debug ("no fib entry for %pI4\n", &st->oe.dst);
		return -ENOENT;
	}

	if (sf->arp_state == ARP_REACHABLE || sf->arp_state == ARP_REPROBE) {
		pr_debug ("no arp entry for %pI4\n", &st->oe.dst);
		return -ENOENT;
	}

	eth = (struct ethhdr *) __skb_push (skb, sizeof (*eth));
	skb_reset_mac_header (skb);
	maccopy (sf->mac, eth->h_dest);
	maccopy (dev->perm_addr, eth->h_source);
	eth->h_proto = htons (ETH_P_IP);

	return 0;
}


/* arp related code */

static void
sfmc_fib_arp_update (struct sfmc * sfmc, struct arpbody *rep)
{
	struct sfmc_fib *sf;

	list_for_each_entry_rcu (sf, &sfmc->fib_list, list) {
		if (memcmp (&sf->gateway, rep->ar_sip, 4) == 0) {
			if (sf->arp_state == ARP_PROBE ||
			    sf->arp_state == ARP_REPROBE) {
				memcpy (sf->mac, rep->ar_sha, ETH_ALEN);
				sf->arp_state = ARP_REACHABLE;
				sf->arp_ttl = ARP_REACH_LIFETIME;
			}
		}
	}
}

void
sfmc_snoop_arp (struct sk_buff *skb)
{
	/* snoop arp reply for update fib_tree */
	
	struct arphdr *arp;
	struct arpbody *rep;
	struct net_device *dev = skb->dev;
	struct sfmc *sfmc = netdev_get_sfmc (dev);

	if (skb->protocol != htons (ETH_P_ARP))
		return;

	arp = arp_hdr (skb);
	if (arp->ar_pro == htons (ETH_P_IP) &&
	    arp->ar_op == htons (ARPOP_REPLY)) {
		rep = (struct arpbody *) (arp + 1);
		if (memcmp (rep->ar_tha, dev->perm_addr, ETH_ALEN) == 0 &&
		    memcmp (rep->ar_tip, &sfmc->oc.src, 4) == 0) {
			/* this arp reply is for this interface. learn it! */
			sfmc_fib_arp_update (sfmc, rep);
		}
	}
}

static void
sfmc_send_arp (struct sfmc *sfmc, struct sfmc_fib *sf)
{
	/* make and send arp requeset for sf->mac via dev */

	struct net_device *dev = sfmc->dev;

	struct sk_buff *skb;
	struct ethhdr *eth;
	struct arphdr *arp;
	struct arpbody *req;

	size_t size = sizeof (*eth) + sizeof (*arp) + sizeof (*req);

	skb = alloc_skb (size, GFP_ATOMIC);
	if (!skb) {
		pr_info ("failed to alloc skb for arp req.\n");
		return;
	}

	/* build arp header */

	req = (struct arpbody *) skb_put (skb, sizeof (*req));
	memset (req, 0, sizeof (*req));
	memcpy (req->ar_sha, dev->perm_addr, ETH_ALEN);
	memcpy (req->ar_sip, &sfmc->oc.src, sizeof (sfmc->oc.src));
	memcpy (req->ar_tip, &sf->gateway, sizeof (sf->gateway));

	arp = (struct arphdr *) skb_put (skb, sizeof (*arp));
	arp->ar_hrd	= htons (ARPHRD_ETHER);
	arp->ar_pro 	= htons (ETH_P_IP);
	arp->ar_hln	= ETH_ALEN;
	arp->ar_pln	= 4;
	arp->ar_op	= htons (ARPOP_REQUEST);

	/* build ethernet header */
	eth = (struct ethhdr *) skb_put (skb, sizeof (*eth));
	macbcast (eth->h_dest);
	maccopy (dev->perm_addr, eth->h_source);
	eth->h_proto = htons (ETH_P_ARP);


	skb->protocol = htons (ETH_P_ARP);
	skb->dev = dev;

	arp_xmit (skb);
}

/* switchdev ops */

static inline __be32
extract_gateway_addr_from_fib_info (struct fib_info *fi)
{
	struct fib_nh *nh;

	/* get first gateway address for this fib. */
	if (fi->fib_nhs < 1)
		return 0;

	nh = fi->fib_nh;
	return nh->nh_gw;
}

static int
sfmc_port_obj_add (struct net_device *dev, struct switchdev_obj *obj)
{
	int err = 0;
	__be32 gateway;
	struct sfmc *sfmc = netdev_get_sfmc (dev);
	struct sfmc_fib *sf;
	struct switchdev_obj_ipv4_fib *fib;

	/* XXX: obj->trans should be handled here ? */

	switch (obj->id) {
	case SWITCHDEV_OBJ_IPV4_FIB:
		fib = &obj->u.ipv4_fib;
		gateway = extract_gateway_addr_from_fib_info (fib->fi);
		if (gateway == 0)
			return -EINVAL;
		sf = sfmc_fib_add (sfmc, fib->dst, fib->dst_len, gateway);
		if (!sf)
			return -ENOMEM;
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

	switch (obj->id) {
	case SWITCHDEV_OBJ_IPV4_FIB:
		fib = &obj->u.ipv4_fib;
		err = sfmc_fib_del (sfmc, fib->dst, fib->dst_len);
		break;

	default:
		err = -EOPNOTSUPP;
		break;
	}

	return err;
}

static const struct switchdev_ops sfmc_switchdev_ops = {
	.switchdev_port_obj_add	= sfmc_port_obj_add,
	.switchdev_port_obj_del	= sfmc_port_obj_del,
};


/* arp handler */
static void
sfmc_arp_processor (unsigned long arg)
{
	unsigned long next_timer;
	struct sfmc *sfmc = (struct sfmc *) arg;
	struct sfmc_fib *sf;

#define time_for_send_arp_req(sf) (sf->arp_ttl % ARP_PROBE_INTERVAL == 0)
#define decrement_arp_ttl(sf, max)					\
	do {								\
		sf->arp_ttl = (sf->arp_ttl == 0) ? max : sf->arp_ttl - 1; \
	} while (0)

	list_for_each_entry_rcu (sf, &sfmc->fib_list, list) {
		/* check arp state and process */
		switch (sf->arp_state) {
		case ARP_PROBE :
			if (time_for_send_arp_req (sf))
				sfmc_send_arp (sfmc, sf);

			decrement_arp_ttl (sf, ARP_PROBE_LIFETIME);
			break;

		case ARP_REACHABLE :
			decrement_arp_ttl (sf, 0);
			if (sf->arp_ttl == 0)
				sf->arp_state = ARP_REPROBE;
			break;

		case ARP_REPROBE :
			if (time_for_send_arp_req (sf))
				sfmc_send_arp (sfmc, sf);

			decrement_arp_ttl (sf, 0);
			if (sf->arp_ttl == 0)
				sf->arp_state = ARP_PROBE;
			break;
		}
	}

	next_timer = jiffies + (1 * HZ);
	mod_timer (&sfmc->arp_timer, next_timer);
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

	/* init arp processor timer */
	init_timer_deferrable (&sfmc->arp_timer);
	sfmc->arp_timer.function = sfmc_arp_processor;
	sfmc->arp_timer.data = (unsigned long) sfmc;
	mod_timer (&sfmc->arp_timer, jiffies + (1 * HZ));

	/* add switchdev_ops for physical device netdev */
	dev->switchdev_ops = &sfmc_switchdev_ops;

	/* regsiter madcap ops */
	err = madcap_register_device (dev, &sfmc_madcap_ops);
	if (err < 0) {
		netdev_err (dev, "failed to register madcap_ops.\n");
		return err;
	}

	return 0;
}

int
sfmc_exit (struct sfmc *sfmc)
{
	/* stop arp processor.
	 * destroy sfmc_table and sfmc_fib tree.
	 */

	del_timer_sync (&sfmc->arp_timer);
	sfmc_table_destroy (sfmc);
	sfmc_fib_destroy (sfmc);

	return 0;
}
