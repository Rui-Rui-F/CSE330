// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/delay.h>
#include <linux/semaphore.h>
#include <linux/spinlock.h>
#include <linux/slab.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Your Name");
MODULE_DESCRIPTION("CSE330 Project 3: Bounded buffer Producer Consumer (Zombie Finder)");
MODULE_VERSION("1.0");

static int prod = 0;
static int cons = 0;
static int size = 1;
static int uid  = 0;

module_param(prod, int, 0444);
module_param(cons, int, 0444);
module_param(size, int, 0444);
module_param(uid, int, 0444);

static struct task_struct **ring = NULL;
static int r_head = 0, r_tail = 0, r_cnt = 0;

static DEFINE_SPINLOCK(r_lock);

static struct semaphore sem_empty;
static struct semaphore sem_full;

static struct task_struct **prod_ts = NULL;
static struct task_struct **cons_ts = NULL;

static inline void ring_push(struct task_struct *p)
{
    ring[r_tail] = p;
    r_tail = (r_tail + 1) % size;
    r_cnt++;
}

static inline struct task_struct *ring_pop(void)
{
    struct task_struct *p = ring[r_head];
    r_head = (r_head + 1) % size;
    r_cnt--;
    return p;
}

struct prod_arg { int id; };

static int producer_fn(void *data)
{
    struct prod_arg *arg = (struct prod_arg *)data;
    const int id = arg ? arg->id : 1;

    while (!kthread_should_stop()) {
        struct task_struct *t;

        rcu_read_lock();
        for_each_process(t) {
            if (kthread_should_stop())
                break;

            if (from_kuid_munged(current_user_ns(), t->cred->uid) != (uid_t)uid)
                continue;

            if (t->exit_state & EXIT_ZOMBIE) {
                int ret = down_interruptible(&sem_empty);
                if (ret) {
                    if (kthread_should_stop()) {
                        rcu_read_unlock();
                        goto out;
                    }
                    continue;
                }

                get_task_struct(t);

                spin_lock(&r_lock);
                ring_push(t);
                spin_unlock(&r_lock);

                printk(KERN_INFO "[Producer-%d] has produced a zombie process with pid %d and parent pid %d\n",
                       id, t->pid, t->parent ? t->parent->pid : 0);

                up(&sem_full);
                cond_resched();
            }
        }
        rcu_read_unlock();

        if (!kthread_should_stop()) {
            set_current_state(TASK_INTERRUPTIBLE);
            schedule_timeout(HZ / 10);
        }
    }
out:
    return 0;
}

struct cons_arg { int id; };

static int consumer_fn(void *data)
{
    struct cons_arg *arg = (struct cons_arg *)data;
    const int id = arg ? arg->id : 1;

    while (!kthread_should_stop()) {
        struct task_struct *z = NULL;
        int ret = down_interruptible(&sem_full);
        if (ret) {
            if (kthread_should_stop())
                break;
            continue;
        }

        spin_lock(&r_lock);
        if (r_cnt > 0)
            z = ring_pop();
        spin_unlock(&r_lock);

        up(&sem_empty);

        if (z) {
            printk(KERN_INFO "[Consumer-%d] has consumed a zombie process with pid %d and parent pid %d\n",
                   id, z->pid, z->parent ? z->parent->pid : 0);
            put_task_struct(z);
        }

        cond_resched();
    }
    return 0;
}

static int __init producer_consumer_init(void)
{
    int i;

    if (size <= 0) {
        pr_err("size must be > 0\n");
        return -EINVAL;
    }
    if (prod < 0 || prod > 1) {
        pr_err("prod must be 0 or 1\n");
        return -EINVAL;
    }
    if (cons < 0) {
        pr_err("cons must be >= 0\n");
        return -EINVAL;
    }

    ring = kcalloc(size, sizeof(*ring), GFP_KERNEL);
    if (!ring)
        return -ENOMEM;

    prod_ts = (prod > 0) ? kcalloc(prod, sizeof(*prod_ts), GFP_KERNEL) : NULL;
    if (prod > 0 && !prod_ts)
        goto err_ring;

    cons_ts = (cons > 0) ? kcalloc(cons, sizeof(*cons_ts), GFP_KERNEL) : NULL;
    if (cons > 0 && !cons_ts)
        goto err_prod;

    sema_init(&sem_empty, size);
    sema_init(&sem_full, 0);

    if (prod > 0) {
        for (i = 0; i < prod; i++) {
            struct prod_arg *arg = kmalloc(sizeof(*arg), GFP_KERNEL);
            if (!arg) goto err_threads;
            arg->id = i + 1;
            prod_ts[i] = kthread_run(producer_fn, arg, "Producer-%d", arg->id);
            if (IS_ERR(prod_ts[i])) {
                pr_err("Failed to start producer %d\n", i + 1);
                kfree(arg);
                prod_ts[i] = NULL;
                goto err_threads;
            }
        }
    }

    for (i = 0; i < cons; i++) {
        struct cons_arg *arg = kmalloc(sizeof(*arg), GFP_KERNEL);
        if (!arg) goto err_threads;
        arg->id = i + 1;
        cons_ts[i] = kthread_run(consumer_fn, arg, "Consumer-%d", arg->id);
        if (IS_ERR(cons_ts[i])) {
            pr_err("Failed to start consumer %d\n", i + 1);
            kfree(arg);
            cons_ts[i] = NULL;
            goto err_threads;
        }
    }

    pr_info("producer_consumer loaded (prod=%d, cons=%d, size=%d, uid=%d)\n",
            prod, cons, size, uid);
    return 0;

err_threads:
    if (prod_ts) {
        for (i = 0; i < prod; i++) {
            if (prod_ts[i]) {
                kthread_stop(prod_ts[i]);
                prod_ts[i] = NULL;
            }
        }
    }
    if (cons_ts) {
        for (i = 0; i < cons; i++) {
            if (cons_ts[i]) {
                kthread_stop(cons_ts[i]);
                cons_ts[i] = NULL;
            }
        }
    }
    kfree(cons_ts);
err_prod:
    kfree(prod_ts);
err_ring:
    kfree(ring);
    return -ENOMEM;
}

static void __exit producer_consumer_exit(void)
{
    int i;

    if (prod_ts) {
        for (i = 0; i < prod; i++) {
            if (prod_ts[i]) {
                kthread_stop(prod_ts[i]);
                prod_ts[i] = NULL;
            }
        }
    }

    if (cons_ts) {
        for (i = 0; i < cons; i++) {
            if (cons_ts[i]) {
                kthread_stop(cons_ts[i]);
                cons_ts[i] = NULL;
            }
        }
    }

    spin_lock(&r_lock);
    while (r_cnt > 0) {
        struct task_struct *z = ring_pop();
        spin_unlock(&r_lock);
        if (z) put_task_struct(z);
        spin_lock(&r_lock);
    }
    spin_unlock(&r_lock);

    kfree(cons_ts);
    kfree(prod_ts);
    kfree(ring);

    pr_info("producer_consumer unloaded\n");
}

module_init(producer_consumer_init);
module_exit(producer_consumer_exit);
