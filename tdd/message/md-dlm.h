#ifndef  _MD_DLM_H
#define  _MD_DLM_H

#include <linux/dlm.h>
#include <linux/list.h>
#include <linux/wait.h>
#include <linux/slab.h>
#include <linux/sched.h>

#define LVB_SIZE 32
#define THREAD_WAKEUP  0

#define METADATA_UPDATED	(0)
#define RESYNC_FINISHED	(1)
#define SUSPEND_RANGE		(2)

struct msg_entry {
	int type;
	char buf[0];
};

struct dlm_md_msg {
	struct list_head list;
	wait_queue_head_t waiter;
	int async;
	char *buf;
	int len;
	int sent;
};

struct cluster_msg {
	int type;
	int bitmap;
	unsigned long long low;
	unsigned long long high;
};

struct md_thread {
	void			(*run) (struct md_thread *thread);
	wait_queue_head_t	wqueue;
	unsigned long           flags;
	struct task_struct	*tsk;
	unsigned long		timeout;
	void			*private;
};

struct dlm_lock_resource {
	struct list_head list;
	int state;
	int index; /* bitmap index for bitmaps. */
	char finished;
	wait_queue_head_t waiter;
	char *name; /* lock name. md uuid + name. */
	int namelen;
	int mode;
	uint32_t flags;
	struct dlm_lksb lksb;
	uint32_t parent_lkid;
	void (*bast)(void *arg, int mode); /*bast for sync lock*/
};

/*used by message sender*/
extern int md_send_resync_finished(int bmpno);
extern int md_send_metadata_update(int async);
extern int md_send_suspend(int bmpno, unsigned long long sus_start, 
		unsigned long long sus_end);

#endif   /*_MD_DLM_H*/
