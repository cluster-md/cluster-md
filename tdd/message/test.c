#include <linux/module.h>
#include <linux/init.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include "md-dlm.h"


void __handle_message(int key, bool check_mask)
{
	int rval = key - '0';
	if (rval > 2)
	{
		printk(KERN_ERR "value should be 0 1 2\n");
		return;
	}
	switch (rval) {
		case 0:
			printk(KERN_ERR "get 0\n");
			md_send_metadata_update(0);
			printk(KERN_ERR "send metadata update\n");
			break;
		case 1:
			printk(KERN_ERR "get 1\n");
			md_send_suspend(1,2,3);
			printk(KERN_ERR "send suspend range\n");
			break;
		case 2:
			printk(KERN_ERR "get 2\n");
			md_send_resync_finished(1);
			printk(KERN_ERR "send resync finish\n");
			break;
		default:
			printk(KERN_ERR "get %d,should be 0 1 2\n",rval);
	}
}

static ssize_t write_message_trigger(struct file *file, const char __user *buf,
				   size_t count, loff_t *ppos)
{
	if (count) {
		char c;

		if (get_user(c, buf))
			return -EFAULT;
		__handle_message(c, false);
	}

	return count;
}

static const struct file_operations proc_message_trigger_operations = {
	.write		= write_message_trigger,
	.llseek		= noop_llseek,
};

static void message_init_procfs(void)
{
	if (!proc_create("message-trigger", S_IWUSR, NULL,
			 &proc_message_trigger_operations))
		printk(KERN_ERR "Failed to register proc interface\n");
}

static int __init begin_test(void)
{
	printk(KERN_ERR "installing message test module\n");
	message_init_procfs();
	return 0;
}

static void __exit end_test(void)
{
	remove_proc_entry("message-trigger", NULL);
	printk(KERN_ERR "geting out of message test module\n");
}

module_init(begin_test);
module_exit(end_test);
MODULE_LICENSE("GPL");
