

#include "sfmc.h"

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

	memset (st, 0, sizeof (st));

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
	kfree_rcu (st);
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



/* madcap_ops functions */

static int
sfmc_acquire_dev (struct net_device *dev)
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

	if (memcmp (oc, sfmc->oc), sizeof (*oc) != 0) {
		/* offset or length is changed. drop all table entry. */
		sfmc_lock (sfmc);
		sfmc_table_destroy (sfmc);
		sfmc_unlock (sfmc);
	}
}

static struct madcap_obj *
sfmc_llt_config_get (struct net_device *dev)
{
	struct sfmc = netdev_get_sfmc (dev);
	return MADCAP_OBJ (sfmc->oc);
}

static int
sfmc_llt_entry_add (struct net_device *dev, struct madcap_obj *obj)
{
	struct sfmc_table *st;
	struct sfmc *sfmc = netdev_get_sfmc (dev);
	struct madcap_obj_entry *oe = MADCAP_OBJ_ENTRY (obj);
	
	st = sfmc_table_faind (sfmc, oe->id);
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
	
	st = sfmc_table_faind (sfmc, oe->id);
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

	for (n = 0, cnt = 0; n < RAVEN_HASH_SIZE; n++) {
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

static int
sfmc_udp_config_get (struct net_device *dev)
{
	struct sfmc *sfmc = netdev_get_sfmc (dev);
	return MADCAP_OBJ(sfmc->ou);
}


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

static int
sfmc_encap_packet (struct sk_buff *skb, struct net_device *dev)
{
	int err, headroom;
	__u64 id;
	struct sfmc *sfmc = netdv_get_sfmc (dev);
	struct sfmc_table *st;

	id = extract_id_from_packet (skb, &sfmc->oc);
	st = sfmc_table_find (sfmc, id);

	/* find default destination */
	st = (st) ? st : sfmc_table_find (sfmc, id);

	if (!st)
		return -ENOENT;

	/* encap udp */

	/* encap ip */

	/* arp resolve and set ethernet header */
		
	return 0;
}
