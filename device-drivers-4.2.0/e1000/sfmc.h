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
#include "patricia.h"	/* patricia trie */


#define SFMC_HASH_BITS	8
#define SFMC_HASH_SIZE	(1 << SFMC_HASH_BITS)
#define SFMC_VDEV_MAX	16


/* madcap table and config structure */
struct sfmc {

	struct net_device 	*dev;	/* physical device */
	rwlock_t		lock;

	u64			id;	/* h/w id for switchdev */

	struct net_device	*vdev[SFMC_VDEV_MAX];	/* acquiring device */

	struct hlist_head	sfmc_table[SFMC_HASH_SIZE]; /* sfmc_table */
	struct list_head	fib_list;	/* sfmc_fib list */
	patricia_tree_t		*fib_tree;	/* ipv4 fib table
						 * struct sfmc_fib */
	struct madcap_obj_udp		ou;	/* udp encap config	*/
	struct madcap_obj_config	oc;	/* offset and length */
};



/* sfmc structure constructor and destructor.
 * inserted before regsiter_netdev */
int sfmc_init (struct sfmc *sfmc, struct net_device *dev);
int sfmc_exit (struct sfmc *sfmc);

/* add (udp), ip, and ethernet header in accordance with llt */
int sfmc_encap_packet (struct sk_buff *skb, struct net_device *dev);


#endif
