
#include <linux/etherdevice.h>
#include <linux/if_ether.h> 
#include <linux/if_arp.h>

#include "sfmc.h"

#include "ixgbe.h"


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
static void
sfmc_fib_add (struct sfmc *sfmc, __be32 prefix, u8 len, __be32 gateway)
{
	
}

static struct sfmc_fib *
sfmc_fib_find_exact (struct sfmc *sfmc, __be32 prefix, u8 len)
{
	return NULL;
}

static struct sfmc_fib *
sfmc_fib_find_best (struct sfmc *sfmc, __be32 prefix, u8 len)
{
	return NULL;
}

static void
sfmc_fib_delete (struct sfmc_fib *sf)
{
	if (!sf)
		return;

	list_del_rcu (&sf->list);
	kfree (sf->prefix);
	kfree_rcu (sf, rcu);
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
	return 0;
}

static int
sfmc_release_dev (struct net_device *dev, struct net_device *vdev)
{
	return 0;
}

static int
sfmc_llt_cfg (struct net_device *dev, struct madcap_obj *obj)
{
	struct sfmc *sfmc = netdev_get_sfmc (dev);
	struct madcap_obj_config *oc = MADCAP_OBJ_CONFIG (obj);

	if (memcmp (oc, &sfmc->oc, sizeof (*oc) != 0)) {
		/* offset or length is changed. drop all table entry. */
		sfmc_lock (sfmc);
		sfmc_table_destroy (sfmc);
		sfmc_unlock (sfmc);
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

static int
sfmc_encap_packet (struct sk_buff *skb, struct net_device *dev)
{
	__u64 id;
	struct sfmc *sfmc = netdev_get_sfmc (dev);
	struct sfmc_table *st;
	struct sfmc_fib *sf;
	struct iphdr *iph;
	struct udphdr *uh;
	struct ethhdr *eth;

	id = extract_id_from_packet (skb, &sfmc->oc);
	st = sfmc_table_find (sfmc, id);

	/* find default destination */
	st = (st) ? st : sfmc_table_find (sfmc, id);

	if (!st)
		return -ENOENT;

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
	if (!sf)
		return -ENOENT;

#define ARP_STATE_XMITABLE(sf) \
	(sf->arp_state == ARP_REACHABLE || sf->arp_state == ARP_REPROBE)
#define maccopy(a, b)					\
	do {						\
		b[0] = a[0]; b[1] = a[1]; b[2] = a[2];	\
		b[3] = a[3]; b[4] = a[4]; b[5] = a[5];	\
	} while (0)

	if (!ARP_STATE_XMITABLE(sf))
		return -ENOENT;

	eth = (struct ethhdr *) __skb_push (skb, sizeof (*eth));
	skb_reset_mac_header (skb);
	maccopy (sf->mac, eth->h_dest);
	maccopy (dev->perm_addr, eth->h_source);
	eth->h_proto = htons (ETH_P_IP);

	return 0;
}


/* arp related code */

void
sfmc_recv_arp (struct sfmc * sfmc, struct sk_buff *skb)
{
	
}

static void
sfmc_send_arp (struct sfmc *sfmc, struct sfmc_fib *sf)
{
	/* make and send arp requeset for sf->mac via dev */

	struct net_device *dev = sfmc->dev;

	struct sk_buff *skb;
	struct ethhdr *eth;
	struct arphdr *arp;

	struct arpreq {
		unsigned char ar_sha[ETH_ALEN];
		unsigned char ar_sip[4];
		unsigned char ar_tha[ETH_ALEN];
		unsigned char ar_tip[4];
	};
	struct arpreq *req;

	size_t size = sizeof (*eth) + sizeof (*arp) + sizeof (*req);

	skb = alloc_skb (size, GFP_ATOMIC);
	if (!skb) {
		pr_info ("failed to alloc skb for arp req.\n");
		return;
	}

	/* build arp header */
	skb->protocol = htons (ETH_P_ARP);
	arp = (struct arphdr *) skb_put (skb, sizeof (*arp));
	arp->ar_hrd	= htons (ARPHRD_ETHER);
	arp->ar_pro 	= htons (ETH_P_IP);
	arp->ar_hln	= ETH_ALEN;
	arp->ar_pln	= 4;
	arp->ar_op	= htons (ARPOP_REQUEST);

	req = (struct arpreq *) skb_put (skb, sizeof (*req));
	memset (req, 0, sizeof (*req));
	memcpy (req->ar_sha, dev->perm_addr, ETH_ALEN);
	memcpy (req->ar_sip, &sfmc->oc.src, sizeof (sfmc->oc.src));
	memcpy (req->ar_tip, &sf->gateway, sizeof (sf->gateway));

	skb->dev = dev;
	dev_queue_xmit (skb);
}



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
