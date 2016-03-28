/*
 * Software based Madcap Implementation.
 * 
 * This codes enables easy addition of software madcap acceleration to
 * exsiting ethernet card device drivers. 
 *
 * 1. add "struct sfmc sfmc" to private data of struct net_device
 * 2. #define SFMC_NETDEV_PRIV your_structure_name_of_netdev_priv
 * 3. register madcap_ops with sfmc_ operations.
 */

#ifndef _SFMC_MADCAP_H_
#define _SFMC_MADCAP_H_

#include <linux/hash.h>
#include <linux/rwlock.h>
#include <linux/rculist.h>
#include <madcap.h>


#define SFMC_NETDEV_PRIV       _structure_name_here_

#define netdev_get_sfmc(dev) \
	&(((struct SFMC_NETDEV_PRIV *)(netdev_priv (dev)))->sfmc)


/* struct madcap_ops */
static int sfmc_acquire_dev (struct net_device *dev);
static int sfmc_release_dev (struct net_device *dev, struct net_device *vdev);

static int sfmc_llt_cfg (struct net_device *dev, struct madcap_obj *obj);
static struct madcap_obj * sfmc_llt_config_get (struct net_device *dev);

static int sfmc_udp_cfg (struct net_device *dev, struct madcap_obj *obj);
static int sfmc_udp_config_get (struct net_device *dev);

static int sfmc_llt_entry_add (struct net_device *dev, struct madcap_obj *obj);
static int sfmc_llt_entry_del (struct net_device *dev, struct madcap_obj *obj);

struct madcap_obj * sfmc_llt_entry_dump (struct net_device *dev,
					 struct netlink_callback *cb);


/* madcap table and config structure */
struct sfmc {

	struct net_device *dev;	/* physical device */

#define SFMC_HASH_BITS	8
#define SFMC_HASH_SIZE	(1 << SFMC_HASH_BITS)

	struct hlist_head	sfmc_table[SFMC_HASH_SIZE]; /* sfmc_table */
	rwlock_t		lock;

	struct madcap_obj_udp		ou;	/* udp encap config	*/
	struct madcap_obj_config	oc;	/* offset and length */
};

struct sfmc_table {
	struct hlist_node	hlist;	/* sfmc->sfmc_table[] */
	struct rcu_head		rcu;
	struct sfmc		*sfmc;
	unsigned long		updated;

	struct madcap_obj_entry	oe;
};

#define sfmc_lock(sfmc) write_lock_bh(&sfmc->lock)
#define sfmc_unlock(sfmc) write_lock_bh(&sfmc->lock)

#endif
