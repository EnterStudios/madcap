
/* madcap.h */

#ifndef _MADCAP_H_
#define _MADCAP_H_

/*
 * Encapsulation Madness!!
 * 
 * - Transmit protocol specific encaped packet.
 * - Receive encaped packet to protocol driver pseudo interface.
 * - Acquire Tunnel Overlay device (with specified queue?)
 * - Release Tunnel Overlay device (with specified queue?)
 * - Config locator look-up table :
 *   - set offset/length.
 *   - add entry.
 *   - delete entry.
 */

enum madcap_obj_id {
	MADCAP_OBJ_ID_UNDEFINED,
	MADCAP_OBJ_ID_LLT_CONFIG,
	MADCAP_OBJ_ID_LLT_ENTRY,
	MADCAP_OBJ_ID_UDP,
};

struct madcap_obj {
	enum madcap_obj_id id;
	__u16	tb_id;		/* table id (queue?) */
};

struct madcap_obj_config {
	struct madcap_obj obj;
	__u16	offset;
	__u16	length;
	__u8	proto;	/* IP protocol number. */
	__be32	src;	/* XXX: src ip address. should track ifa? */
};

struct madcap_obj_entry {
	struct madcap_obj obj;
	__u64	id;	/* identifier of dst */
	__be32	dst;	/* dst ipv4 address (locator) */
};

struct madcap_obj_udp {
	struct madcap_obj obj;
	int	encap_enable;
	int	src_hash_enable;
	__be16	dst_port;
	__be16	src_port;
};

#define MADCAP_OBJ(obj_)	&((obj_).obj)
#define MADCAP_IFINDEX(obj_) (obj_)->obj.ifindex
#define MADCAP_OBJ_CONFIG(obj)	\
	container_of (obj, struct madcap_obj_config, obj)
#define MADCAP_OBJ_ENTRY(obj)	\
	container_of (obj, struct madcap_obj_entry, obj)
#define MADCAP_OBJ_UDP(obj)	\
	container_of (obj, struct madcap_obj_udp, obj)


#ifdef __KERNEL__

#include <linux/netdevice.h>
#include <linux/netlink.h>

struct madcap_ops {
	int		(*mco_if_rx) (struct sk_buff *skb);	/* ??? */

	/* vdev (pseudo NIC) acquires/releases dev (physical tunnel device) */
	int		(*mco_acquire_dev) (struct net_device *dev,
					     struct net_device *vdev);
	int		(*mco_release_dev) (struct net_device *dev,
					    struct net_device *vdev);

	int		(*mco_llt_cfg) (struct net_device *dev,
					struct madcap_obj *obj);

	int		(*mco_llt_entry_add) (struct net_device *dev,
					      struct madcap_obj *obj);
	int		(*mco_llt_entry_del) (struct net_device *dev,
					      struct madcap_obj *obj);

	int		(*mco_udp_cfg) (struct net_device *dev,
					struct madcap_obj *obj);

	struct madcap_obj *(*mco_llt_entry_dump) (struct net_device *dev,
						  struct netlink_callback *cb);

	struct madcap_obj *(*mco_llt_config_get) (struct net_device *dev);
	struct madcap_obj *(*mco_udp_config_get) (struct net_device *dev);
};


/* prototypes for madcap operations */

/*	madcap_queue_xmit
 *	@skb : transmiting packet encapsulated in protocol specific header(s)
 *	@dev : madcap capable physical device
 */
int madcap_queue_xmit (struct sk_buff *skb, struct net_device *dev);

int madcap_acquire_dev (struct net_device *dev, struct net_device *vdev);
int madcap_release_dev (struct net_device *dev, struct net_device *vdev);

int madcap_llt_cfg (struct net_device *dev, struct madcap_obj *obj);

int madcap_llt_entry_add (struct net_device *dev, struct madcap_obj *obj);
int madcap_llt_entry_del (struct net_device *dev, struct madcap_obj *obj);

int madcap_udp_cfg (struct net_device *dev, struct madcap_obj *obj);

/* entry dump skb and cb is generic netlink. */
struct madcap_obj *  madcap_llt_entry_dump (struct net_device *dev,
					    struct netlink_callback *cb);

struct madcap_obj * madcap_llt_config_get (struct net_device *dev);
struct madcap_obj * madcap_udp_config_get (struct net_device *dev);


/* dev<->madcap_ops mappings are maintained in a table in madcap.ko
 * in order to eliminate any modifications to mainline kernel.
 */
struct madcap_ops * get_madcap_ops (struct net_device *dev);
int madcap_register_device (struct net_device *dev, struct madcap_ops *mc_ops);
int madcap_unregister_device (struct net_device *dev);

#endif /* __KERNEL__ */




/* Generic Netlink, madcap family definition. */

/*
 * XXX: Acquire/release tonic device are called when overlay pseudo
 * device is created/destroyed. It will be implmeneted as a
 * modification for protocol drivers. Otherwise, notifier like
 * switchdev is needed ?
 */

#define MADCAP_GENL_NAME	"madcap"
#define MADCAP_GENL_VERSION	0x00

/* genl commands */
enum {
	MADCAP_CMD_LLT_CONFIG,
	MADCAP_CMD_LLT_CONFIG_GET,
	MADCAP_CMD_LLT_ENTRY_ADD,
	MADCAP_CMD_LLT_ENTRY_DEL,
	MADCAP_CMD_LLT_ENTRY_GET,
	MADCAP_CMD_UDP_CONFIG,
	MADCAP_CMD_UDP_CONFIG_GET,

	__MADCAP_CMD_MAX,
};
#define MADCAP_CMD_MAX	(__MADCAP_CMD_MAX - 1)

/* genl attr types */
enum {
	MADCAP_ATTR_NONE,		/* none */
	MADCAP_ATTR_IFINDEX,		/* ifindex of madcap device */
	MADCAP_ATTR_OBJ_CONFIG,		/* struct madcap_obj_config */
	MADCAP_ATTR_OBJ_ENTRY,		/* struct madcap_obj_entry */
	MADCAP_ATTR_OBJ_UDP,		/* struct madcap_obj_udp */

	__MADCAP_ATTR_MAX,
};
#define MADCAP_ATTR_MAX	(__MADCAP_ATTR_MAX - 1)


#endif /* _MADCAP_H_ */
