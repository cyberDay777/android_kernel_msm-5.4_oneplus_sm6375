/***********************************************************
** Copyright (C), 2008-2019, OPLUS Mobile Comm Corp., Ltd.
** File: hans.c
** Description: Add for hans freeze manager
**
** Version: 1.0
** Date : 2019/09/23
**
** ------------------ Revision History:------------------------
** <author>      <data>      <version >       <desc>
** Kun Zhou    2019/09/23      1.0       OPLUS_ARCH_EXTENDS
** Kun Zhou    2020/05/27      1.1       OPLUS_FEATURE_HANS_FREEZE
****************************************************************/

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/netlink.h>
#include <linux/skbuff.h>
#include <net/sock.h>
#include <linux/hans.h>

#define NETLINK_PORT_HANS        (0x15356)

static struct sock *sock_handle = NULL;
static atomic_t hans_deamon_port;

/*
 *reuse LOOPBACK and FROZEN_TRANS channel to notify framework whether kernel support cgroupv2 or not
 */
static void hans_kern_support_cgrpv2(void) {
	/*notify framework that kernel support cgroupv2*/
	hans_report(PKG, -1, -1, -1, -1, "PKG", HANS_USE_CGRPV2);
	printk(KERN_ERR "%s: hans support cgroupv2\n", __func__);
}

/*
 * netlink report function to tell HANS native deamon unfreeze process info
 * if the parameters is empty, fill it with (pid/uid with -1)
 */
int hans_report(enum message_type type, int caller_pid, int caller_uid, int target_pid, int target_uid, const char *rpc_name, int code)
{
	int len = 0;
	int ret = 0;
	struct hans_message *data = NULL;
	struct sk_buff *skb = NULL;
	struct nlmsghdr *nlh = NULL;

	if (atomic_read(&hans_deamon_port) == -1) {
		pr_err("%s: hans_deamon_port invalid!\n", __func__);
                return HANS_ERROR;
	}

	if (sock_handle == NULL) {
		pr_err("%s: sock_handle invalid!\n", __func__);
                return HANS_ERROR;
	}

	if (type >= TYPE_MAX) {
		pr_err("%s: type = %d invalid!\n", __func__, type);
		return HANS_ERROR;
	}

	len = sizeof(struct hans_message);
	skb = nlmsg_new(len, GFP_ATOMIC);
	if (skb == NULL) {
		pr_err("%s: type =%d, nlmsg_new failed!\n", __func__, type);
		return HANS_ERROR;
	}

	nlh = nlmsg_put(skb, 0, 0, 0, len, 0);
	if (nlh == NULL) {
		pr_err("%s: type =%d, nlmsg_put failed!\n", __func__, type);
		kfree_skb(skb);
		return HANS_ERROR;
	}

	data = nlmsg_data(nlh);
	if(data == NULL) {
		pr_err("%s: type =%d, nlmsg_data failed!\n", __func__, type);
		return HANS_ERROR;
	}
	data->type = type;
	data->port = NETLINK_PORT_HANS;
	data->caller_pid = caller_pid;
	data->caller_uid = caller_uid;
	data->target_pid = target_pid;
	data->target_uid = target_uid;
	data->pkg_cmd = -1; /* invalid package cmd */
	data->code = code;
	strlcpy(data->rpc_name, rpc_name, INTERFACETOKEN_BUFF_SIZE);
	nlmsg_end(skb, nlh);

	if ((ret = nlmsg_unicast(sock_handle, skb, (u32)atomic_read(&hans_deamon_port))) < 0) {
		pr_err("%s: nlmsg_unicast failed! err = %d\n", __func__ , ret);
		return HANS_ERROR;
	}

	return HANS_NOERROR;
}

/* HANS kernel module handle the message from HANS native deamon */
static void hans_handler(struct sk_buff *skb)
{
	struct hans_message *data = NULL;
	struct nlmsghdr *nlh = NULL;
	unsigned int len  = 0;
	int uid = -1;

	if (!skb) {
		pr_err("%s: recv skb NULL!\n", __func__);
		return;
	}

	/* safety check */
	uid = (*NETLINK_CREDS(skb)).uid.val;
	/* only allow native deamon talk with HANS kernel. */
	if (uid != 1000) {
		pr_err("%s: uid: %d, permission denied\n", __func__, uid);
		return;
	}

	if (skb->len >= NLMSG_SPACE(0)) {
		nlh = nlmsg_hdr(skb);
		len = NLMSG_PAYLOAD(nlh, 0);
		data = (struct hans_message *)NLMSG_DATA(nlh);

		if (len < (sizeof(struct hans_message) - sizeof(int))) {
			pr_err("%s: hans_message len check faied! len = %d  min_expected_len = %lu!\n", __func__, len, sizeof(struct hans_message) - sizeof(int));
			return;
		}

		if (data->port < 0) {
			pr_err("%s: portid = %d invalid!\n", __func__, data->port);
			return;
		}
		if (data->type >= TYPE_MAX) {
			pr_err("%s: type = %d invalid!\n", __func__, data->type);
			return;
		}
		if (atomic_read(&hans_deamon_port) == -1 && data->type != LOOP_BACK) {
			pr_err("%s: handshake not setup, type = %d!\n", __func__, data->type);
                        return;
		}

		switch (data->type) {
		case LOOP_BACK:  /*Loop back message, only for native deamon and kernel handshake*/
			atomic_set(&hans_deamon_port, data->port);
			hans_report(LOOP_BACK, -1, -1, -1, -1, "loop back", CPUCTL_VERSION);
			printk(KERN_ERR "%s: --> LOOP_BACK, port = %d\n", __func__, data->port);
			hans_kern_support_cgrpv2();
			break;
		case PKG:
			if (len < sizeof(struct hans_message)) {
				/* native daemon in ofreezer 1.0 has no 'int persistent' in the message structure */
				printk(KERN_ERR "%s: --> PKG, ofreezer 1.0 native, uid = %d, pkg_cmd = %d\n",
						__func__, data->target_uid, data->pkg_cmd);
				hans_network_cmd_parse(data->target_uid, 0 /* persistent */, data->pkg_cmd);
				break;
			}
			printk(KERN_ERR "%s: --> PKG, uid = %d, persistent = %d, pkg_cmd = %d\n",
					__func__, data->target_uid, data->persistent, data->pkg_cmd);
			hans_network_cmd_parse(data->target_uid, data->persistent, data->pkg_cmd);
			break;
		case FROZEN_TRANS:
		case CPUCTL_TRANS:
			if (CHECK_KERN_SUPPORT_CGRPV2 == data->target_uid) {
				hans_kern_support_cgrpv2();
			} else {
				printk(KERN_ERR "%s: --> FROZEN_TRANS, uid = %d\n", __func__, data->target_uid);
				hans_check_frozen_transcation(data->target_uid, data->type);
			}
			break;

		default:
			pr_err("%s: hans_messag type invalid %d\n", __func__, data->type);
			break;
		}
	}
}


/*free_async_space = (alloc->free_async_space);
 *binder_buffer_size = (sizeof(struct binder_buffer));
 *alloc_buffer_size = alloc->buffer_size*/
void hans_check_async_binder_buffer(bool is_async, int free_async_space, int size, int binder_buffer_size, int alloc_buffer_size, int pid)
{
	struct task_struct *p = NULL;

	if (is_async
		&& (free_async_space < 3 * (size + binder_buffer_size)
		|| (free_async_space < ((alloc_buffer_size / 2) * 9 / 10)))) {
		rcu_read_lock();
		p = find_task_by_vpid(pid);
		rcu_read_unlock();

		if (p != NULL && is_frozen_tg(p)) {
			hans_report(ASYNC_BINDER, task_tgid_nr(current), task_uid(current).val, task_tgid_nr(p), task_uid(p).val, "free_buffer_full", -1);
		}
	}
}

void hans_check_signal(struct task_struct *p, int sig)
{
	if (is_frozen_tg(p)  /*signal receiver thread group is frozen?*/
		&& (sig == SIGKILL || sig == SIGTERM || sig == SIGABRT || sig == SIGQUIT)) {
		if (hans_report(SIGNAL, task_tgid_nr(current), task_uid(current).val, task_tgid_nr(p), task_uid(p).val, "signal", -1) == HANS_ERROR) {
			printk(KERN_ERR "HANS: report signal-freeze failed, sig = %d, caller = %d, target_uid = %d\n", sig, task_tgid_nr(current), task_uid(p).val);
		}
	}

#if defined(CONFIG_CFS_BANDWIDTH)
	if (is_belong_cpugrp(p)  /*signal receiver thread group is cpuctl?*/
		&& (sig == SIGKILL || sig == SIGTERM || sig == SIGABRT || sig == SIGQUIT)) {
		if (hans_report(SIGNAL_CPUCTL, task_tgid_nr(current), task_uid(current).val, task_tgid_nr(p), task_uid(p).val, "signal", -1) == HANS_ERROR) {
			printk(KERN_ERR "HANS: report signal-cpuctl failed, sig = %d, caller = %d, target_uid = %d\n", sig, task_tgid_nr(current), task_uid(p).val);
		}
	}
#endif
}

static int __init hans_core_init(void)
{
	struct netlink_kernel_cfg cfg = {
		.input = hans_handler,
	};

	atomic_set(&hans_deamon_port, -1);

        sock_handle = netlink_kernel_create(&init_net, NETLINK_OPLUS_HANS, &cfg);
	if (sock_handle == NULL) {
		pr_err("%s: create netlink socket failed!\n", __func__);
		return HANS_ERROR;
	}

	if (hans_netfilter_init() == HANS_ERROR) {
		pr_err("%s: netfilter init failed!\n", __func__);
		netlink_kernel_release(sock_handle);  /* release socket */
                return HANS_ERROR;
	}

	printk(KERN_INFO "%s: -\n", __func__);
	return HANS_NOERROR;
}

static void __exit hans_core_exit(void)
{
	if (sock_handle)
		netlink_kernel_release(sock_handle);

	hans_netfilter_deinit();
	printk(KERN_INFO "%s: -\n", __func__);
}

module_init(hans_core_init);
module_exit(hans_core_exit);

MODULE_LICENSE("GPL");
