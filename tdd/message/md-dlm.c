#include "md-dlm.h"
#include "linux/kthread.h"
#include "linux/module.h"


static LIST_HEAD(send_list);
static DEFINE_SPINLOCK(send_lock);

struct md_thread *send_thread;
struct md_thread *recv_thread;

dlm_lockspace_t *dlm_md_lockspace;
struct dlm_lock_resource *dlm_md_message; 
struct dlm_lock_resource *dlm_md_token; 
struct dlm_lock_resource *dlm_md_ack; 

/*
 * lockspace and lock resource
 * */
void deinit_lock_resource(struct dlm_lock_resource *res)
{
	if (!res) {
		return;
	}
	if (res->name) {
		kfree(res->name);
	}
	if (res->lksb.sb_lvbptr) {
		kfree(res->lksb.sb_lvbptr);
	}
	kfree(res);
	return;
}

struct dlm_lock_resource *init_lock_resource(const char *name)
{
	struct dlm_lock_resource *ret = NULL;
	ret = kzalloc(sizeof(struct dlm_lock_resource), GFP_KERNEL);
	if (!ret) {
		return ret;
	}
	INIT_LIST_HEAD(&ret->list);
	init_waitqueue_head(&ret->waiter);
	ret->name = kzalloc(strlen(name), GFP_KERNEL);
	if (!ret->name) {
		kfree(ret);
		return NULL;
	}
	memcpy(ret->name, name, strlen(name));
	ret->namelen = strlen(name);
	return ret;
}

void sync_ast(void *arg)
{
	struct dlm_lock_resource *res;
	res = (struct dlm_lock_resource *) arg;
	res->finished = 1;
	wake_up(&res->waiter);
}

int dlm_lock_sync(dlm_lockspace_t *ls, struct dlm_lock_resource *res)
{
	int ret = 0;
	res->finished = 0;
	ret = dlm_lock(ls, res->mode, &res->lksb,
			res->flags, res->name, res->namelen,
			res->parent_lkid, sync_ast, res, res->bast);
	if (ret) {
		return ret;
	}
	wait_event(res->waiter, res->finished == 1);
	return res->lksb.sb_status;
}

int dlm_unlock_sync(dlm_lockspace_t *ls, struct dlm_lock_resource *res)
{
	int ret = 0;
	res->finished = 0;
	ret = dlm_unlock(ls, res->lksb.sb_lkid, 0, &res->lksb, res);
	if (ret) {
		return ret;
	}
	wait_event(res->waiter, res->finished == 1);
	printk(KERN_ERR "md/raid1:unlock sync\n");
	return res->lksb.sb_status;
}
/*
 *thread stuff
 * */
static void md_wakeup_thread(struct md_thread *thread)
{
	if (thread) {
		set_bit(THREAD_WAKEUP, &thread->flags);
		wake_up(&thread->wqueue);
	}
}

static void wait_for_receive_message(void *arg, int mode)
{
	md_wakeup_thread(recv_thread);
}
/*
 * thread for receiving message
 * */
static void raid1_recvd(struct md_thread *thread)
{
	struct dlm_lock_resource *ack = dlm_md_ack;
	struct dlm_lock_resource *message = dlm_md_message;
	struct cluster_msg *msg;
	struct msg_entry *entry;

	printk(KERN_ERR "md/raid1:raid1_recvd step1\n");
	/*get CR on Message*/
	message->state = 0;
	message->mode = DLM_LOCK_CR;
	message->flags = DLM_LKF_VALBLK;
	message->parent_lkid = 0;
	if (dlm_lock_sync(dlm_md_lockspace, message)) {
		printk(KERN_ERR "md/raid1:failed to get CR on MESSAGE\n");
		return;
	}
	printk(KERN_ERR "md/raid1:raid1_recvd step2\n");

	//read lvb and wake up thread to process this message
	entry = kzalloc(sizeof(struct msg_entry) + sizeof(struct cluster_msg), GFP_KERNEL); 
	if (!entry) {
		printk(KERN_ERR "md/raid1:failed to alloc mem\n");
		return;
	}
	memcpy(entry->buf, message->lksb.sb_lvbptr, sizeof(struct cluster_msg));
	msg = (struct cluster_msg *) entry->buf;
	entry->type = msg->type;
	switch(entry->type) {
		case 0:
			printk(KERN_ALERT "raid1_recvd:receive message type: metadata update\n");
			break;
		case 1:
			printk(KERN_ALERT "raid1_recvd:receive message type: suspend range\n");
			break;
		case 2:
			printk(KERN_ALERT "raid1_recvd:receive message type: resync finish\n");
			break;
		default:
			printk(KERN_ALERT "raid1_recvd:should not show this message!!!\n");
	}

	/*release CR on ack*/
	dlm_unlock_sync(dlm_md_lockspace, ack);
	printk(KERN_ERR "md/raid1:raid1_recvd step3\n");
	/*release CR on message*/
	dlm_unlock_sync(dlm_md_lockspace, message);
	printk(KERN_ERR "md/raid1:raid1_recvd step4\n");
	/*get CR on ack again*/
	ack->state = 0;
	ack->mode = DLM_LOCK_CR;
	ack->flags = 0;
	ack->bast = wait_for_receive_message;
	ack->parent_lkid = 0;
	dlm_lock_sync(dlm_md_lockspace, ack);
	printk(KERN_ERR "md/raid1:raid1_recvd step5\n");
}


/*
 * thread for sending message
 * QUSTION: whether process message in the send_list in a loop?
 * */
static void raid1_sendd(struct md_thread *thread)
{
	struct dlm_lock_resource *ack = dlm_md_ack;
	struct dlm_lock_resource *message = dlm_md_message;
	struct dlm_lock_resource *token = dlm_md_token;
	struct dlm_md_msg *msg;
	int error = 0;

	if (list_empty(&send_list)) {
		printk(KERN_ERR "md/raid1:mddev->send_list is empty \n");
		return;
	}
	printk(KERN_ERR "md/raid1:raid1_sendd step1\n");

	spin_lock(&send_lock);
	while (!list_empty(&send_list)) {
		msg = list_entry(send_list.next,
				struct dlm_md_msg,
				list);
		list_del(&msg->list);
		spin_unlock(&send_lock);

	    printk(KERN_ERR "md/raid1:raid1_sendd step2\n");
		/*Get EX on Token*/
		token->state = 0;
		token->mode = DLM_LOCK_EX;
		token->flags = 0;
		token->parent_lkid = 0;
		if (dlm_lock_sync(dlm_md_lockspace, token)) {
			printk(KERN_ERR "md/raid1:failed to get EX on TOKEN\n");
			return;
		}

	    printk(KERN_ERR "md/raid1:raid1_sendd step3\n");

		/*get EX on Message*/
		message->state = 0;
		message->mode = DLM_LOCK_EX;
		message->flags = 0;
		message->parent_lkid = 0;
		message->bast = NULL;
		if (dlm_lock_sync(dlm_md_lockspace, message)) {
			printk(KERN_ERR "md/raid1:failed to get EX on MESSAGE\n");
			error = 1;
			goto failed_message;
		}
	    printk(KERN_ERR "md/raid1:raid1_sendd step4\n");

		/*down-convert EX to CR on Message*/
		message->mode = DLM_LOCK_CR;
		message->flags = DLM_LKF_CONVERT|DLM_LKF_VALBLK;
		memcpy(message->lksb.sb_lvbptr, msg->buf, sizeof(struct cluster_msg));
		if (dlm_lock_sync(dlm_md_lockspace, message)) {
			printk(KERN_ERR "md/raid1:failed to convert EX to CR on MESSAGE\n");
			error = 1;
			goto failed_message;
		}

	    printk(KERN_ERR "md/raid1:raid1_sendd step5\n");
		/*up-convert CR to EX on Ack*/
		ack->state = 0;
		ack->mode = DLM_LOCK_EX;
		ack->flags = DLM_LKF_CONVERT;
		ack->parent_lkid = 0;
		if (dlm_lock_sync(dlm_md_lockspace, ack)) {
			printk(KERN_ERR "md/raid1:failed to convert CR to EX on ACK\n");
			error = 1;
			goto failed_ack;
		}

	    printk(KERN_ERR "md/raid1:raid1_sendd step6\n");
		/*down-convert EX to CR on Ack*/
		ack->mode = DLM_LOCK_CR;
		ack->flags = DLM_LKF_CONVERT;
		ack->bast = wait_for_receive_message;
		if (dlm_lock_sync(dlm_md_lockspace, ack)) {
			printk(KERN_ERR "md/raid1:failed to convert EX to CR on ACK\n");
			error = 1;
			goto failed_ack;
		}

	    printk(KERN_ERR "md/raid1:raid1_sendd step7\n");
 failed_ack:
		dlm_unlock_sync(dlm_md_lockspace, message);
 failed_message:
		dlm_unlock_sync(dlm_md_lockspace, token);
		msg->sent = 1;
		wake_up(&msg->waiter);
	    printk(KERN_ERR "md/raid1:raid1_sendd step8\n");
		spin_lock(&send_lock);
		if (error) break;
	}
	spin_unlock(&send_lock);
}

static int md_thread(void * arg)
{
	struct md_thread *thread = arg;

	/*
	 * md_thread is a 'system-thread', it's priority should be very
	 * high. We avoid resource deadlocks individually in each
	 * raid personality. (RAID5 does preallocation) We also use RR and
	 * the very same RT priority as kswapd, thus we will never get
	 * into a priority inversion deadlock.
	 *
	 * we definitely have to have equal or higher priority than
	 * bdflush, otherwise bdflush will deadlock if there are too
	 * many dirty RAID5 blocks.
	 */

	allow_signal(SIGKILL);
	while (!kthread_should_stop()) {

		/* We need to wait INTERRUPTIBLE so that
		 * we don't add to the load-average.
		 * That means we need to be sure no signals are
		 * pending
		 */
		if (signal_pending(current))
			flush_signals(current);

		wait_event_interruptible_timeout
			(thread->wqueue,
			 test_bit(THREAD_WAKEUP, &thread->flags)
			 || kthread_should_stop(),
			 thread->timeout);

		clear_bit(THREAD_WAKEUP, &thread->flags);
		if (!kthread_should_stop())
			thread->run(thread);
	}

	return 0;
}

struct md_thread *md_register_thread(void (*run) (struct md_thread *),
		 const char *name)
{
	struct md_thread *thread;

	thread = kzalloc(sizeof(struct md_thread), GFP_KERNEL);
	if (!thread)
		return NULL;

	init_waitqueue_head(&thread->wqueue);

	thread->run = run;
	thread->timeout = MAX_SCHEDULE_TIMEOUT;
	thread->tsk = kthread_run(md_thread, thread,
				  "%s_%s",
				  "CRAID1",
				  name);
	if (IS_ERR(thread->tsk)) {
		kfree(thread);
		return NULL;
	}
	return thread;
}

void md_unregister_thread(struct md_thread **threadp)
{
	struct md_thread *thread = *threadp;
	if (!thread)
		return;
	/* Locking ensures that mddev_unlock does not wake_up a
	 * non-existent thread
	 */
	*threadp = NULL;

	kthread_stop(thread->tsk);
	kfree(thread);
}

int md_send_metadata_update(int async)
{
	struct dlm_md_msg *msg;
	struct cluster_msg *update;
	/* super block updated. 
	 * Should prepare message to the send thread
	 * later when sen thread is wake up, message 
	 * will be sent out
	 */
	msg = kzalloc(sizeof(struct dlm_md_msg), GFP_KERNEL);
	if (!msg) {
		printk(KERN_WARNING "alloc memory for msg failed!\n");
		return -ENOMEM;
	}
	INIT_LIST_HEAD(&msg->list);
	init_waitqueue_head(&msg->waiter);
	msg->sent = 0;
	msg->buf = kzalloc(sizeof(struct cluster_msg), GFP_KERNEL);
	if (!msg->buf) {
		printk(KERN_WARNING "alloc memory for msg failed!\n");
		kfree(msg);
		return -ENOMEM;
	}
	msg->async = async;
	update = (struct cluster_msg *)msg->buf;
	update->type = cpu_to_le32(METADATA_UPDATED);
	spin_lock(&send_lock);
	list_add_tail(&msg->list, &send_list);
	spin_unlock(&send_lock);
	md_wakeup_thread(send_thread);
	if (!async) {
		wait_event(msg->waiter, msg->sent != 0);
		kfree(msg->buf);
		kfree(msg);
	}
	return 0;
}

int md_send_suspend(int bmpno, unsigned long long sus_start, unsigned long long sus_end)
{
	struct dlm_md_msg *msg;
	struct cluster_msg *suspend;
	msg = kzalloc(sizeof(struct dlm_md_msg), GFP_KERNEL);
	if (!msg) {
		return -ENOMEM;
	}
	msg->buf = kzalloc(sizeof(struct cluster_msg), GFP_KERNEL);
	if (!msg->buf) {
		kfree(msg);
		return -ENOMEM;
	}

	suspend = (struct cluster_msg*)msg->buf;
	suspend->type = cpu_to_le32(SUSPEND_RANGE);
	suspend->bitmap = cpu_to_le32(bmpno);
	suspend->low = cpu_to_le64(sus_start);
	suspend->high = cpu_to_le64(sus_end);
	msg->len = sizeof(struct cluster_msg);
	INIT_LIST_HEAD(&msg->list);
	init_waitqueue_head(&msg->waiter);
	msg->sent = 0;
	spin_lock(&send_lock);
	list_add_tail(&msg->list, &send_list);
	spin_unlock(&send_lock);
	md_wakeup_thread(send_thread);
	wait_event(msg->waiter, msg->sent != 0);
	kfree(msg->buf);
	kfree(msg);
	return 0;
}

int md_send_resync_finished(int bmpno)
{
	struct dlm_md_msg *msg;
	struct cluster_msg *resync;
	msg = kzalloc(sizeof(struct dlm_md_msg), GFP_KERNEL);
	if (!msg) {
		printk(KERN_WARNING "allocate memory for message failed!\n");
		return -ENOMEM;
	}
	INIT_LIST_HEAD(&msg->list);
	init_waitqueue_head(&msg->waiter);
	msg->sent = 0;
	resync = kzalloc(sizeof(struct cluster_msg), GFP_KERNEL);
	if (!resync) {
		kfree(msg);
		printk(KERN_WARNING "allocate memory for message failed!\n");
		return -ENOMEM;
	}
	msg->buf = (char *)resync;
	resync->type = cpu_to_le32(RESYNC_FINISHED);
	resync->bitmap = cpu_to_le32(bmpno);
	spin_lock(&send_lock);
	list_add_tail(&msg->list, &send_list);
	spin_unlock(&send_lock);
	md_wakeup_thread(send_thread);
	wait_event(msg->waiter, msg->sent != 0);
	kfree(msg->buf);
	kfree(msg);
	return 0;
}

static int __init md_init(void)
{
	int ret = 0;
	struct dlm_lock_resource *res;
	//create lockspace and lock resource
	ret = dlm_new_lockspace("md-cluster", NULL, DLM_LSFL_FS, 32, NULL, NULL, NULL,		&dlm_md_lockspace);
	if (ret)
		goto failed;
	dlm_md_message = init_lock_resource("message");
	if (!dlm_md_message)
		goto failed;
	dlm_md_message->lksb.sb_lvbptr = kzalloc(32, GFP_KERNEL);
	if (!dlm_md_message->lksb.sb_lvbptr)
		goto failed;
	dlm_md_token = init_lock_resource("token");
	if (!dlm_md_token)
		goto failed;
	dlm_md_ack = init_lock_resource("ack");
	if (!dlm_md_ack)
		goto failed;
	/* get sync CR lock on ACK. */
	res = dlm_md_ack;
	res->finished = 0;
	res->mode = DLM_LOCK_CR;
	res->flags = DLM_LKF_NOQUEUE;
	res->parent_lkid = 0;
	res->state = 0;
	res->bast = wait_for_receive_message;
	if (dlm_lock_sync(dlm_md_lockspace, res)) {
		printk(KERN_ERR "failed to get a sync CR lock on ACK!\n");
	}
	
	//create two threads for receiving and sending
	recv_thread = md_register_thread(raid1_recvd,  "raid1_recvd");
	if (!recv_thread) {
		printk(KERN_ERR "cannot allocate memory for recv_thread!\n");
		goto failed;
	}
	send_thread = md_register_thread(raid1_sendd,  "raid1_sendd");
	if (!send_thread) {
		printk(KERN_ERR "cannot allocate memory for send_thread!\n");
		goto failed;
	}
failed:
	return ret;
	;
}

static void __exit md_exit(void)
{
	//release CR lock on ack
	dlm_unlock_sync(dlm_md_lockspace, dlm_md_ack);

	//cancel these two threads
	md_unregister_thread(&recv_thread);
	md_unregister_thread(&send_thread);
	//release lockspace and lock resource
	deinit_lock_resource(dlm_md_message);
	deinit_lock_resource(dlm_md_token);
	deinit_lock_resource(dlm_md_ack);
	dlm_md_message = NULL;
	dlm_md_token = NULL;
	dlm_md_ack = NULL;
	dlm_release_lockspace(dlm_md_lockspace, 3);
}

module_init(md_init);
module_exit(md_exit);
EXPORT_SYMBOL(md_send_metadata_update);
EXPORT_SYMBOL(md_send_resync_finished);
EXPORT_SYMBOL(md_send_suspend);
MODULE_LICENSE("GPL");
