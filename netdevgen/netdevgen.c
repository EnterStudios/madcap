/*
 * netdev gen
 */


#include <linux/version.h>
#include <linux/module.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <linux/ip.h>
#include <linux/proc_fs.h>
#include <linux/string.h>
#include <asm/atomic.h>
#include <net/ip.h>
#include <net/route.h>
#include <net/net_namespace.h>

#ifdef OVBENCH
#include <linux/ovbench.h>
#endif

MODULE_AUTHOR ("upa@haeena.net");
MODULE_DESCRIPTION ("netdevgen");
MODULE_LICENSE ("GPL");

#define kthread_run_on_cpu(threadfn, data, cpu, namefmt, ...)		\
	({								\
	        struct task_struct *__k					\
			= kthread_create(threadfn, data,		\
					 namefmt, ## __VA_ARGS__);	\
		if (!IS_ERR(__k))					\
			wake_up_process(__k);				\
		kthread_bind(__k, cpu);					\
		__k;							\
	})

static bool ndg_thread_running = false;
static struct task_struct * ndg_tsk, * ndg_one_tsk;


static int pktlen __read_mostly = 46;
module_param_named (pktlen, pktlen, int, 0444);
MODULE_PARM_DESC (pktlen, "packet length - eth header and preamble");


static int measure_pps __read_mostly = 0;
module_param_named (measure_pps, measure_pps, int, 0444);
MODULE_PARM_DESC (measure_pps, "if 1, measure pps mode");


static __be32 srcip_ipip	= 0x010110AC; /* 172.16.1.1 */
static __be32 dstip_ipip	= 0x020110AC; /* 172.16.1.2 */

static __be32 srcip_gre		= 0x010210AC; /* 172.16.2.1 */
static __be32 dstip_gre		= 0x020210AC; /* 172.16.2.2 */

static __be32 srcip_gretap	= 0x010310AC; /* 172.16.3.1 */
static __be32 dstip_gretap	= 0x020310AC; /* 172.16.3.2 */

static __be32 srcip_vxlan	= 0x010410AC; /* 172.16.4.1 */
static __be32 dstip_vxlan	= 0x020410AC; /* 172.16.4.2 */

static __be32 srcip_nsh		= 0x010510AC; /* 172.16.5.1 */
static __be32 dstip_nsh		= 0x020510AC; /* 172.16.5.2 */

static __be32 srcip_noencap	= 0x010610AC; /* 172.16.6.1 */
static __be32 dstip_noencap	= 0x020610AC; /* 172.16.6.2 */

static __be32 srcip;
static __be32 dstip;

#ifdef OVBENCH
static int ovtype;
#endif

#define PROC_NAME "driver/netdevgen"

static atomic_t start;



static struct sk_buff *
netdevgen_build_packet (void)
{
	struct sk_buff * skb;
	struct iphdr * ip;
	struct udphdr * udp;
	struct flowi4 fl4;
	struct rtable * rt;
	struct net * net = get_net_ns_by_pid (1);

	if (!net) {
		pr_err ("failed to get netns by pid 1\n");
		return NULL;
	}

	/* alloc and build skb */
	skb = alloc_skb_fclone ((pktlen + 14), GFP_KERNEL);
	skb->protocol = htons (ETH_P_IP);
	skb_put (skb, pktlen);
	skb_set_network_header (skb, 0);
	skb_set_transport_header (skb, sizeof (*ip));

	memset(IPCB(skb), 0, sizeof(*IPCB(skb)));

	ip = (struct iphdr *) skb_network_header (skb);
	ip->ihl		= 5;
	ip->version	= 4;
	ip->tos		= 0;
	ip->tot_len	= htons (pktlen);
	ip->id		= 0;
	ip->frag_off	= 0;
	ip->ttl		= 12;
	ip->protocol	= IPPROTO_UDP;	
	ip->check	= 0;
	ip->saddr	= srcip;
	ip->daddr	= dstip;

	udp = (struct udphdr *) skb_transport_header (skb);
	udp->check	= 0;
	udp->source	= 0;
	udp->dest	= 0;
	udp->len	= htons (pktlen - sizeof (*ip));

#if LINUX_VERSION_CODE >= KERNEL_VERSION (4, 2, 0)
	__ip_select_ident(get_net_ns_by_pid (1),
			  ip, skb_shinfo(skb)->gso_segs ?: 1);
#else
	__ip_select_ident(ip, skb_shinfo(skb)->gso_segs ?: 1);	
#endif

	memset (&fl4, 0, sizeof (fl4));
	fl4.saddr = srcip;
	fl4.daddr = dstip;
	rt = ip_route_output_key (net, &fl4);
	if (IS_ERR (rt)) {
		pr_err ("no route to %pI4 from %pI4\n", &dstip, &srcip);
		return NULL;
	}
	skb_dst_drop (skb);
	skb_dst_set (skb, &rt->dst);

#ifdef OVBENCH
	skb->ovbench_encaped = 0;

	if (measure_pps)
		skb->ovbench_type = 0;
	else {
		skb->ovbench_type = ovtype;
		pr_info ("build packet %pI4->%pI4\n", &ip->saddr, &ip->daddr);
	}
#endif

	return skb;
}

static void
netdevgen_xmit_one (void)
{
	struct sk_buff * skb;

	skb = netdevgen_build_packet ();
	if (!skb) {
		pr_err ("skb build failed\n");
		return;
	}

#ifdef OVBENCH
	skb->first_xmit = rdtsc ();
#endif

	ip_local_out (skb);
}

static int
netdevgen_xmit_one_thread (void * arg)
{
	netdevgen_xmit_one ();

	return 0;
}

static int
netdevgen_thread (void * arg)
{
	struct sk_buff * skb, * pskb;

	ndg_thread_running = true;

	skb = netdevgen_build_packet ();
	if (!skb) {
		pr_err ("skb build failed\n");
		goto err_out;
	}

	while (!kthread_should_stop ()) {

		pskb = skb_clone (skb, GFP_KERNEL);
		if (!pskb) {
			pr_err ("failed to clone skb\n");
			continue;
		}

		ip_local_out (pskb);
	}

err_out:
	kfree_skb (skb);
	ndg_thread_running = false;

	pr_info ("netdevgen: thread finished\n");

	return 0;
}


static void
start_netdevgen_thread (void)
{
	unsigned int cpu = 0x00000002;	/* Only running on CPU 1 */

	if (ndg_tsk && ndg_thread_running) {
		pr_info ("netdecgen: thread already running\n");
		return;
	}
	
	ndg_tsk = kthread_run_on_cpu (netdevgen_thread, NULL, cpu,
				      "netdevgen");

	pr_info ("netdevgen: thread start\n");
}

static void
stop_netdevgen_thread (void)
{
	if (ndg_tsk && ndg_thread_running)
		kthread_stop (ndg_tsk);

	pr_info ("netdevgen: thread stop\n");
}

static void
start_netdevgen_xmit_one_thread (void)
{
	ndg_one_tsk = kthread_run (netdevgen_xmit_one_thread,
				   NULL, "netdevgen");
}

static void
start_stop (void)
{
	if (atomic_read (&start)) {
		pr_info ("netdevgen: start -> stop\n");
		stop_netdevgen_thread ();
	} else {
		pr_info ("netdevgen: stop -> start\n");
		start_netdevgen_thread ();
	}
}

static ssize_t
proc_read(struct file *fp, char *buf, size_t size, loff_t *off)
{
	pr_info ("proc read\n");

	//copy_to_user (buf, "stop!\n", size);
	start_stop ();
	return size;
}

static ssize_t
proc_write(struct file *fp, const char *buf, size_t size, loff_t *off)
{
	if (strncmp (buf, "xmit", 4) == 0) {

		start_netdevgen_xmit_one_thread ();

	} else if (strncmp (buf, "vxlan", 5) == 0) {

		srcip = srcip_vxlan;
		dstip = dstip_vxlan;
#ifdef OVBENCH
		ovtype = OVTYPE_VXLAN;
#endif
		if (!measure_pps)
			start_netdevgen_xmit_one_thread ();
		else
			start_netdevgen_thread ();
		
	} else if (strncmp (buf, "gretap", 6) == 0) {

		srcip = srcip_gretap;
		dstip = dstip_gretap;
#ifdef OVBENCH
		ovtype = OVTYPE_GRETAP;
#endif
		if (!measure_pps)
			start_netdevgen_xmit_one_thread ();
		else
			start_netdevgen_thread ();
		
	} else if (strncmp (buf, "gre", 3) == 0) {

		srcip = srcip_gre;
		dstip = dstip_gre;
#ifdef OVBENCH
		ovtype = OVTYPE_GRE;
#endif
		if (!measure_pps)
			start_netdevgen_xmit_one_thread ();
		else
			start_netdevgen_thread ();
		
	} else if (strncmp (buf, "ipip", 4) == 0) {

		srcip = srcip_ipip;
		dstip = dstip_ipip;
#ifdef OVBENCH
		ovtype = OVTYPE_IPIP;
#endif
		if (!measure_pps)
			start_netdevgen_xmit_one_thread ();
		else
			start_netdevgen_thread ();
		
	} else if (strncmp (buf, "nsh", 3) == 0) {

		srcip = srcip_nsh;
		dstip = dstip_nsh;
#ifdef OVBENCH
		ovtype = OVTYPE_NSH;
#endif
		if (!measure_pps)
			start_netdevgen_xmit_one_thread ();
		else
			start_netdevgen_thread ();

	} else if (strncmp (buf, "noencap", 7) == 0) {

		srcip = srcip_noencap;
		dstip = dstip_noencap;
#ifdef OVBENCH
		ovtype = OVTYPE_NOENCAP;
#endif
		if (!measure_pps)
			start_netdevgen_xmit_one_thread ();
		else
			start_netdevgen_thread ();
		
	} else if (strncmp (buf, "start", 5) == 0) {

		start_netdevgen_thread ();

	} else if (strncmp (buf, "stop", 4) == 0) {

		stop_netdevgen_thread ();

	} else {
		pr_info ("invalid command\n");
	}

        return size;
}

static const struct file_operations proc_file_fops = {
	.owner = THIS_MODULE,
	.read = proc_read,
	.write = proc_write,
};

static int __init
netdevgen_init (void)
{
	struct proc_dir_entry * ent;

        ent = proc_create(PROC_NAME, S_IRUGO | S_IWUGO | S_IXUGO,
			  NULL, &proc_file_fops);
        if (ent == NULL)
                return -ENOMEM;


	atomic_set (&start, 0);

	if (IS_ERR (ndg_tsk)) {
		pr_err ("failed to run netdevgen thread\n");
		return -1;
	}

	pr_info ("netdevgen loaded\n");
	if (measure_pps)
		pr_info ("measurement pps mode, pktlen is %d\n", pktlen);
		
	return 0;
}

static void __exit
netdevgen_exit (void)
{
	remove_proc_entry (PROC_NAME, NULL);

	if (ndg_tsk && ndg_thread_running)
		kthread_stop (ndg_tsk);
	else {
		pr_info ("thread is already done\n");
	}

	pr_info ("netdevgen unloaded\n");

	return;
}

module_init (netdevgen_init);
module_exit (netdevgen_exit);
