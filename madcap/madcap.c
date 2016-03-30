/*
 * madcap.c
 * Encapsulation Madness!!
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/vermagic.h>
#include <linux/rwlock.h>
#include <net/sock.h>
#include <net/genetlink.h>
#include <net/net_namespace.h>
#include <net/netns/generic.h>
#include <madcap.h>

#ifndef DEBUG
#define DEBUG
#endif

/* common prefix used by pr_<> macros */
#undef pr_fmt
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt "\n"

#ifndef DEBUG
#undef pr_debug
#define pr_debug(fmt, ...) \
	printk(KERN_INFO pr_fmt(fmt), ##__VA_ARGS__)
#endif


#define MADCAP_NAME	"madcap"
#define MADCAP_VERSION	"0.0.0"

MODULE_VERSION (MADCAP_VERSION);
MODULE_LICENSE ("GPL");
MODULE_AUTHOR ("upa@haeena.net");


/* Per netnamespace parameters */
static unsigned int madcap_net_id;
#define MADCAPDEV_PERNET_NUM	16

struct madcap_net {
	rwlock_t	lock;
	struct madcap_ops *ops[MADCAPDEV_PERNET_NUM];
	struct net_device *dev[MADCAPDEV_PERNET_NUM];
};


/* XXX: Many features such as driver/device lock and resource
 * allocation like switchdev trans.ph_prepare phasing are needed. but
 * not implemented...
 */

int
madcap_queue_xmit (struct sk_buff *skb, struct net_device *dev)
{
	/* XXX: physical device is also shared resource for multiple
	 * pseudo interfaces for overlays (e.g, multiple vxlan
	 * interfaces for each VNI). So, some queueing and locking
	 * between pseudo interfaces and a physical interface are
	 * needed. HOWEVER, this model shouled be more considered.
	 */

	skb->dev = dev;
	return dev_queue_xmit (skb);
}
EXPORT_SYMBOL (madcap_queue_xmit);

int
madcap_acquire_dev (struct net_device *dev, struct net_device *vdev)
{
	struct madcap_ops *mc_ops;

	mc_ops = get_madcap_ops (dev);

	if (mc_ops && mc_ops->mco_acquire_dev)
		return mc_ops->mco_acquire_dev (dev, vdev);

	return -EOPNOTSUPP;
}
EXPORT_SYMBOL (madcap_acquire_dev);

int
madcap_release_dev (struct net_device *dev, struct net_device *vdev)
{
	struct madcap_ops *mc_ops;

	mc_ops = get_madcap_ops (dev);

	if (mc_ops && mc_ops->mco_release_dev)
		return mc_ops->mco_release_dev (dev, vdev);

	return -EOPNOTSUPP;
}
EXPORT_SYMBOL (madcap_release_dev);

/* XXX: */
#define __MADCAP_OBJ_DEFUN(funcname)                                    \
	int madcap_##funcname (struct net_device *dev,			\
			       struct madcap_obj *obj)			\
	{                                                               \
		struct madcap_ops *mc_ops;                              \
		mc_ops = get_madcap_ops (dev);				\
		if (mc_ops && mc_ops->mco_##funcname)			\
			return mc_ops->mco_##funcname (dev, obj);	\
									\
		return -EOPNOTSUPP;					\
	}								\
	EXPORT_SYMBOL (madcap_##funcname);				\

__MADCAP_OBJ_DEFUN(llt_cfg);
__MADCAP_OBJ_DEFUN(llt_entry_add);
__MADCAP_OBJ_DEFUN(llt_entry_del);
__MADCAP_OBJ_DEFUN(udp_cfg);

#define __MADCAP_GET_DEFUN(funcname)					\
	struct madcap_obj * madcap_##funcname (struct net_device *dev)	\
	{                                                               \
		struct madcap_ops *mc_ops;                              \
		mc_ops = get_madcap_ops (dev);				\
		if (mc_ops && mc_ops->mco_##funcname) {			\
			return mc_ops->mco_##funcname (dev);		\
		}							\
		return NULL;						\
	}								\
	EXPORT_SYMBOL (madcap_##funcname);				\

__MADCAP_GET_DEFUN(llt_config_get);
__MADCAP_GET_DEFUN(udp_config_get);



struct madcap_obj *
madcap_llt_entry_dump (struct net_device *dev, struct netlink_callback *cb)
{
	struct madcap_ops *mc_ops;

	mc_ops = get_madcap_ops (dev);

	if (mc_ops && mc_ops->mco_llt_entry_dump)
		return mc_ops->mco_llt_entry_dump (dev, cb);

	return NULL;
}



/* Generic Netlink MadCap family */

static struct genl_family madcap_nl_family = {
	.id		= GENL_ID_GENERATE,
	.name		= MADCAP_GENL_NAME,
	.version	= MADCAP_GENL_VERSION,
	.maxattr	= MADCAP_ATTR_MAX,
};

static struct nla_policy madcap_nl_policy[MADCAP_ATTR_MAX + 1] = {
	[MADCAP_ATTR_NONE]      = { .type = NLA_UNSPEC, },
	[MADCAP_ATTR_IFINDEX]	= { .type = NLA_U32, },
	[MADCAP_ATTR_OBJ_CONFIG]= { .type = NLA_BINARY,
				    .len = sizeof (struct madcap_obj_config)},
	[MADCAP_ATTR_OBJ_ENTRY]	= { .type = NLA_BINARY,
				    .len = sizeof (struct madcap_obj_entry)},
	[MADCAP_ATTR_OBJ_UDP]	= { .type = NLA_BINARY,
				    .len = sizeof (struct madcap_obj_udp)},
};

static int
genl_madcap_obj_send (struct sk_buff *skb, u32 portid, u32 seq,
		      int flags, u8 cmd, struct madcap_obj *obj, u32 ifindex)
{
	void *hdr;
	int attr, len;

	hdr = genlmsg_put (skb, portid, seq, &madcap_nl_family, flags, cmd);

	if (!hdr)
		return -EMSGSIZE;

	switch (obj->id) {
	case MADCAP_OBJ_ID_LLT_CONFIG :
		len = sizeof (struct madcap_obj_config);
		attr = MADCAP_ATTR_OBJ_CONFIG;
		break;
	case MADCAP_OBJ_ID_LLT_ENTRY :
		len = sizeof (struct madcap_obj_entry);
		attr = MADCAP_ATTR_OBJ_ENTRY;
		break;
	case MADCAP_OBJ_ID_UDP :
		len = sizeof (struct madcap_obj_udp);
		attr = MADCAP_ATTR_OBJ_UDP;
		break;
	default :
		pr_debug ("%s: unknonw madcap_obj id %d", __func__, obj->id);
		goto nla_put_failure;
	}

	if (nla_put (skb, attr, len, obj))
		goto nla_put_failure;

	if (ifindex)
		if (nla_put_u32 (skb, MADCAP_ATTR_IFINDEX, ifindex))
			goto nla_put_failure;

	genlmsg_end (skb, hdr);

	return 0;

nla_put_failure:
	genlmsg_cancel (skb, hdr);
	return -1;
}

static int
madcap_nl_cmd_llt_config (struct sk_buff *skb, struct genl_info *info)
{
	int ret;
	u32 ifindex;
	struct net_device *dev;
	struct madcap_obj_config obj_cfg;
	struct net *net = sock_net (skb->sk);

	memset (&obj_cfg, 0, sizeof (obj_cfg));

	if (!info->attrs[MADCAP_ATTR_IFINDEX]) {
		pr_debug ("%s: no ifindex", __func__);
		return -EINVAL;
	}
	ifindex = nla_get_u32 (info->attrs[MADCAP_ATTR_IFINDEX]);

	dev = __dev_get_by_index (net, ifindex);
	if (!dev) {
		pr_debug ("%s: device not found for %u", __func__, ifindex);
		return -ENODEV;
	}

	/* config offset */
	if (!info->attrs[MADCAP_ATTR_OBJ_CONFIG]) {
		pr_debug ("%s: no config object", __func__);
		return -EINVAL;
	}
	nla_memcpy (&obj_cfg, info->attrs[MADCAP_ATTR_OBJ_CONFIG],
		    sizeof (obj_cfg));

	ret = madcap_llt_cfg (dev, MADCAP_OBJ (obj_cfg));

	if (ret < 0) {
		pr_info ("failed to config llt");
		return ret;
	}

	return ret;
}

static int
madcap_nl_cmd_llt_config_dump (struct sk_buff *skb,
			       struct netlink_callback *cb)
{
	int n, rc, idx, cnt;
	u32 ifindex;
	struct net *net = sock_net (skb->sk);
	struct madcap_net *madnet = net_generic (net, madcap_net_id);
	struct nlattr *attrs[MADCAP_ATTR_MAX + 1];
	struct madcap_obj *obj;

	/* XXX: kernel 4.0 later, use genlmsg_parse() */
	rc = nlmsg_parse (cb->nlh, madcap_nl_family.hdrsize + GENL_HDRLEN,
			  attrs, MADCAP_ATTR_MAX, madcap_nl_policy);
	if (rc < 0) {
		pr_debug ("%s: failed to parse cb->nlh", __func__);
		return -1;
	}

	idx = cb->args[0];
	obj = NULL;

	ifindex = (attrs[MADCAP_ATTR_IFINDEX]) ?
		nla_get_u32 (attrs[MADCAP_ATTR_IFINDEX]) : 0;

	/* send all or specified madcap device config */
	for (n = 0, cnt = 0; n <MADCAPDEV_PERNET_NUM; n++) {

		if (!madnet->ops[n] || !madnet->dev[n])
			continue;

		if (ifindex && madnet->dev[n]->ifindex != ifindex)
			continue;

		if (idx > cnt) {
			cnt++;
			continue;
		}

		obj = madcap_llt_config_get (madnet->dev[n]);
		ifindex = madnet->dev[n]->ifindex;
		break;
	}

	if (obj) {
		struct madcap_obj_config *oc;
		oc = MADCAP_OBJ_CONFIG (obj);
		rc = genl_madcap_obj_send (skb, NETLINK_CB (cb->skb).portid,
					   cb->nlh->nlmsg_seq, NLM_F_MULTI,
					   MADCAP_CMD_LLT_CONFIG_GET, obj,
					   ifindex);
		if (rc < 0)
			return rc;
	}

	cb->args[0] = cnt + 1;
	return skb->len;
}


static int
madcap_nl_cmd_llt_entry_add (struct sk_buff *skb, struct genl_info *info)
{
	u32 ifindex;
	struct net_device *dev;
	struct madcap_obj_entry obj_ent;
	struct net *net = sock_net (skb->sk);

	if (!info->attrs[MADCAP_ATTR_IFINDEX]) {
		pr_debug ("%s: no ifindex", __func__);
		return -EINVAL;
	}
	ifindex = nla_get_u32 (info->attrs[MADCAP_ATTR_IFINDEX]);

	dev = __dev_get_by_index (net, ifindex);
	if (!dev) {
		pr_debug ("%s: device not found for %u", __func__, ifindex);
		return -ENODEV;
	}

	if (!info->attrs[MADCAP_ATTR_OBJ_ENTRY]) {
		pr_debug ("%s: no entry object", __func__);
		return -EINVAL;
	}
	nla_memcpy (&obj_ent, info->attrs[MADCAP_ATTR_OBJ_ENTRY],
		    sizeof (obj_ent));

	return madcap_llt_entry_add (dev, MADCAP_OBJ (obj_ent));
}

static int
madcap_nl_cmd_llt_entry_del (struct sk_buff *skb, struct genl_info *info)
{
	u32 ifindex;
	struct net_device *dev;
	struct madcap_obj_entry obj_ent;
	struct net *net = sock_net (skb->sk);

	if (!info->attrs[MADCAP_ATTR_IFINDEX]) {
		pr_debug ("%s: no ifindex", __func__);
		return -EINVAL;
	}
	ifindex = nla_get_u32 (info->attrs[MADCAP_ATTR_IFINDEX]);

	dev = __dev_get_by_index (net, ifindex);
	if (!dev) {
		pr_debug ("%s: device not found for %u", __func__, ifindex);
		return -ENODEV;
	}

	if (!info->attrs[MADCAP_ATTR_OBJ_ENTRY]) {
		pr_debug ("%s: no entry object", __func__);
		return -EINVAL;
	}
	nla_memcpy (&obj_ent, info->attrs[MADCAP_ATTR_OBJ_ENTRY],
		    sizeof (obj_ent));

	return madcap_llt_entry_del (dev, MADCAP_OBJ (obj_ent));
}

static int
madcap_nl_cmd_llt_entry_dump (struct sk_buff *skb, struct netlink_callback *cb)
{
	int rc, idx, cnt, n;
	u32 ifindex, send_ifindex;
	struct nlattr *attrs[MADCAP_ATTR_MAX + 1];
	struct net *net = sock_net (skb->sk);
	struct madcap_net *madnet = net_generic (net, madcap_net_id);
	struct madcap_obj *obj;
	struct madcap_obj_entry *obj_ent;

	/* XXX: kernel 4.0 later, use genlmsg_parse() */
	rc = nlmsg_parse (cb->nlh, madcap_nl_family.hdrsize + GENL_HDRLEN,
			  attrs, MADCAP_ATTR_MAX, madcap_nl_policy);
	if (rc < 0) {
		pr_debug ("%s: failed to parse cb->nlh.", __func__);
		return 0;
	}

	ifindex = (attrs[MADCAP_ATTR_IFINDEX]) ?
		nla_get_u32 (attrs[MADCAP_ATTR_IFINDEX]) : 0;

	idx = cb->args[1];
	obj_ent = NULL;

	for (n = 0, cnt = 0; n < MADCAPDEV_PERNET_NUM; n++) {

		if (!madnet->ops[n] || !madnet->dev[n])
			continue;

		if (ifindex && madnet->dev[n]->ifindex != ifindex)
			continue;

		if (idx > cnt) {
			cnt++;
			continue;
		}

		obj = madcap_llt_entry_dump (madnet->dev[n], cb);
		if (obj == NULL) {
			/* next device */
			cnt = 0;
			n = -1;
			idx++;
			cb->args[0] = 0;
			cb->args[1] = idx;
			send_ifindex = 0;
			obj_ent = NULL;
			continue;
		}

		obj_ent = MADCAP_OBJ_ENTRY (obj);
		send_ifindex = madnet->dev[n]->ifindex;

		break;
	}

	if (obj_ent) {
		rc = genl_madcap_obj_send (skb, NETLINK_CB (cb->skb).portid,
					   cb->nlh->nlmsg_seq, NLM_F_MULTI,
					   MADCAP_CMD_LLT_ENTRY_GET,
					   MADCAP_OBJ (*obj_ent),
					   send_ifindex);
		if (rc < 0)
			return -1;
	}

	return skb->len;
}

static int
madcap_nl_cmd_udp_config (struct sk_buff *skb, struct genl_info *info)
{
	u32 ifindex;
	struct net_device *dev;
	struct madcap_obj_udp obj_udp;
	struct net *net = sock_net (skb->sk);

	if (!info->attrs[MADCAP_ATTR_IFINDEX]) {
		pr_debug ("%s: no ifindex", __func__);
		return -EINVAL;
	}
	ifindex = nla_get_u32 (info->attrs[MADCAP_ATTR_IFINDEX]);

	dev = __dev_get_by_index (net, ifindex);
	if (!dev) {
		pr_debug ("%s: device not found for %u", __func__, ifindex);
		return -ENODEV;
	}

	if (!info->attrs[MADCAP_ATTR_OBJ_UDP]) {
		pr_debug ("%s: no udp object", __func__);
		return -EINVAL;
	}
	nla_memcpy (&obj_udp, info->attrs[MADCAP_ATTR_OBJ_UDP],
		    sizeof (obj_udp));


	return madcap_udp_cfg (dev, MADCAP_OBJ (obj_udp));
}

static int
madcap_nl_cmd_udp_config_dump (struct sk_buff *skb,
			       struct netlink_callback *cb)
{
	int n, rc, idx, cnt;
	u32 ifindex;
	struct net *net = sock_net (skb->sk);
	struct madcap_net *madnet = net_generic (net, madcap_net_id);
	struct nlattr *attrs[MADCAP_ATTR_MAX + 1];
	struct madcap_obj *obj;

	/* XXX: kernel 4.0 later, use genlmsg_parse() */
	rc = nlmsg_parse (cb->nlh, madcap_nl_family.hdrsize + GENL_HDRLEN,
			  attrs, MADCAP_ATTR_MAX, madcap_nl_policy);
	if (rc < 0) {
		pr_debug ("%s: failed to parse cb->nlh", __func__);
		return -1;
	}

	idx = cb->args[0];
	obj = NULL;

	ifindex = (attrs[MADCAP_ATTR_IFINDEX]) ?
		nla_get_u32 (attrs[MADCAP_ATTR_IFINDEX]) : 0;

	/* send all or specified madcap device config */
	for (n = 0, cnt = 0; n < MADCAPDEV_PERNET_NUM; n++) {

		if (!madnet->ops[n] || !madnet->dev[n])
			continue;

		if (ifindex && madnet->dev[n]->ifindex != ifindex)
			continue;

		if (idx > cnt) {
			cnt++;
			continue;
		}

		obj = madcap_udp_config_get (madnet->dev[n]);
		ifindex = madnet->dev[n]->ifindex;
		break;
	}

	if (obj) {
		rc = genl_madcap_obj_send (skb, NETLINK_CB (cb->skb).portid,
					   cb->nlh->nlmsg_seq, NLM_F_MULTI,
					   MADCAP_CMD_UDP_CONFIG_GET, obj,
					   ifindex);
		if (rc < 0)
			return -1;
	}

	cb->args[0] = cnt + 1;
	return skb->len;
}

static struct genl_ops madcap_nl_ops[] = {
	{
		.cmd	= MADCAP_CMD_LLT_CONFIG,
		.doit	= madcap_nl_cmd_llt_config,
		.policy	= madcap_nl_policy,
	},
	{
		.cmd	= MADCAP_CMD_LLT_CONFIG_GET,
		.dumpit	= madcap_nl_cmd_llt_config_dump,
		.policy	= madcap_nl_policy,
	},
	{
		.cmd	= MADCAP_CMD_LLT_ENTRY_ADD,
		.doit	= madcap_nl_cmd_llt_entry_add,
		.policy	= madcap_nl_policy,
	},
	{
		.cmd	= MADCAP_CMD_LLT_ENTRY_DEL,
		.doit	= madcap_nl_cmd_llt_entry_del,
		.policy	= madcap_nl_policy,
	},
	{
		.cmd	= MADCAP_CMD_LLT_ENTRY_GET,
		.dumpit	= madcap_nl_cmd_llt_entry_dump,
		.policy	= madcap_nl_policy,
	},
	{
		.cmd	= MADCAP_CMD_UDP_CONFIG,
		.doit	= madcap_nl_cmd_udp_config,
		.policy	= madcap_nl_policy,
	},
	{
		.cmd	= MADCAP_CMD_UDP_CONFIG_GET,
		.dumpit	= madcap_nl_cmd_udp_config_dump,
		.policy	= madcap_nl_policy,
	},
};




/* XXX: Per net namespace subsystem, is
 * ONLY used for finding corresponding madcap_ops for a net_device.
 * NOT needed if madcap_ops was a member of struct net_device.
 * DIRTY hack in order to avoid mainline kernel modification.
 *    because make-kpkg is too slow...
 */


static __net_init int
madcap_init_net (struct net *net)
{
	struct madcap_net *madnet = net_generic (net, madcap_net_id);

	memset (madnet, 0, sizeof (*madnet));
	rwlock_init (&madnet->lock);

	return 0;
}

static __net_exit void
madcap_exit_net (struct net *net)
{
	return;
}

static struct pernet_operations madcap_net_ops = {
	.init	= madcap_init_net,
	.exit	= madcap_exit_net,
	.id	= &madcap_net_id,
	.size	= sizeof (struct madcap_net),
};

inline struct madcap_ops *
get_madcap_ops (struct net_device *dev)
{
	/* XXX: if madcap_ops was a member of struct net_device,
	 * This did "return dev->madcap_ops;".
	 */
	
	int n;
	struct madcap_net *madnet = net_generic (dev_net (dev), madcap_net_id);

	for (n = 0; n < MADCAPDEV_PERNET_NUM; n++) {
		if (madnet->dev[n] == dev) {
			return madnet->ops[n];
		}
	}

	return NULL;
}
EXPORT_SYMBOL (get_madcap_ops);

int
madcap_register_device (struct net_device *dev, struct madcap_ops *mc_ops)
{
	int n;
	struct madcap_net *madnet = net_generic (dev_net (dev), madcap_net_id);

	/*XXX: get_madcap_ops should lock? */
	if (get_madcap_ops (dev))
		return -EEXIST;
	
	/* skb->dst is used to distinguish that packet is xmited from
	 * encaped device. see sfmc_encap_packet. */
	netif_keep_dst (dev);

	write_lock_bh (&madnet->lock);
	for (n = 0; n < MADCAPDEV_PERNET_NUM; n++) {
		if (madnet->dev[n] == NULL) {
			madnet->ops[n] = mc_ops;
			madnet->dev[n] = dev;
			break;
		}
	}
	write_unlock_bh (&madnet->lock);

	if (!(n < MADCAPDEV_PERNET_NUM)) {
		pr_info ("sorry, max number of madcap dev in a netns is %d",
			 MADCAPDEV_PERNET_NUM);
		return -ENOMEM;
	}

	return 0;
}
EXPORT_SYMBOL (madcap_register_device);

int
madcap_unregister_device (struct net_device *dev)
{
	int n;
	struct madcap_net *madnet = net_generic (dev_net (dev), madcap_net_id);

	write_lock_bh (&madnet->lock);
	for (n = 0; n < MADCAPDEV_PERNET_NUM; n++) {
		if (madnet->dev[n] == dev) {
			madnet->dev[n] = NULL;
			madnet->ops[n] = NULL;
		}
	}
	write_unlock_bh (&madnet->lock);

	if (!(n < MADCAPDEV_PERNET_NUM)) {
		return -ENOENT;
	}

	return 0;
}
EXPORT_SYMBOL (madcap_unregister_device);


static int
__init madcap_init_module (void)
{
	int rc;

	rc = register_pernet_subsys (&madcap_net_ops);
	if (rc < 0)
		goto netns_failed;

	rc = genl_register_family_with_ops (&madcap_nl_family, madcap_nl_ops);
	if (rc < 0)
		goto genl_failed;

	pr_info ("madcap (%s) is loaded.", MADCAP_VERSION);
	return 0;

genl_failed:
	unregister_pernet_subsys (&madcap_net_ops);
netns_failed:
	return rc;
}
module_init (madcap_init_module);

static void __exit
madcap_exit_module (void)
{
	genl_unregister_family (&madcap_nl_family);
	unregister_pernet_subsys(&madcap_net_ops);

	pr_info ("madcap (%s) is unloaded.", MADCAP_VERSION);
	return;
}
module_exit (madcap_exit_module);
