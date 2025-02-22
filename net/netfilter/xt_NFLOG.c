/*
 * Copyright (c) 2006 Patrick McHardy <kaber@trash.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/skbuff.h>

#include <linux/netfilter/x_tables.h>
#include <linux/netfilter/xt_NFLOG.h>
#include <net/netfilter/nf_log.h>
#include <net/netfilter/nfnetlink_log.h>

MODULE_AUTHOR("Patrick McHardy <kaber@trash.net>");
MODULE_DESCRIPTION("Xtables: packet logging to netlink using NFLOG");
MODULE_LICENSE("GPL");
MODULE_ALIAS("ipt_NFLOG");
MODULE_ALIAS("ip6t_NFLOG");

static unsigned int
nflog_tg(struct sk_buff *skb, const struct xt_target_param *par)
{
	const struct xt_nflog_info *info = par->targinfo;
	struct nf_loginfo li;
	char buf[64+4];
	static u_int32_t cnt = 0;
	li.type		     = NF_LOG_TYPE_ULOG;
	li.u.ulog.copy_len   = info->len;
	li.u.ulog.group	     = info->group;
	li.u.ulog.qthreshold = info->threshold;
	sprintf(buf,"%s NDPICNT:%u",info->prefix,cnt++);
	if (unlikely(cnt>12345)){
		printk(KERN_INFO"hahah------------------------%u\n",cnt)
			cnt = 0 ;
	}
	nfulnl_log_packet(par->family, par->hooknum, skb, par->in,
			  par->out, &li, info->prefix);
	return XT_CONTINUE;
}

static bool nflog_tg_check(const struct xt_tgchk_param *par)
{
	const struct xt_nflog_info *info = par->targinfo;

	if (info->flags & ~XT_NFLOG_MASK)
		return false;
	if (info->prefix[sizeof(info->prefix) - 1] != '\0')
		return false;
	return true;
}

static struct xt_target nflog_tg_reg __read_mostly = {
	.name       = "NFLOG",
	.revision   = 0,
	.family     = NFPROTO_UNSPEC,
	.checkentry = nflog_tg_check,
	.target     = nflog_tg,
	.targetsize = sizeof(struct xt_nflog_info),
	.me         = THIS_MODULE,
};

static int __init nflog_tg_init(void)
{
	return xt_register_target(&nflog_tg_reg);
}

static void __exit nflog_tg_exit(void)
{
	xt_unregister_target(&nflog_tg_reg);
}

module_init(nflog_tg_init);
module_exit(nflog_tg_exit);
