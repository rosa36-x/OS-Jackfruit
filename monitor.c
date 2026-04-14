#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pid.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/timer.h>
#include <linux/uaccess.h>
#include <linux/version.h>

#include "monitor_ioctl.h"

#define DEVICE_NAME "container_monitor"
#define CHECK_INTERVAL_SEC 1

struct monitored_entry {
    pid_t pid;
    char container_id[32];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int soft_warned;
    struct list_head list;
};

static LIST_HEAD(monitored_list);
static DEFINE_MUTEX(monitored_lock);

static struct timer_list monitor_timer;
static dev_t dev_num;
static struct cdev c_dev;
static struct class *cl;

/* ---------- RSS ---------- */
static long get_rss_bytes(pid_t pid) {
    struct task_struct *task;
    struct mm_struct *mm;
    long rss = 0;

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (!task) {
        rcu_read_unlock();
        return -1;
    }
    get_task_struct(task);
    rcu_read_unlock();

    mm = get_task_mm(task);
    if (mm) {
        rss = get_mm_rss(mm);
        mmput(mm);
    }

    put_task_struct(task);
    return rss * PAGE_SIZE;
}

/* ---------- Timer ---------- */
static void timer_callback(struct timer_list *t) {
    struct monitored_entry *e, *tmp;
    long rss;

    mutex_lock(&monitored_lock);

    list_for_each_entry_safe(e, tmp, &monitored_list, list) {
        rss = get_rss_bytes(e->pid);

        if (rss < 0) {
            list_del(&e->list);
            kfree(e);
            continue;
        }

        if ((unsigned long)rss > e->hard_limit_bytes) {
            struct task_struct *task;
            rcu_read_lock();
            task = pid_task(find_vpid(e->pid), PIDTYPE_PID);
            if (task)
                send_sig(SIGKILL, task, 1);
            rcu_read_unlock();

            printk(KERN_WARNING "[HARD LIMIT] %s pid=%d\n", e->container_id, e->pid);

            list_del(&e->list);
            kfree(e);
            continue;
        }

        if ((unsigned long)rss > e->soft_limit_bytes && !e->soft_warned) {
            printk(KERN_WARNING "[SOFT LIMIT] %s pid=%d\n", e->container_id, e->pid);
            e->soft_warned = 1;
        }
    }

    mutex_unlock(&monitored_lock);

    mod_timer(&monitor_timer, jiffies + CHECK_INTERVAL_SEC * HZ);
}

/* ---------- IOCTL ---------- */
static long monitor_ioctl(struct file *f, unsigned int cmd, unsigned long arg) {
    struct monitor_request req;

    if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
        return -EFAULT;

    if (cmd == MONITOR_REGISTER) {
        struct monitored_entry *e;

        if (req.soft_limit_bytes > req.hard_limit_bytes)
            return -EINVAL;

        e = kmalloc(sizeof(*e), GFP_KERNEL);
        if (!e) return -ENOMEM;

        e->pid = req.pid;
        e->soft_limit_bytes = req.soft_limit_bytes;
        e->hard_limit_bytes = req.hard_limit_bytes;
        e->soft_warned = 0;

        strncpy(e->container_id, req.container_id, 31);
        e->container_id[31] = '\0';

        INIT_LIST_HEAD(&e->list);

        mutex_lock(&monitored_lock);
        list_add_tail(&e->list, &monitored_list);
        mutex_unlock(&monitored_lock);

        return 0;
    }

    if (cmd == MONITOR_UNREGISTER) {
        struct monitored_entry *e, *tmp;

        mutex_lock(&monitored_lock);
        list_for_each_entry_safe(e, tmp, &monitored_list, list) {
            if (e->pid == req.pid) {
                list_del(&e->list);
                kfree(e);
                break;
            }
        }
        mutex_unlock(&monitored_lock);
        return 0;
    }

    return -EINVAL;
}

static struct file_operations fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = monitor_ioctl,
};

static int __init monitor_init(void) {
    alloc_chrdev_region(&dev_num, 0, 1, DEVICE_NAME);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6,4,0)
    cl = class_create(DEVICE_NAME);
#else
    cl = class_create(THIS_MODULE, DEVICE_NAME);
#endif

    device_create(cl, NULL, dev_num, NULL, DEVICE_NAME);

    cdev_init(&c_dev, &fops);
    cdev_add(&c_dev, dev_num, 1);

    timer_setup(&monitor_timer, timer_callback, 0);
    mod_timer(&monitor_timer, jiffies + HZ);

    printk(KERN_INFO "monitor loaded\n");
    return 0;
}

static void __exit monitor_exit(void) {
    struct monitored_entry *e, *tmp;

    del_timer_sync(&monitor_timer);

    mutex_lock(&monitored_lock);
    list_for_each_entry_safe(e, tmp, &monitored_list, list) {
        list_del(&e->list);
        kfree(e);
    }
    mutex_unlock(&monitored_lock);

    cdev_del(&c_dev);
    device_destroy(cl, dev_num);
    class_destroy(cl);
    unregister_chrdev_region(dev_num, 1);

    printk(KERN_INFO "monitor unloaded\n");
}

module_init(monitor_init);
module_exit(monitor_exit);
MODULE_LICENSE("GPL");
