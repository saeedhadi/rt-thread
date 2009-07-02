/*
 * File      : ipc.c
 * This file is part of RT-Thread RTOS
 * COPYRIGHT (C) 2006, RT-Thread Development Team
 *
 * The license and distribution terms for this file may be
 * found in the file LICENSE in this distribution or at
 * http://openlab.rt-thread.com/license/LICENSE
 *
 * Change Logs:
 * Date           Author       Notes
 * 2006-03-14     Bernard      the first version
 * 2006-04-25     Bernard      implement semaphore
 * 2006-05-03     Bernard      add IPC_DEBUG
 *                             modify the type of IPC waiting time to rt_int32_t
 * 2006-05-10     Bernard      fix the semaphore take bug and add IPC object
 * 2006-05-12     Bernard      implement mailbox and message queue
 * 2006-05-20     Bernard      implement mutex
 * 2006-05-23     Bernard      implement fast event
 * 2006-05-24     Bernard      implement event
 * 2006-06-03     Bernard      fix the thread timer init bug
 * 2006-06-05     Bernard      fix the mutex release bug
 * 2006-06-07     Bernard      fix the message queue send bug
 * 2006-08-04     Bernard      add hook support
 * 2009-05-21     Yi.qiu          fix the sem release bug
 */

#include <rtthread.h>
#include <rthw.h>

#include "kservice.h"

/* #define IPC_DEBUG */

#ifdef RT_USING_HOOK
extern void (*rt_object_trytake_hook)(struct rt_object* object);
extern void (*rt_object_take_hook)(struct rt_object* object);
extern void (*rt_object_put_hook)(struct rt_object* object);
#endif

/**
 * @addtogroup IPC
 */

/*@{*/

/**
 * This function will initialize an IPC object
 *
 * @param ipc the IPC object
 *
 * @return the operation status, RT_EOK on successful
 */
rt_inline rt_err_t rt_ipc_object_init(struct rt_ipc_object *ipc)
{
	/* init ipc object */
	rt_list_init(&(ipc->suspend_thread));
	ipc->suspend_thread_count = 0;

	return RT_EOK;
}

/**
 * This function will suspend a thread for a specified IPC object and put the
 * thread into suspend queue of IPC object
 *
 * @param ipc the IPC object
 * @param thread the thread object to be suspended
 *
 * @return the operation status, RT_EOK on successful
 */
rt_inline rt_err_t rt_ipc_object_suspend(struct rt_ipc_object *ipc, struct rt_thread *thread)
{
	/* suspend thread */
	rt_thread_suspend(thread);
	ipc->suspend_thread_count ++;

	switch (ipc->parent.flag)
	{
	case RT_IPC_FLAG_FIFO:
		rt_list_insert_before(&(ipc->suspend_thread), &(thread->tlist));
		break;

	case RT_IPC_FLAG_PRIO:
		{
			struct rt_list_node* n;
			struct rt_thread* sthread;

			/* find a suitable position */
			for (n = ipc->suspend_thread.next; n != &(ipc->suspend_thread);
				n = n->next)
			{
				sthread = rt_list_entry(n, struct rt_thread, tlist);

				/* find out */
				if (thread->current_priority < sthread->current_priority) break;
			}

			rt_list_insert_before(&(ipc->suspend_thread), &(thread->tlist));
		}
		break;
	}

	return RT_EOK;
}

/**
 * This function will resume a thread from an IPC object:
 * - remove the thread from suspend queue of IPC object
 * - put the thread into system ready queue
 *
 * @param ipc the IPC object
 *
 * @return the operation status, RT_EOK on successful
 */
rt_inline rt_err_t rt_ipc_object_resume(struct rt_ipc_object* ipc)
{
	struct rt_thread *thread;

	/* get thread entry */
	thread = rt_list_entry(ipc->suspend_thread.next, struct rt_thread, tlist);

#ifdef IPC_DEBUG
	rt_kprintf("resume thread:%s\n", thread->name);
#endif

	/* resume it */
	rt_thread_resume(thread);

	/* decrease suspended thread count */
	ipc->suspend_thread_count --;

	return RT_EOK;
}

/**
 * This function will resume all suspended threads in an IPC object.
 *
 * @param ipc the IPC object
 *
 * @return the operation status, RT_EOK on successful
 */
rt_inline rt_err_t rt_ipc_object_resume_all(struct rt_ipc_object* ipc)
{
	struct rt_thread* thread;
	register rt_ubase_t temp;

	/* wakeup all suspend threads */
	while (!rt_list_isempty(&(ipc->suspend_thread)))
	{
		/* disable interrupt */
		temp = rt_hw_interrupt_disable();

		/* get next suspend thread */
		thread = rt_list_entry(ipc->suspend_thread.next, struct rt_thread, tlist);
		/* set error code to RT_ERROR */
		thread->error = -RT_ERROR;

		/*
		 * resume thread
		 * In rt_thread_resume function, it will remove current thread from
		 * suspend list
		 */
		rt_thread_resume(thread);

		/* decrease suspended thread count */
		ipc->suspend_thread_count --;

		/* enable interrupt */
		rt_hw_interrupt_enable(temp);
	}

	return RT_EOK;
}

/* decrease ipc suspended thread number when thread can not take resource successfully */
rt_inline void rt_ipc_object_decrease(struct rt_ipc_object* ipc)
{
	register rt_ubase_t level;

	/* disable interrupt */
	level = rt_hw_interrupt_disable();

	/* decrease suspended thread count */
	ipc->suspend_thread_count --;

	/* enable interrupt */
	rt_hw_interrupt_enable(level);
}
#ifdef RT_USING_SEMAPHORE

/**
 * This function will initialize a semaphore and put it under control of resource
 * management.
 *
 * @param sem the semaphore object
 * @param name the name of semaphore
 * @param value the init value of semaphore
 * @param flag the flag of semaphore
 *
 * @return the operation status, RT_EOK on successful
 */
rt_err_t rt_sem_init (rt_sem_t sem, const char* name, rt_uint32_t value, rt_uint8_t flag)
{
	RT_ASSERT(sem != RT_NULL);

	/* init object */
	rt_object_init(&(sem->parent.parent), RT_Object_Class_Semaphore, name);

	/* init ipc object */
	rt_ipc_object_init(&(sem->parent));

	/* set init value */
	sem->value	= value;

	/* set parent */
	sem->parent.parent.flag = flag;

	return RT_EOK;
}

/**
 * This function will detach a semaphore from resource management
 *
 * @param sem the semaphore object
 *
 * @return the operation status, RT_EOK on successful
 *
 * @see rt_sem_delete
 */
rt_err_t rt_sem_detach (rt_sem_t sem)
{
	RT_ASSERT(sem != RT_NULL);

	/* wakeup all suspend threads */
	rt_ipc_object_resume_all(&(sem->parent));

	/* detach semaphore object */
	rt_object_detach(&(sem->parent.parent));

	return RT_EOK;
}

#ifdef RT_USING_HEAP
/**
 * This function will create a semaphore from system resource
 *
 * @param name the name of semaphore
 * @param value the init value of semaphore
 * @param flag the flag of semaphore
 *
 * @return the created semaphore, RT_NULL on error happen
 *
 * @see rt_sem_init
 */
rt_sem_t rt_sem_create (const char* name, rt_uint32_t value, rt_uint8_t flag)
{
	rt_sem_t sem;

	/* allocate object */
	sem = (rt_sem_t) rt_object_allocate(RT_Object_Class_Semaphore, name);
	if (sem == RT_NULL) return sem;

	/* init ipc object */
	rt_ipc_object_init(&(sem->parent));

	/* set init value */
	sem->value	= value;

	/* set parent */
	sem->parent.parent.flag = flag;

	return sem;
}

/**
 * This function will delete a semaphore object and release the memory
 *
 * @param sem the semaphore object
 *
 * @return the error code
 *
 * @see rt_sem_detach
 */
rt_err_t rt_sem_delete (rt_sem_t sem)
{
	RT_ASSERT(sem != RT_NULL);

	/* wakeup all suspend threads */
	rt_ipc_object_resume_all(&(sem->parent));

	/* delete semaphore object */
	rt_object_delete(&(sem->parent.parent));

	return RT_EOK;
}
#endif

/**
 * This function will take a semaphore, if the semaphore is unavailable, the
 * thread shall wait for a specified time.
 *
 * @param sem the semaphore object
 * @param time the waiting time
 *
 * @return the error code
 */
rt_err_t rt_sem_take (rt_sem_t sem, rt_int32_t time)
{
	register rt_base_t temp;
	struct rt_thread* thread;

	RT_ASSERT(sem != RT_NULL);

#ifdef RT_USING_HOOK
	if (rt_object_trytake_hook != RT_NULL) rt_object_trytake_hook(&(sem->parent.parent));
#endif

	/* disable interrupt */
	temp = rt_hw_interrupt_disable();

#ifdef IPC_DEBUG
	rt_kprintf("thread %s take sem:%s, which value is: %d\n", rt_thread_self()->name, 
		((struct rt_object*)sem)->name, sem->value);
#endif
	if (sem->value > 0)
	{
		/* semaphore is available */
		sem->value --;

		/* enable interrupt */
		rt_hw_interrupt_enable(temp);
	}
	else
	{
		/* no waiting, return with timeout */
		if (time == 0 )
		{
			rt_hw_interrupt_enable(temp);
			return -RT_ETIMEOUT;
		}
		else
		{
			/* semaphore is unavailable, push to suspend list */
			sem->value --;

			/* get current thread */
			thread = rt_thread_self();

			/* reset thread error number */
			thread->error = RT_EOK;

#ifdef IPC_DEBUG
			rt_kprintf("sem take: suspend thread - %s\n", thread->name);
#endif

			/* suspend thread */
			rt_ipc_object_suspend(&(sem->parent), thread);

			/* has waiting time, start thread timer */
			if (time > 0)
			{
#ifdef IPC_DEBUG
				rt_kprintf("set thread:%s to timer list\n", thread->name);
#endif
				/* reset the timeout of thread timer and start it */
				rt_timer_control(&(thread->thread_timer), RT_TIMER_CTRL_SET_TIME, &time);
				rt_timer_start(&(thread->thread_timer));
			}

			/* enable interrupt */
			rt_hw_interrupt_enable(temp);

			/* do schedule */
			rt_schedule();

			if (thread->error != RT_EOK)
			{
				/* decrease suspended thread count */
				rt_ipc_object_decrease(&(sem->parent));
				return thread->error;
			}
		}
	}

#ifdef RT_USING_HOOK
	if (rt_object_take_hook != RT_NULL) rt_object_take_hook(&(sem->parent.parent));
#endif

	return RT_EOK;
}

/**
 * This function will try to take a semaphore and immediately return
 *
 * @param sem the semaphore object
 *
 * @return the error code
 */
rt_err_t rt_sem_trytake(rt_sem_t sem)
{
	return rt_sem_take(sem, 0);
}

/**
 * This function will release a semaphore, if there are threads suspended on
 * semaphore, it will be waked up.
 *
 * @param sem the semaphore object
 *
 * @return the error code
 */
rt_err_t rt_sem_release(rt_sem_t sem)
{
	register rt_base_t temp;

#ifdef RT_USING_HOOK
	if (rt_object_put_hook != RT_NULL) rt_object_put_hook(&(sem->parent.parent));
#endif

	/* disable interrupt */
	temp = rt_hw_interrupt_disable();

#ifdef IPC_DEBUG
	rt_kprintf("thread %s releases sem:%s, which value is: %d\n", rt_thread_self()->name, 
		((struct rt_object*)sem)->name, sem->value);
#endif
	/* increase value */
	sem->value ++;

	if (sem->value <= 0 && sem->parent.suspend_thread_count > 0)
	{
		rt_ipc_object_resume(&(sem->parent));

		/* enable interrupt */
		rt_hw_interrupt_enable(temp);

		/* resume a thread, re-schedule */
		rt_schedule();
		return RT_EOK;
	}

	/* enable interrupt */
	rt_hw_interrupt_enable(temp);

	return RT_EOK;
}

/**
 * This function can get or set some extra attributions of a semaphore object.
 *
 * @param sem the semaphore object
 * @param cmd the execution command
 * @param arg the execution argument
 *
 * @return the error code
 */
rt_err_t rt_sem_control(rt_sem_t sem, rt_uint8_t cmd, void* arg)
{
	return RT_EOK;
}

#endif /* end of RT_USING_SEMAPHORE */

#ifdef RT_USING_MUTEX

/**
 * This function will initialize a mutex and put it under control of resource
 * management.
 *
 * @param mutex the mutex object
 * @param name the name of mutex
 * @param flag the flag of mutex
 *
 * @return the operation status, RT_EOK on successful
 */
rt_err_t rt_mutex_init (rt_mutex_t mutex, const char* name, rt_uint8_t flag)
{
	RT_ASSERT(mutex != RT_NULL);

	/* init object */
	rt_object_init(&(mutex->parent.parent), RT_Object_Class_Mutex, name);

	/* init ipc object */
	rt_ipc_object_init(&(mutex->parent));

	mutex->value = 1;
	mutex->owner = RT_NULL;
	mutex->original_priority = 0xFF;
	mutex->hold  = 0;

	/* set flag */
	mutex->parent.parent.flag = flag;

	return RT_EOK;
}

/**
 * This function will detach a mutex from resource management
 *
 * @param mutex the mutex object
 *
 * @return the operation status, RT_EOK on successful
 *
 * @see rt_mutex_delete
 */
rt_err_t rt_mutex_detach (rt_mutex_t mutex)
{
	RT_ASSERT(mutex != RT_NULL);

	/* wakeup all suspend threads */
	rt_ipc_object_resume_all(&(mutex->parent));

	/* detach semaphore object */
	rt_object_detach(&(mutex->parent.parent));

	return RT_EOK;
}

#ifdef RT_USING_HEAP
/**
 * This function will create a mutex from system resource
 *
 * @param name the name of mutex
 * @param flag the flag of mutex
 *
 * @return the created mutex, RT_NULL on error happen
 *
 * @see rt_mutex_init
 */
rt_mutex_t rt_mutex_create (const char* name, rt_uint8_t flag)
{
	struct rt_mutex *mutex;

	/* allocate object */
	mutex = (rt_mutex_t) rt_object_allocate(RT_Object_Class_Mutex, name);
	if (mutex == RT_NULL) return mutex;

	/* init ipc object */
	rt_ipc_object_init(&(mutex->parent));

	mutex->value = 1;
	mutex->owner = RT_NULL;
	mutex->original_priority = 0xFF;
	mutex->hold  = 0;

	/* set flag */
	mutex->parent.parent.flag = flag;

	return mutex;
}

/**
 * This function will delete a mutex object and release the memory
 *
 * @param mutex the mutex object
 *
 * @return the error code
 *
 * @see rt_mutex_detach
 */
rt_err_t rt_mutex_delete (rt_mutex_t mutex)
{
	RT_ASSERT(mutex != RT_NULL);

	/* wakeup all suspend threads */
	rt_ipc_object_resume_all(&(mutex->parent));

	/* delete semaphore object */
	rt_object_delete(&(mutex->parent.parent));

	return RT_EOK;
}
#endif

/**
 * This function will take a mutex, if the mutex is unavailable, the
 * thread shall wait for a specified time.
 *
 * @param mutex the mutex object
 * @param time the waiting time
 *
 * @return the error code
 */
rt_err_t rt_mutex_take (rt_mutex_t mutex, rt_int32_t time)
{
	register rt_base_t temp;
	struct rt_thread* thread;

	RT_ASSERT(mutex != RT_NULL);

#ifdef RT_USING_HOOK
	if (rt_object_trytake_hook != RT_NULL) rt_object_trytake_hook(&(mutex->parent.parent));
#endif

	/* disable interrupt */
	temp = rt_hw_interrupt_disable();

#ifdef IPC_DEBUG
	rt_kprintf("mutex_take:mutex value: %d, hold: %d\n", mutex->value, mutex->hold);
#endif

	/* get current thread */
	thread = rt_thread_self();

	/* reset thread error */
	thread->error = RT_EOK;

	if (mutex->owner == thread)
	{
		/* it's the same thread */
		mutex->hold ++;
	}
	else
	{
		if (mutex->value > 0)
		{
			/* mutex is available */
			mutex->value --;

			/* set mutex owner and original priority */
			mutex->owner = thread;
			mutex->original_priority = thread->current_priority;
			mutex->hold ++;
		}
		else
		{
			/* no waiting, return with timeout */
			if (time == 0 )
			{
				/* set error as timeout */
				thread->error = -RT_ETIMEOUT;

				/* enable interrupt */
				rt_hw_interrupt_enable(temp);

				return -RT_ETIMEOUT;
			}
			else
			{
				/* mutex is unavailable, push to suspend list */
				mutex->value --;

#ifdef IPC_DEBUG
				rt_kprintf("sem take: suspend thread: %s\n", thread->name);
#endif
				/* change the owner thread priority of mutex */
				if (thread->current_priority < mutex->owner->current_priority)
				{
					/* change the owner thread priority */
					rt_thread_control(mutex->owner, RT_THREAD_CTRL_CHANGE_PRIORITY,
						&thread->current_priority);
				}

				/* suspend current thread */
				rt_ipc_object_suspend(&(mutex->parent), thread);

				/* has waiting time, start thread timer */
				if (time > 0)
				{
#ifdef IPC_DEBUG
					rt_kprintf("set thread:%s to timer list\n", thread->name);
#endif
					/* reset the timeout of thread timer and start it */
					rt_timer_control(&(thread->thread_timer), RT_TIMER_CTRL_SET_TIME, &time);
					rt_timer_start(&(thread->thread_timer));
				}

				/* enable interrupt */
				rt_hw_interrupt_enable(temp);

				/* do schedule */
				rt_schedule();

				if (thread->error != RT_EOK)
				{
					/* decrease suspended thread count */
					rt_ipc_object_decrease(&(mutex->parent));

					/* return error */
					return thread->error;
				}
				else
				{
					/* disable interrupt */
					temp = rt_hw_interrupt_disable();

					/* take mutex */
					mutex->owner = thread;
					mutex->hold ++;

					/* set thread error */
					thread->error = RT_EOK;
				}
			}
		}
	}

	/* enable interrupt */
	rt_hw_interrupt_enable(temp);

#ifdef RT_USING_HOOK
	if (rt_object_take_hook != RT_NULL) rt_object_take_hook(&(mutex->parent.parent));
#endif

	return RT_EOK;
}

/**
 * This function will release a mutex, if there are threads suspended on mutex,
 * it will be waked up.
 *
 * @param mutex the mutex object
 *
 * @return the error code
 */
rt_err_t rt_mutex_release(rt_mutex_t mutex)
{
	register rt_base_t temp;
	struct rt_thread* thread;

	/* disable interrupt */
	temp = rt_hw_interrupt_disable();

#ifdef IPC_DEBUG
	rt_kprintf("mutex_release:mutex value: %d, hold: %d\n", mutex->value, mutex->hold);
#endif

#ifdef RT_USING_HOOK
	if (rt_object_put_hook != RT_NULL) rt_object_put_hook(&(mutex->parent.parent));
#endif

	/* get current thread */
	thread = rt_thread_self();

	/* mutex is only released by owner */
	if (thread != mutex->owner)
	{
		thread->error = -RT_ERROR;

		/* enable interrupt */
		rt_hw_interrupt_enable(temp);

		return -RT_ERROR;
	}

	/* decrease hold */
	mutex->hold --;

	/* if no hold */
	if (mutex->hold == 0)
	{
		/* change the owner thread to original priority */
		if (mutex->owner->init_priority != mutex->owner->current_priority)
		{
			rt_thread_control(mutex->owner, RT_THREAD_CTRL_CHANGE_PRIORITY,
				&(mutex->owner->init_priority));
		}

		/* wakeup suspended thread */
		if (mutex->value <= 0 && mutex->parent.suspend_thread_count > 0)
		{
#ifdef IPC_DEBUG
		rt_kprintf("mutex release: resume thread: %s\n", thread->name);
#endif

			/* resume thread */
			rt_ipc_object_resume(&(mutex->parent));
		}

		/* increase value */
		mutex->value ++;

	/* enable interrupt */
	rt_hw_interrupt_enable(temp);

	rt_schedule();
		return RT_EOK;
	}

	/* enable interrupt */
	rt_hw_interrupt_enable(temp);

	return RT_EOK;
}

/**
 * This function can get or set some extra attributions of a mutex object.
 *
 * @param mutex the mutex object
 * @param cmd the execution command
 * @param arg the execution argument
 *
 * @return the error code
 */
rt_err_t rt_mutex_control(rt_mutex_t mutex, rt_uint8_t cmd, void* arg)
{
	return RT_EOK;
}

#endif /* end of RT_USING_MUTEX */

#ifdef RT_USING_FASTEVENT

/**
 * This function will initialize a fast event and put it under control of resource
 * management.
 *
 * @param event the fast event object
 * @param name the name of fast event
 * @param flag the flag of fast event
 *
 * @return the operation status, RT_EOK on successful
 */
rt_err_t rt_fast_event_init(rt_fast_event_t event, const char* name, rt_uint8_t flag)
{
	register rt_base_t offset;
	RT_ASSERT(event != RT_NULL);

	/* init object */
	rt_object_init(&(event->parent), RT_Object_Class_FastEvent, name);

	/* set parent */
	event->parent.flag = flag;

	/* clear event set */
	event->set = 0x00;

	/* init thread list */
	for (offset = 0; offset < 32; offset ++)
	{
		rt_list_init(&(event->thread_list[offset]));
	}

	return RT_EOK;
}

/**
 * This function will detach a fast event from resource management
 *
 * @param event the fast event object
 *
 * @return the operation status, RT_EOK on successful
 */
rt_err_t rt_fast_event_detach(rt_fast_event_t event)
{
	register rt_base_t bit;
	struct rt_thread* thread;
	register rt_ubase_t level;

	RT_ASSERT(event != RT_NULL);

	for (bit = 0; bit < RT_EVENT_LENGTH; bit ++)
	{
		/* resume all suspend thread */
		if (!rt_list_isempty(&(event->thread_list[bit])))
		{
			/* wakeup all suspend threads */
			while (!rt_list_isempty(&(event->thread_list[bit])))
			{
				/* disable interrupt */
				level = rt_hw_interrupt_disable();

				/* get next suspend thread */
				thread = rt_list_entry(event->thread_list[bit].next, struct rt_thread, tlist);
				/* set error code to RT_ERROR */
				thread->error = -RT_ERROR;

				/* resume thread */
				rt_thread_resume(thread);

				/* enable interrupt */
				rt_hw_interrupt_enable(level);
			}

		}
	}

	/* detach event object */
	rt_object_detach(&(event->parent));

	return RT_EOK;
}

#ifdef RT_USING_HEAP
/**
 * This function will create a fast event object from system resource
 *
 * @param name the name of fast event
 * @param flag the flag of fast event
 *
 * @return the created fast event, RT_NULL on error happen
 */
rt_fast_event_t rt_fast_event_create (const char* name, rt_uint8_t flag)
{
	rt_fast_event_t event;
	register rt_base_t offset;

	/* allocate object */
	event = (rt_fast_event_t) rt_object_allocate(RT_Object_Class_FastEvent, name);
	if (event == RT_NULL) return event;

	/* set parent */
	event->parent.flag = flag;

	/* clear event set */
	event->set = 0x00;

	/* init thread list */
	for (offset = 0; offset < 32; offset ++)
	{
		rt_list_init(&(event->thread_list[offset]));
	}

	return event;
}

/**
 * This function will delete a fast event object and release the memory
 *
 * @param event the fast event object
 *
 * @return the error code
 */
rt_err_t rt_fast_event_delete (rt_fast_event_t event)
{
	register rt_base_t bit;
	struct rt_thread* thread;
	register rt_ubase_t level;

	RT_ASSERT(event != RT_NULL);

	for (bit = 0; bit < RT_EVENT_LENGTH; bit ++)
	{
		/* resume all suspend thread */
		if (!rt_list_isempty(&(event->thread_list[bit])))
		{
			/* wakeup all suspend threads */
			while (!rt_list_isempty(&(event->thread_list[bit])))
			{
				/* disable interrupt */
				level = rt_hw_interrupt_disable();

				/* get next suspend thread */
				thread = rt_list_entry(event->thread_list[bit].next, struct rt_thread, tlist);
				/* set error code to RT_ERROR */
				thread->error = -RT_ERROR;

				/* resume thread */
				rt_thread_resume(thread);

				/* enable interrupt */
				rt_hw_interrupt_enable(level);
			}

		}
	}

	/* detach semaphore object */
	rt_object_delete(&(event->parent));

	return RT_EOK;
}
#endif

/**
 * This function will send an event to the fast event object, if there are threads
 * suspended on fast event object, it will be waked up.
 *
 * @param event the fast event object
 * @param bit the event bit
 *
 * @return the error code
 */
rt_err_t rt_fast_event_send(rt_fast_event_t event, rt_uint8_t bit)
{
	rt_uint32_t offset;
	register rt_ubase_t level;
	struct rt_thread *thread;
	struct rt_list_node *n;

	/* parameter check */
	RT_ASSERT(event != RT_NULL);
	RT_ASSERT(bit < RT_EVENT_LENGTH);

	offset = 1 << bit;

#ifdef RT_USING_HOOK
	if (rt_object_put_hook != RT_NULL) rt_object_put_hook(&(event->parent));
#endif

	/* disable interrupt */
	level = rt_hw_interrupt_disable();

	event->set |= offset;

	/* if thread list at offset is not empty */
	n = event->thread_list[bit].next;
	while (n != &(event->thread_list[bit]))
	{
		/* get thread */
		thread = rt_list_entry(n, struct rt_thread, tlist);

		/* move to next node */
		n = n->next;

		/* clear bit or not */
		if (thread->event_info & RT_EVENT_FLAG_CLEAR)
			event->set &= ~offset;

		/* resume thread */
		rt_thread_resume(thread);
	}

	/* enable interrupt */
	rt_hw_interrupt_enable(level);

	/* do a schedule */
	rt_schedule();

	return RT_EOK;
}

/**
 * This function will receive an event from fast event object, if the event is
 * unavailable, the thread shall wait for a specified time.
 *
 * @param event the fast event object
 * @param bit the interested event
 * @param option the receive option
 * @param timeout the waiting time
 *
 * @return the error code
 */
rt_err_t rt_fast_event_recv(rt_fast_event_t event, rt_uint8_t bit, rt_uint8_t option, rt_int32_t timeout)
{
 	rt_base_t offset;
	struct rt_thread* thread;
	register rt_ubase_t level;

	/* parameter check */
	RT_ASSERT(event != RT_NULL);
	RT_ASSERT(bit < RT_EVENT_LENGTH);

	offset = 1 << bit;

#ifdef RT_USING_HOOK
	if (rt_object_trytake_hook != RT_NULL) rt_object_trytake_hook(&(event->parent));
#endif

	/* disable interrupt */
	level = rt_hw_interrupt_disable();

	/* get current thread */
	thread = rt_thread_self();
	thread->error = RT_EOK;

	/* get event successfully */
	if (event->set & offset)
	{
		if (option & RT_EVENT_FLAG_CLEAR)
			event->set &= ~ offset;

		/* enable interrupt */
		rt_hw_interrupt_enable(level);

		return RT_EOK;
	}

	/* no event happen */

	/* check waiting time */
	if (timeout == 0)
	{
		/* no waiting */
		thread->error = -RT_ETIMEOUT;
	}
	else
	{
		/* there are no event, suspend thread */
		rt_thread_suspend(thread);

		/* set event info in thread */
		thread->event_info = option;

		switch (event->parent.flag)
		{
		case RT_IPC_FLAG_FIFO:
			rt_list_insert_after(&(event->thread_list[bit]), &(thread->tlist));
			break;

		case RT_IPC_FLAG_PRIO:
			{
				struct rt_list_node* n;
				struct rt_thread* sthread;

				/* find a suitable position */
				for (n = event->thread_list[bit].next; n != &(event->thread_list[bit]); n = n->next)
				{
					sthread = rt_list_entry(n, struct rt_thread, tlist);

					/* find out */
					if (thread->current_priority < sthread->current_priority) break;
				}

				/* insert thread */
				rt_list_insert_before(&(event->thread_list[bit]), &(thread->tlist));
			}
			break;
		}

		/* if there is timeout, active thread timer */
		if (timeout > 0)
		{
			/* reset the timeout of thread timer and start it */
			rt_timer_control(&(thread->thread_timer), RT_TIMER_CTRL_SET_TIME, &timeout);
			rt_timer_start(&(thread->thread_timer));
		}
	}

	/* enable interrupt */
	rt_hw_interrupt_enable(level);

#ifdef RT_USING_HOOK
	if (rt_object_take_hook != RT_NULL) rt_object_take_hook(&(event->parent));
#endif

	return thread->error;
}

/**
 * This function can get or set some extra attributions of a fast event object.
 *
 * @param event the event object
 * @param cmd the execution command
 * @param arg the execution argument
 *
 * @return the error code
 */
rt_err_t rt_fast_event_control (rt_fast_event_t event, rt_uint8_t cmd, void* arg)
{
	return RT_EOK;
}
#endif

#ifdef RT_USING_EVENT

/**
 * This function will initialize an event and put it under control of resource
 * management.
 *
 * @param event the event object
 * @param name the name of event
 * @param flag the flag of event
 *
 * @return the operation status, RT_EOK on successful
 */
rt_err_t rt_event_init(rt_event_t event, const char* name, rt_uint8_t flag)
{
	RT_ASSERT(event != RT_NULL);

	/* init object */
	rt_object_init(&(event->parent.parent), RT_Object_Class_Event, name);

	/* set parent flag */
	event->parent.parent.flag = flag;

	/* init ipc object */
	rt_ipc_object_init(&(event->parent));

	/* init event */
	event->set = 0;

	return RT_EOK;
}

/**
 * This function will detach an event object from resource management
 *
 * @param event the event object
 *
 * @return the operation status, RT_EOK on successful
 */
rt_err_t rt_event_detach(rt_event_t event)
{
	/* parameter check */
	RT_ASSERT(event != RT_NULL);

	/* resume all suspended thread */
	rt_ipc_object_resume_all(&(event->parent));

	/* detach event object */
	rt_object_detach(&(event->parent.parent));

	return RT_EOK;
}

#ifdef RT_USING_HEAP
/**
 * This function will create an event object from system resource
 *
 * @param name the name of event
 * @param flag the flag of event
 *
 * @return the created event, RT_NULL on error happen
 */
rt_event_t rt_event_create (const char* name, rt_uint8_t flag)
{
	rt_event_t event;

	/* allocate object */
	event = (rt_event_t) rt_object_allocate(RT_Object_Class_Event, name);
	if (event == RT_NULL) return event;

	/* set parent */
	event->parent.parent.flag = flag;

	/* init ipc object */
	rt_ipc_object_init(&(event->parent));

	/* init event */
	event->set = 0;

	return event;
}

/**
 * This function will delete an event object and release the memory
 *
 * @param event the event object
 *
 * @return the error code
 */
rt_err_t rt_event_delete (rt_event_t event)
{
	/* parameter check */
	RT_ASSERT(event != RT_NULL);

	/* resume all suspended thread */
	rt_ipc_object_resume_all(&(event->parent));

	/* delete event object */
	rt_object_delete(&(event->parent.parent));

	return RT_EOK;
}
#endif

/**
 * This function will send an event to the event object, if there are threads
 * suspended on event object, it will be waked up.
 *
 * @param event the event object
 * @param set the event set
 *
 * @return the error code
 */
rt_err_t rt_event_send(rt_event_t event, rt_uint32_t set)
{
	struct rt_list_node *n;
	struct rt_thread *thread;
	register rt_ubase_t level;
	register rt_base_t status;

	/* parameter check */
	RT_ASSERT(event != RT_NULL);
	if (set == 0) return -RT_ERROR;

#ifdef RT_USING_HOOK
	if (rt_object_put_hook != RT_NULL) rt_object_put_hook(&(event->parent.parent));
#endif

	/* disable interrupt */
	level = rt_hw_interrupt_disable();

	/* set event */
	event->set |= set;

	if (event->parent.suspend_thread_count > 0)
	{
		/* search thread list to resume thread */
		n = event->parent.suspend_thread.next;
		while (n != &(event->parent.suspend_thread))
		{
			/* get thread */
			thread = rt_list_entry(n, struct rt_thread, tlist);

			status = -RT_ERROR;
			if (thread->event_info & RT_EVENT_FLAG_AND)
			{
				if ((thread->event_set & event->set) == thread->event_set)
				{
					status = RT_EOK;
				}
			}
			else if (thread->event_info & RT_EVENT_FLAG_OR)
			{
				if (thread->event_set & event->set)
				{
					status = RT_EOK;
				}
			}

			/* move node to the nexe */
			n = n->next;

			/* condition is satisfied, resume thread */
			if (status == RT_EOK)
			{
				/* resume thread, and thread list breaks out */
				rt_thread_resume(thread);

				/* decrease suspended thread count */
				event->parent.suspend_thread_count--;

				if (thread->event_info & RT_EVENT_FLAG_CLEAR)
					event->set &= ~thread->event_set;
			}
		}
	}

	/* enable interrupt */
	rt_hw_interrupt_enable(level);

	/* do a schedule */
	rt_schedule();

	return RT_EOK;
}

/**
 * This function will receive an event from event object, if the event is unavailable,
 * the thread shall wait for a specified time.
 *
 * @param event the fast event object
 * @param set the interested event set
 * @param option the receive option
 * @param timeout the waiting time
 * @param recved the received event
 *
 * @return the error code
 */
rt_err_t rt_event_recv(rt_event_t event, rt_uint32_t set, rt_uint8_t option, rt_int32_t timeout, rt_uint32_t* recved)
{
	struct rt_thread *thread;
	register rt_ubase_t level;
	register rt_base_t status;

	/* parameter check */
	RT_ASSERT(event != RT_NULL);
	if (set == 0) return -RT_ERROR;

	/* init status */
	status = -RT_ERROR;

#ifdef RT_USING_HOOK
	if (rt_object_trytake_hook != RT_NULL) rt_object_trytake_hook(&(event->parent.parent));
#endif

	/* disable interrupt */
	level = rt_hw_interrupt_disable();

	/* check event set */
	if (option & RT_EVENT_FLAG_AND)
	{
		if ((event->set & set) == set) status = RT_EOK;
	}
	else if (option & RT_EVENT_FLAG_OR)
	{
		if (event->set & set) status = RT_EOK;
	}

	/* get current thread */
	thread = rt_thread_self();
	/* reset thread error */
	thread->error = RT_EOK;

	if (status == RT_EOK)
	{
		/* set received event */
		*recved = event->set;

		/* received event */
		if (option & RT_EVENT_FLAG_CLEAR) event->set &= ~set;
	}
	else if (timeout == 0)
	{
		/* no waiting */
		thread->error = -RT_ETIMEOUT;
	}
	else
	{
		/* fill thread event info */
		thread->event_set  = set;
		thread->event_info = option;

		/* put thread to suspended thread list */
		rt_ipc_object_suspend(&(event->parent), thread);

		/* if there is a waiting timeout, active thread timer */
		if (timeout > 0)
		{
			/* reset the timeout of thread timer and start it */
			rt_timer_control(&(thread->thread_timer), RT_TIMER_CTRL_SET_TIME, &timeout);
			rt_timer_start(&(thread->thread_timer));
		}

		/* enable interrupt */
		rt_hw_interrupt_enable(level);

		/* do a schedule */
		rt_schedule();

		if (thread->error != RT_EOK)
		{
			/* decrease suspended thread count */
			rt_ipc_object_decrease(&(event->parent));
			return thread->error;
		}

		/* disable interrupt */
		level = rt_hw_interrupt_disable();

		/* get received event */
		*recved = event->set;
	}

	/* enable interrupt */
	rt_hw_interrupt_enable(level);

#ifdef RT_USING_HOOK
	if (rt_object_take_hook != RT_NULL) rt_object_take_hook(&(event->parent.parent));
#endif

	return thread->error;
}

/**
 * This function can get or set some extra attributions of an event object.
 *
 * @param event the event object
 * @param cmd the execution command
 * @param arg the execution argument
 *
 * @return the error code
 */
rt_err_t rt_event_control (rt_event_t event, rt_uint8_t cmd, void* arg)
{
	return RT_EOK;
}

#endif /* end of RT_USING_EVENT */

#ifdef RT_USING_MAILBOX

/**
 * This function will initialize a mailbox and put it under control of resource
 * management.
 *
 * @param mb the mailbox object
 * @param name the name of mailbox
 * @param msgpool the begin address of buffer to save received mail
 * @param size the size of mailbox
 * @param flag the flag of mailbox
 *
 * @return the operation status, RT_EOK on successful
 */
rt_err_t rt_mb_init(rt_mailbox_t mb, const char* name, void* msgpool, rt_size_t size, rt_uint8_t flag)
{
	RT_ASSERT(mb != RT_NULL);

	/* init object */
	rt_object_init(&(mb->parent.parent), RT_Object_Class_MailBox, name);

	/* set parent flag */
	mb->parent.parent.flag = flag;

	/* init ipc object */
	rt_ipc_object_init(&(mb->parent));

	/* init mailbox */
	mb->msg_pool = msgpool;
	mb->size 	 = size;
	mb->entry 	 	= 0;
	mb->in_offset 	= 0;
	mb->out_offset 	= 0;

	return RT_EOK;
}

/**
 * This function will detach a mailbox from resource management
 *
 * @param mb the mailbox object
 *
 * @return the operation status, RT_EOK on successful
 */
rt_err_t rt_mb_detach(rt_mailbox_t mb)
{
	/* parameter check */
	RT_ASSERT(mb != RT_NULL);

	/* resume all suspended thread */
	rt_ipc_object_resume_all(&(mb->parent));

	/* detach mailbox object */
	rt_object_detach(&(mb->parent.parent));

	return RT_EOK;
}

#ifdef RT_USING_HEAP
/**
 * This function will create a mailbox object from system resource
 *
 * @param name the name of mailbox
 * @param size the size of mailbox
 * @param flag the flag of mailbox
 *
 * @return the created mailbox, RT_NULL on error happen
 */
rt_mailbox_t rt_mb_create (const char* name, rt_size_t size, rt_uint8_t flag)
{
	rt_mailbox_t mb;

	/* allocate object */
	mb = (rt_mailbox_t) rt_object_allocate(RT_Object_Class_MailBox, name);
	if (mb == RT_NULL) return mb;

	/* set parent */
	mb->parent.parent.flag = flag;

	/* init ipc object */
	rt_ipc_object_init(&(mb->parent));

	/* init mailbox */
	mb->size 	 	= size;
	mb->msg_pool 	= rt_malloc(mb->size * sizeof(rt_uint32_t));
	if (mb->msg_pool == RT_NULL)
	{
		/* delete mailbox object */
		rt_object_delete(&(mb->parent.parent));

		return RT_NULL;
	}
	mb->entry  = 0;
	mb->in_offset 	= 0;
	mb->out_offset 	= 0;

	return mb;
}

/**
 * This function will delete a mailbox object and release the memory
 *
 * @param mb the mailbox object
 *
 * @return the error code
 */
rt_err_t rt_mb_delete (rt_mailbox_t mb)
{
	/* parameter check */
	RT_ASSERT(mb != RT_NULL);

	/* resume all suspended thread */
	rt_ipc_object_resume_all(&(mb->parent));

	/* free mailbox pool */
	rt_free(mb->msg_pool);

	/* delete mailbox object */
	rt_object_delete(&(mb->parent.parent));

	return RT_EOK;
}
#endif

/**
 * This function will send a mail to mailbox object, if there are threads suspended
 * on mailbox object, it will be waked up.
 *
 * @param mb the mailbox object
 * @param value the mail
 *
 * @return the error code
 */
rt_err_t rt_mb_send (rt_mailbox_t mb, rt_uint32_t value)
{
	register rt_ubase_t temp;

	/* parameter check */
	RT_ASSERT(mb != RT_NULL);

#ifdef RT_USING_HOOK
	if (rt_object_put_hook != RT_NULL) rt_object_put_hook(&(mb->parent.parent));
#endif

	/* disable interrupt */
	temp = rt_hw_interrupt_disable();

	/* mailbox is full */
	if (mb->entry == mb->size)
	{
		/* enable interrupt */
		rt_hw_interrupt_enable(temp);

		return -RT_EFULL;
	}

	/* set ptr */
	mb->msg_pool[mb->in_offset] = value;
	/* increase input offset */
	++ mb->in_offset;
	mb->in_offset %= mb->size;
	/* increase message entry */
	mb->entry ++;

	/* resume suspended thread */
	if (mb->parent.suspend_thread_count > 0)
	{
		rt_ipc_object_resume(&(mb->parent));

		/* enable interrupt */
		rt_hw_interrupt_enable(temp);

		rt_schedule();
		return RT_EOK;
	}

	/* enable interrupt */
	rt_hw_interrupt_enable(temp);

	return RT_EOK;
}

/**
 * This function will receive a mail from mailbox object, if there is no mail in
 * mailbox object, the thread shall wait for a specified time.
 *
 * @param mb the mailbox object
 * @param value the received mail will be saved in
 * @param timeout the waiting time
 *
 * @return the error code
 */
rt_err_t rt_mb_recv (rt_mailbox_t mb, rt_uint32_t* value, rt_int32_t timeout)
{
	struct rt_thread *thread;
	register rt_ubase_t temp;

	/* parameter check */
	RT_ASSERT(mb != RT_NULL);

#ifdef RT_USING_HOOK
	if (rt_object_trytake_hook != RT_NULL) rt_object_trytake_hook(&(mb->parent.parent));
#endif

	/* disable interrupt */
	temp = rt_hw_interrupt_disable();

	/* mailbox is empty */
	if (mb->entry == 0)
	{
		/* get current thread */
		thread = rt_thread_self();

		/* reset error number in thread */
		thread->error = RT_EOK;

		/* no waiting, return timeout */
		if (timeout == 0)
		{
			/* enable interrupt */
			rt_hw_interrupt_enable(temp);

			thread->error = -RT_ETIMEOUT;
			return -RT_ETIMEOUT;
		}

		/* suspend current thread */
		rt_ipc_object_suspend(&(mb->parent), thread);

		/* has waiting time, start thread timer */
		if (timeout > 0)
		{
#ifdef IPC_DEBUG
			rt_kprintf("set thread:%s to timer list\n", thread->name);
#endif
			/* reset the timeout of thread timer and start it */
			rt_timer_control(&(thread->thread_timer), RT_TIMER_CTRL_SET_TIME, &timeout);
			rt_timer_start(&(thread->thread_timer));
		}

		/* enable interrupt */
		rt_hw_interrupt_enable(temp);

		/* re-schedule */
		rt_schedule();

		/* recv message */
		if (thread->error != RT_EOK)
		{
			/* decrease suspended thread count */
			rt_ipc_object_decrease(&(mb->parent));
			return thread->error;
		}

		/* disable interrupt */
		temp = rt_hw_interrupt_disable();
	}

	/* fill ptr */
	*value = mb->msg_pool[mb->out_offset];

	/* increase output offset */
	++mb->out_offset;
	mb->out_offset %= mb->size;
	/* decrease message entry */
	mb->entry --;

	/* enable interrupt */
	rt_hw_interrupt_enable(temp);

#ifdef RT_USING_HOOK
	if (rt_object_take_hook != RT_NULL) rt_object_take_hook(&(mb->parent.parent));
#endif

	return RT_EOK;
}

/**
 * This function can get or set some extra attributions of a mailbox object.
 *
 * @param mb the mailbox object
 * @param cmd the execution command
 * @param arg the execution argument
 *
 * @return the error code
 */
rt_err_t rt_mb_control(rt_mailbox_t mb, rt_uint8_t cmd, void* arg)
{
	return RT_EOK;
}

#endif /* end of RT_USING_MAILBOX */

#ifdef RT_USING_MESSAGEQUEUE

struct rt_mq_message
{
	struct rt_mq_message* next;
};

/**
 * This function will initialize a message queue and put it under control of resource
 * management.
 *
 * @param mq the message object
 * @param name the name of message queue
 * @param msgpool the beginning address of buffer to save messages
 * @param msg_size the maximum size of message
 * @param pool_size the size of buffer to save messages
 * @param flag the flag of message queue
 *
 * @return the operation status, RT_EOK on successful
 */
rt_err_t rt_mq_init(rt_mq_t mq, const char* name, void *msgpool, rt_size_t msg_size, rt_size_t pool_size, rt_uint8_t flag)
{
	struct rt_mq_message* head;
	register rt_base_t temp;

	/* parameter check */
	RT_ASSERT(mq != RT_NULL);

	/* init object */
	rt_object_init(&(mq->parent.parent), RT_Object_Class_MessageQueue, name);

	/* set parent flag */
	mq->parent.parent.flag = flag;

	/* init ipc object */
	rt_ipc_object_init(&(mq->parent));

	/* set messasge pool */
	mq->msg_pool 	= msgpool;

	/* get correct message size */
	mq->msg_size	= RT_ALIGN(msg_size,  RT_ALIGN_SIZE);
	mq->max_msgs	= pool_size / (mq->msg_size + sizeof(struct rt_mq_message));

	/* init message list */
	mq->msg_queue_head = RT_NULL;
	mq->msg_queue_tail = RT_NULL;

	/* init message empty list */
	mq->msg_queue_free = RT_NULL;
	for (temp = 0; temp < mq->max_msgs; temp ++)
	{
		head = (struct rt_mq_message*)((rt_uint8_t*)mq->msg_pool +
			temp * (mq->msg_size + sizeof(struct rt_mq_message)));
		head->next = mq->msg_queue_free;
		mq->msg_queue_free = head;
	}

	/* the initial entry is zero */
	mq->entry		= 0;

	return RT_EOK;
}

/**
 * This function will detach a message queue object from resource management
 *
 * @param mq the message queue object
 *
 * @return the operation status, RT_EOK on successful
 */
rt_err_t rt_mq_detach(rt_mq_t mq)
{
	/* parameter check */
	RT_ASSERT(mq != RT_NULL);

	/* resume all suspended thread */
	rt_ipc_object_resume_all((struct rt_ipc_object*)mq);

	/* detach message queue object */
	rt_object_detach(&(mq->parent.parent));

	return RT_EOK;
}

#ifdef RT_USING_HEAP
/**
 * This function will create a message queue object from system resource
 *
 * @param name the name of message queue
 * @param msg_size the size of message
 * @param max_msgs the maximum number of message in queue
 * @param flag the flag of message queue
 *
 * @return the created message queue, RT_NULL on error happen
 */
rt_mq_t rt_mq_create (const char* name, rt_size_t msg_size, rt_size_t max_msgs, rt_uint8_t flag)
{
	struct rt_messagequeue* mq;
	struct rt_mq_message* head;
	register rt_base_t temp;

	/* allocate object */
	mq = (rt_mq_t) rt_object_allocate(RT_Object_Class_MessageQueue, name);
	if (mq == RT_NULL) return mq;

	/* set parent */
	mq->parent.parent.flag = flag;

	/* init ipc object */
	rt_ipc_object_init(&(mq->parent));

	/* init message queue */

	/* get correct message size */
	mq->msg_size	= RT_ALIGN(msg_size, RT_ALIGN_SIZE);
	mq->max_msgs	= max_msgs;

	/* allocate message pool */
	mq->msg_pool 	= rt_malloc((mq->msg_size + sizeof(struct rt_mq_message))* mq->max_msgs);
	if (mq->msg_pool == RT_NULL)
	{
		rt_mq_delete(mq);
		return RT_NULL;
	}

	/* init message list */
	mq->msg_queue_head = RT_NULL;
	mq->msg_queue_tail = RT_NULL;

	/* init message empty list */
	mq->msg_queue_free = RT_NULL;
	for (temp = 0; temp < mq->max_msgs; temp ++)
	{
		head = (struct rt_mq_message*)((rt_uint8_t*)mq->msg_pool +
			temp * (mq->msg_size + sizeof(struct rt_mq_message)));
		head->next = mq->msg_queue_free;
		mq->msg_queue_free = head;
	}

	/* the initial entry is zero */
	mq->entry		= 0;

	return mq;
}

/**
 * This function will delete a message queue object and release the memory
 *
 * @param mq the message queue object
 *
 * @return the error code
 */
rt_err_t rt_mq_delete (rt_mq_t mq)
{
	/* parameter check */
	RT_ASSERT(mq != RT_NULL);

	/* resume all suspended thread */
	rt_ipc_object_resume_all(&(mq->parent));

	/* free mailbox pool */
	rt_free(mq->msg_pool);

	/* delete mailbox object */
	rt_object_delete(&(mq->parent.parent));

	return RT_EOK;
}
#endif

/**
 * This function will send a message to message queue object, if there are threads
 * suspended on message queue object, it will be waked up.
 *
 * @param mq the message queue object
 * @param buffer the message
 * @param size the size of buffer
 *
 * @return the error code
 */
rt_err_t rt_mq_send (rt_mq_t mq, void* buffer, rt_size_t size)
{
	register rt_ubase_t temp;
	struct rt_mq_message *msg;

	/* greater than one message size */
	if (size > mq->msg_size) return -RT_ERROR;

#ifdef RT_USING_HOOK
	if (rt_object_put_hook != RT_NULL) rt_object_put_hook(&(mq->parent.parent));
#endif

	/* disable interrupt */
	temp = rt_hw_interrupt_disable();

	/* get a free list, there must be an empty item */
	msg = (struct rt_mq_message*)mq->msg_queue_free;

	/* message queue is full */
	if (msg == RT_NULL)
	{
		/* enable interrupt */
		rt_hw_interrupt_enable(temp);

		return -RT_EFULL;
	}

	/* move free list pointer */
	mq->msg_queue_free = msg->next;

	/* copy buffer */
	rt_memcpy(msg + 1, buffer, size);

	/* link msg to message queue */
	if (mq->msg_queue_tail != RT_NULL)
	{
		/* if the tail exists, */
		((struct rt_mq_message*)mq->msg_queue_tail)->next = msg;
	}
	/* the msg is the new tail of list, the next shall be NULL */
	msg->next = RT_NULL;

	/* set new tail */
	mq->msg_queue_tail = msg;

	/* if the head is empty, set head */
	if (mq->msg_queue_head == RT_NULL)mq->msg_queue_head = msg;

	/* increase message entry */
	mq->entry ++;

	/* resume suspended thread */
	if (mq->parent.suspend_thread_count > 0)
	{
		rt_ipc_object_resume(&(mq->parent));

		/* enable interrupt */
		rt_hw_interrupt_enable(temp);

		rt_schedule();
		return RT_EOK;
	}

	/* enable interrupt */
	rt_hw_interrupt_enable(temp);

	return RT_EOK;
}

/**
 * This function will send urgently a message to message queue object, which means
 * the message will be inserted to the head of message queue. If there are threads
 * suspended on message queue object, it will be waked up.
 *
 * @param mq the message queue object
 * @param buffer the message
 * @param size the size of buffer
 *
 * @return the error code
 */
rt_err_t rt_mq_urgent(rt_mq_t mq, void* buffer, rt_size_t size)
{
	register rt_ubase_t temp;
	struct rt_mq_message *msg;

	/* greater than one message size */
	if (size > mq->msg_size) return -RT_ERROR;

#ifdef RT_USING_HOOK
	if (rt_object_put_hook != RT_NULL) rt_object_put_hook(&(mq->parent.parent));
#endif

	/* disable interrupt */
	temp = rt_hw_interrupt_disable();

	/* get a free list, there must be an empty item */
	msg = (struct rt_mq_message*)mq->msg_queue_free;

	/* message queue is full */
	if (msg == RT_NULL)
	{
		/* enable interrupt */
		rt_hw_interrupt_enable(temp);

		return -RT_EFULL;
	}

	/* move free list pointer */
	mq->msg_queue_free = msg->next;

	/* copy buffer */
	rt_memcpy(msg + 1, buffer, size);

	/* link msg to the beginning of message queue */
	msg->next = mq->msg_queue_head;
	mq->msg_queue_head = msg;

	/* if there is no tail */
	if (mq->msg_queue_tail == RT_NULL) mq->msg_queue_tail = msg;

	/* increase message entry */
	mq->entry ++;

	/* resume suspended thread */
	if (mq->parent.suspend_thread_count > 0)
	{
		rt_ipc_object_resume(&(mq->parent));

		/* enable interrupt */
		rt_hw_interrupt_enable(temp);

		rt_schedule();

		return RT_EOK;
	}

	/* enable interrupt */
	rt_hw_interrupt_enable(temp);

	return RT_EOK;
}

/**
 * This function will receive a message from message queue object, if there is no
 * message in message queue object, the thread shall wait for a specified time.
 *
 * @param mq the message queue object
 * @param buffer the received message will be saved in
 * @param size the size of buffer
 * @param timeout the waiting time
 *
 * @return the error code
 */
rt_err_t rt_mq_recv (rt_mq_t mq, void* buffer, rt_size_t size, rt_int32_t timeout)
{
	struct rt_thread *thread;
	register rt_ubase_t temp;
	struct rt_mq_message *msg;

#ifdef RT_USING_HOOK
	if (rt_object_trytake_hook != RT_NULL) rt_object_trytake_hook(&(mq->parent.parent));
#endif

	/* disable interrupt */
	temp = rt_hw_interrupt_disable();

	/* mailbox is empty */
	if (mq->entry == 0)
	{
		/* get current thread */
		thread = rt_thread_self();

		/* reset error number in thread */
		thread->error = RT_EOK;

		/* no waiting, return timeout */
		if (timeout == 0)
		{
			/* enable interrupt */
			rt_hw_interrupt_enable(temp);

			thread->error = -RT_ETIMEOUT;
			return -RT_ETIMEOUT;
		}

		/* suspend current thread */
		rt_ipc_object_suspend(&(mq->parent), thread);

		/* has waiting time, start thread timer */
		if (timeout > 0)
		{
#ifdef IPC_DEBUG
			rt_kprintf("set thread:%s to timer list\n", thread->name);
#endif
			/* reset the timeout of thread timer and start it */
			rt_timer_control(&(thread->thread_timer), RT_TIMER_CTRL_SET_TIME, &timeout);
			rt_timer_start(&(thread->thread_timer));
		}

		/* enable interrupt */
		rt_hw_interrupt_enable(temp);

		/* re-schedule */
		rt_schedule();

		/* recv message */
		if (thread->error != RT_EOK)
		{
			/* decrease suspended thread count */
			rt_ipc_object_decrease(&(mq->parent));
			return thread->error;
		}

		/* disable interrupt */
		temp = rt_hw_interrupt_disable();
	}

	/* get message from queue */
	msg = (struct rt_mq_message*) mq->msg_queue_head;

	/* move message queue head */
	mq->msg_queue_head = msg->next;

	/* reach queue tail, set to NULL */
	if (mq->msg_queue_tail == msg) mq->msg_queue_tail = RT_NULL;

	/* copy message */
	rt_memcpy(buffer, msg + 1,
		size > mq->msg_size? mq->msg_size : size);

	/* put message to free list */
	msg->next = (struct rt_mq_message*)mq->msg_queue_free;
	mq->msg_queue_free = msg;

	/* decrease message entry */
	mq->entry --;

	/* enable interrupt */
	rt_hw_interrupt_enable(temp);

#ifdef RT_USING_HOOK
	if (rt_object_take_hook != RT_NULL) rt_object_take_hook(&(mq->parent.parent));
#endif

	return RT_EOK;
}

/**
 * This function can get or set some extra attributions of a message queue object.
 *
 * @param mq the message queue object
 * @param cmd the execution command
 * @param arg the execution argument
 *
 * @return the error code
 */
rt_err_t rt_mq_control(rt_mq_t mq, rt_uint8_t cmd, void* arg)
{
	return RT_EOK;
}

#endif /* end of RT_USING_MESSAGEQUEUE */

/**
 * @ingroup SystemInit
 * This function will init IPC module.
 */
void rt_system_ipc_init()
{
	/* nothing to be done */
}

/*@}*/
