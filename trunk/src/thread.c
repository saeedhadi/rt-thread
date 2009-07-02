/*
 * File      : thread.c
 * This file is part of RT-Thread RTOS
 * COPYRIGHT (C) 2006, RT-Thread Development Team
 *
 * The license and distribution terms for this file may be
 * found in the file LICENSE in this distribution or at
 * http://openlab.rt-thread.com/license/LICENSE
 *
 * Change Logs:
 * Date           Author       Notes
 * 2006-03-28     Bernard      first version
 * 2006-04-29     Bernard      implement thread timer
 * 2006-04-30     Bernard      add THREAD_DEBUG
 * 2006-05-27     Bernard      fix the rt_thread_yield bug
 * 2006-06-03     Bernard      fix the thread timer init bug
 * 2006-08-10     Bernard      fix the timer bug in thread_sleep
 * 2006-09-03     Bernard      change rt_timer_delete to rt_timer_detach
 * 2006-09-03     Bernard      implement rt_thread_detach
 * 2008-02-16     Bernard      fix the rt_thread_timeout bug
 */

#include <rtthread.h>
#include <rthw.h>
#include "kservice.h"

/*#define THREAD_DEBUG */

extern rt_list_t rt_thread_priority_table[RT_THREAD_PRIORITY_MAX];
extern struct rt_thread* rt_current_thread;
extern rt_uint8_t rt_current_priority;

#ifdef RT_USING_HEAP
extern rt_list_t rt_thread_defunct;
#endif

static void rt_thread_exit(void);
void rt_thread_timeout(void* parameter);

static rt_err_t _rt_thread_init(struct rt_thread* thread,
	const char* name,
	void (*entry)(void* parameter), void* parameter,
	void* stack_start, rt_uint32_t stack_size,
	rt_uint8_t priority, rt_uint32_t tick)
{
	/* set thread id and init thread list */
	thread->tid = thread;
	rt_list_init(&(thread->tlist));

	thread->entry = (void*)entry;
	thread->parameter = parameter;

	/* stack init */
	thread->stack_addr = stack_start;
	thread->stack_size = stack_size;

	/* init thread stack */
	rt_memset(thread->stack_addr, '#', thread->stack_size);
	thread->sp = (void*)rt_hw_stack_init(thread->entry, thread->parameter,
		(void *) ((char *)thread->stack_addr + thread->stack_size - 4),
		(void*)rt_thread_exit);

	/* priority init */
	RT_ASSERT(priority < RT_THREAD_PRIORITY_MAX);
	thread->init_priority = priority;
	thread->current_priority = priority;

	/* tick init */
	thread->init_tick = tick;
	thread->remaining_tick = tick;

	/* error and flags */
	thread->error = RT_EOK;
	thread->stat  = RT_THREAD_INIT;
	thread->flags = 0;

#ifdef RT_USING_MODULE
	/* init module parent */
	thread->module_parent = RT_NULL;
#endif

	/* init user data */
	thread->user_data = 0;
	
	/* init thread timer */
	rt_timer_init(&(thread->thread_timer),
		thread->name,
		rt_thread_timeout,
		thread,
		0,
		RT_TIMER_FLAG_ONE_SHOT);

	return RT_EOK;
}

/**
 * @addtogroup Thread
 */

/*@{*/

/**
 * This function will init a thread, normally it's used to initialize a static thread object.
 *
 * @param thread the static thread object
 * @param name the name of thread, which shall be unique
 * @param entry the entry function of thread
 * @param parameter the parameter of thread enter function
 * @param stack_start the start address of thread stack
 * @param stack_size the size of thread stack
 * @param priority the priority of thread
 * @param tick the time slice if there are same priority thread
 *
 * @return the operation status, RT_EOK on OK; -RT_ERROR on error
 *
 */
rt_err_t rt_thread_init(struct rt_thread* thread,
	const char* name,
	void (*entry)(void* parameter), void* parameter,
	void* stack_start, rt_uint32_t stack_size,
	rt_uint8_t priority, rt_uint32_t tick)
{
	/* thread check */
	RT_ASSERT(thread != RT_NULL);
	RT_ASSERT(stack_start != RT_NULL);

	/* init thread object */
	rt_object_init((rt_object_t)thread, RT_Object_Class_Thread, name);

	return _rt_thread_init(thread, name, entry, parameter,
		stack_start, stack_size,
		priority, tick);
}

#ifdef RT_USING_HEAP
/**
 * This function will create a thread object and allocate thread object memory and stack.
 *
 * @param name the name of thread, which shall be unique
 * @param entry the entry function of thread
 * @param parameter the parameter of thread enter function
 * @param stack_size the size of thread stack
 * @param priority the priority of thread
 * @param tick the time slice if there are same priority thread
 *
 * @return the created thread object
 *
 */
rt_thread_t rt_thread_create (const char* name,
	void (*entry)(void* parameter), void* parameter,
	rt_uint32_t stack_size,
	rt_uint8_t priority,
	rt_uint32_t tick)
{
	struct rt_thread* thread;
	void* stack_start;

	thread = (struct rt_thread*) rt_object_allocate(RT_Object_Class_Thread, name);
	if (thread == RT_NULL) return RT_NULL;

	stack_start = (void*)rt_malloc(stack_size);
	if (stack_start == RT_NULL)
	{
		/* allocate stack failure */
		rt_object_delete((rt_object_t)thread);
		return RT_NULL;
	}

	_rt_thread_init(thread, name, entry, parameter,
		stack_start, stack_size,
		priority, tick);

	return thread;
}
#endif

/**
 * This function will return self thread object
 *
 * @return the self thread object
 *
 */
rt_thread_t rt_thread_self (void)
{
	return rt_current_thread;
}

/**
 * This function will start a thread and put it to system ready queue
 *
 * @param thread the thread to be started
 *
 * @return the operation status, RT_EOK on OK; -RT_ERROR on error
 *
 */
rt_err_t rt_thread_startup (rt_thread_t thread)
{
	/* thread check */
	RT_ASSERT(thread != RT_NULL);
	RT_ASSERT(thread->stat == RT_THREAD_INIT);

	/* set current priority to init priority */
	thread->current_priority = thread->init_priority;

	/* calculate priority attribute */
#if RT_THREAD_PRIORITY_MAX > 32
	thread->number 		= thread->current_priority >> 3; 			/* 5bit */
	thread->number_mask	= 1 << thread->number;
	thread->high_mask 	= 1 << (thread->current_priority & 0x07); 	/* 3bit */
#else
	thread->number_mask = 1 << thread->current_priority;
#endif

#ifdef THREAD_DEBUG
	rt_kprintf("startup a thread:%s with priority:%d\n", thread->name, thread->init_priority);
#endif

	/* change thread stat */
	thread->stat = RT_THREAD_SUSPEND;
	/* then resume it */
	rt_thread_resume(thread);

	return RT_EOK;
}

static void rt_thread_exit()
{
	struct rt_thread* thread;
    register rt_base_t temp;

    /* disable interrupt */
    temp = rt_hw_interrupt_disable();

    /* get current thread */
	thread = rt_current_thread;

	/* remove from schedule */
	rt_schedule_remove_thread(thread);

	/* change stat */
	thread->stat = RT_THREAD_CLOSE;

	/* release thread timer */
	rt_timer_detach(&(thread->thread_timer));

	/* enable interrupt */
    rt_hw_interrupt_enable(temp);

	if (rt_object_is_systemobject((rt_object_t)thread) == RT_EOK)
	{
		rt_object_detach((rt_object_t)thread);
	}
#ifdef RT_USING_HEAP
	else
	{
		/* disable interrupt */
		temp = rt_hw_interrupt_disable();

		/* insert to defunct thread list */
		rt_list_insert_after(&rt_thread_defunct, &(thread->tlist));

		/* enable interrupt */
		rt_hw_interrupt_enable(temp);
	}
#endif

	/* switch to next task */
	rt_schedule();
}

/**
 * This function will detach a thread. The thread object will be remove from thread
 * queue and detached/deleted from system object management.
 *
 * @param thread the thread to be deleted
 *
 * @return the operation status, RT_EOK on OK; -RT_ERROR on error
 *
 */
rt_err_t rt_thread_detach (rt_thread_t thread)
{
	/* thread check */
	RT_ASSERT(thread != RT_NULL);

	/* remove from schedule */
	rt_schedule_remove_thread(thread);

	/* release thread timer */
	rt_timer_detach(&(thread->thread_timer));

	rt_object_detach((rt_object_t)thread);

	return RT_EOK;
}

#ifdef RT_USING_HEAP
/**
 * This function will delete a thread. The thread object will be remove from thread
 * queue and detached/deleted from system object management.
 *
 * @param thread the thread to be deleted
 *
 * @return the operation status, RT_EOK on OK; -RT_ERROR on error
 *
 */
rt_err_t rt_thread_delete (rt_thread_t thread)
{
	rt_base_t lock;

	/* thread check */
	RT_ASSERT(thread != RT_NULL);

	/* remove from schedule */
	rt_schedule_remove_thread(thread);

	/* release thread timer */
	rt_timer_detach(&(thread->thread_timer));

	/* change stat */
	thread->stat = RT_THREAD_CLOSE;

	/* disable interrupt */
	lock = rt_hw_interrupt_disable();

	/* insert to defunct thread list */
	rt_list_insert_after(&rt_thread_defunct, &(thread->tlist));

	/* enable interrupt */
	rt_hw_interrupt_enable(lock);

	return RT_EOK;
}
#endif

/**
 * This function will let current thread yield processor, and scheduler will get a highest thread to run.
 * After yield processor, the current thread is still in READY state.
 *
 * @return the operation status, RT_EOK on OK; -RT_ERROR on error
 *
 */
rt_err_t rt_thread_yield ()
{
	register rt_base_t level;
	struct rt_thread *thread;

	/* disable interrupt */
	level = rt_hw_interrupt_disable();

	/* set to current thread */
	thread = rt_current_thread;

	/* if the thread stat is READY and on ready queue list */
	if (thread->stat == RT_THREAD_READY && thread->tlist.next != thread->tlist.prev)
	{
		/* remove thread from thread list */
		rt_list_remove(&(thread->tlist));

		/* put thread to end of ready queue */
		rt_list_insert_before(&(rt_thread_priority_table[thread->current_priority]),
			&(thread->tlist));

		/* enable interrupt */
		rt_hw_interrupt_enable(level);

		rt_schedule();

		return RT_EOK;
	}

	/* enable interrupt */
	rt_hw_interrupt_enable(level);

	return RT_EOK;
}

/**
 * This function will let current thread sleep for some ticks.
 *
 * @param tick the sleep ticks
 *
 * @return the operation status, RT_EOK on OK; RT_ERROR on error
 *
 */
rt_err_t rt_thread_sleep (rt_tick_t tick)
{
	register rt_base_t temp;
	struct rt_thread *thread;

	/* disable interrupt */
	temp = rt_hw_interrupt_disable();
	/* set to current thread */
	thread = rt_current_thread;
	RT_ASSERT(thread != RT_NULL);

	/* suspend thread */
	rt_thread_suspend(thread);

	/* reset the timeout of thread timer and start it */
	rt_timer_control(&(thread->thread_timer), RT_TIMER_CTRL_SET_TIME, &tick);
	rt_timer_start(&(thread->thread_timer));

	/* enable interrupt */
	rt_hw_interrupt_enable(temp);

	rt_schedule();

	return RT_EOK;
}

/**
 * This function will let current thread delay for some ticks.
 *
 * @param tick the delay ticks
 *
 * @return the operation status, RT_EOK on OK; RT_ERROR on error
 *
 */
rt_err_t rt_thread_delay(rt_tick_t tick)
{
	return rt_thread_sleep(tick);
}

rt_err_t rt_thread_control (rt_thread_t thread, rt_uint8_t cmd, void* arg)
{
	register rt_base_t temp;

	/* thread check */
	RT_ASSERT(thread != RT_NULL);

	switch (cmd)
	{
	case RT_THREAD_CTRL_CHANGE_PRIORITY:
		/* disable interrupt */
		temp = rt_hw_interrupt_disable();

		/* for ready thread, change queue */
		if (thread->stat == RT_THREAD_READY)
		{
			/* remove thread from schedule queue first */
			rt_schedule_remove_thread(thread);

			/* change thread priority */
			thread->current_priority = *(rt_uint8_t*) arg;

			/* recalculate priority attribute */
#if RT_THREAD_PRIORITY_MAX > 32
			thread->number 		= thread->current_priority >> 3; 			/* 5bit */
			thread->number_mask	= 1 << thread->number;
			thread->high_mask 	= 1 << (thread->current_priority & 0x07); 	/* 3bit */
#else
			thread->number_mask = 1 << thread->current_priority;
#endif

			/* insert thread to schedule queue again */
			rt_schedule_insert_thread(thread);
		}
		else
		{
			thread->current_priority = *(rt_uint8_t*) arg;

			/* recalculate priority attribute */
#if RT_THREAD_PRIORITY_MAX > 32
			thread->number 		= thread->current_priority >> 3; 			/* 5bit */
			thread->number_mask	= 1 << thread->number;
			thread->high_mask 	= 1 << (thread->current_priority & 0x07); 	/* 3bit */
#else
			thread->number_mask = 1 << thread->current_priority;
#endif
		}

		/* enable interrupt */
		rt_hw_interrupt_enable(temp);
		break;

	case RT_THREAD_CTRL_STARTUP:
		return rt_thread_startup(thread);

#ifdef RT_USING_HEAP
	case RT_THREAD_CTRL_CLOSE:
		return rt_thread_delete(thread);
#endif

	default:
		break;
	}

	return - RT_EOK;
}

/**
 * This function will suspend the specified thread.
 *
 * @param thread the thread to be suspended
 *
 * @return the operation status, RT_EOK on OK; -RT_ERROR on error
 *
 */
rt_err_t rt_thread_suspend (rt_thread_t thread)
{
	register rt_base_t temp;

	/* thread check */
	RT_ASSERT(thread != RT_NULL);

#ifdef THREAD_DEBUG
	rt_kprintf("thread suspend:  %s\n", thread->name);
#endif

	if (thread->stat != RT_THREAD_READY)
	{
#ifdef THREAD_DEBUG
		rt_kprintf("thread suspend: thread disorder, %d\n", thread->stat);
#endif
		return -RT_ERROR;
	}

	/* disable interrupt */
	temp = rt_hw_interrupt_disable();

	/* change thread stat */
	thread->stat = RT_THREAD_SUSPEND;
	rt_schedule_remove_thread(thread);

	/* enable interrupt */
	rt_hw_interrupt_enable(temp);

	return RT_EOK;
}

/**
 * This function will resume a thread and put it to system ready queue.
 *
 * @param thread the thread to be resumed
 *
 * @return the operation status, RT_EOK on OK; -RT_ERROR on error
 *
 */
rt_err_t rt_thread_resume (rt_thread_t thread)
{
	register rt_base_t temp;

	/* thread check */
	RT_ASSERT(thread != RT_NULL);

#ifdef THREAD_DEBUG
	rt_kprintf("thread resume:  %s\n", thread->name);
#endif

	if (thread->stat != RT_THREAD_SUSPEND)
	{
#ifdef THREAD_DEBUG
		rt_kprintf("thread resume: thread disorder, %d\n", thread->stat);
#endif
		return -RT_ERROR;
	}

	/* disable interrupt */
	temp = rt_hw_interrupt_disable();

	/* remove from suspend list */
	rt_list_remove(&(thread->tlist));

	/* remove thread timer */
	rt_list_remove(&(thread->thread_timer.list));

	/* change timer state */
	thread->thread_timer.parent.flag &= ~RT_TIMER_FLAG_ACTIVATED;

	/* enable interrupt */
	rt_hw_interrupt_enable(temp);

	/* insert to schedule ready list */
	rt_schedule_insert_thread(thread);

	return RT_EOK;
}

/**
 * This function is the timeout function for thread, normally which will
 * be invoked when thread is timeout to wait some recourse.
 *
 * @param parameter the parameter of thread timeout function
 *
 */
void rt_thread_timeout(void* parameter)
{
	struct rt_thread* thread;

	thread = (struct rt_thread*) parameter;

	/* thread check */
	RT_ASSERT(thread != RT_NULL);
	RT_ASSERT(thread->stat == RT_THREAD_SUSPEND);

	/* set error number */
	thread->error = -RT_ETIMEOUT;

	/* remove from suspend list */
	rt_list_remove(&(thread->tlist));

	/* insert to schedule ready list */
	rt_schedule_insert_thread(thread);

	/* do schedule */
	rt_schedule();
}

/**
 * This function will find the specified thread.
 *
 * @param name the name of thread finding
 *
 * @return the thread
 */
rt_thread_t rt_thread_find(char* name)
{
	struct rt_thread* thread;

	thread = (struct rt_thread*)rt_object_find(RT_Object_Class_Thread, name);

	return thread;
}

/*@}*/
