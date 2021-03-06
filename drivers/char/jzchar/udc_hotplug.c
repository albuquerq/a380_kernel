/*
 * linux/drivers/char/jzchar/udc_hotplug.c
 *
 * New UDC hotplug driver.
 *
 * Copyright (C) 2007 Ingenic Semiconductor Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */
#include <linux/module.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/kthread.h>
#include <linux/socket.h>
#include <linux/skbuff.h>
#include <linux/string.h>
#include <linux/kobject.h>
#include <linux/miscdevice.h>
#include <linux/proc_fs.h>

#include <asm/jzsoc.h>
#include "jzchars.h"

#ifndef GPIO_UDC_HOTPLUG
#define GPIO_UDC_HOTPLUG 86
#endif

#define UDC_HOTPLUG_PIN   GPIO_UDC_HOTPLUG
#define UDC_HOTPLUG_IRQ   (IRQ_GPIO_0 + UDC_HOTPLUG_PIN)

//#define dprintk(x,...) 
#define dprintk printk

//simple meaning define
#define NOT_CONNECT 0
#define YES_CONNECT 1
#define MAX_GPIO_TIME 50

#define EVENT_USB_ADD              1
#define EVENT_USB_REMOVE           2
#define EVENT_POWER_ADD            3
#define EVENT_POWER_REMOVE         4
#define EVENT_POWER_TO_USB         5
#define EVENT_USB_SUSPEND_POWER    6
//alterac add
#define GPIO_CHANGE_PIN_AL (32*3+14)

struct udc_pnp_stat
{
	char cable_stat, old_cable_stat;
	char protl_stat, old_protl_stat;
	char object_stat1;
	char object_stat2;
};

static struct udc_pnp_stat cur_pnp_stat;

static struct file_operations cable_fops = {
	owner:		THIS_MODULE,
};

static struct miscdevice cable_dev=
{
	231,
	"udc_cable",
	&cable_fops
};

static struct file_operations power_fops = {
	owner:		THIS_MODULE,
};

static struct miscdevice power_dev=
{
	232,
	"power_cable",
	&power_fops
};

int jz_udc_active = 0; /* 0: Have no actions; 1: Have actions */

static int udc_pin_level;
static int udc_old_state;
static int udc_pin_time;
static int change_state;


static struct timer_list udc_long_timer, udc_gpio_timer;

/* Kernel thread to deliver event to user space */
static struct task_struct *kudcd_task;

static void udc_gpio_timer_routine(unsigned long data)
{
	wake_up_process(kudcd_task);
}

#define UDC_GET_STAT_TIMER_COUNTER 3
static void udc_long_timer_routine(unsigned long data)
{
	//dprintk("udc_timer\n");
        //dprintk("%s %d jz_udc_active is %d\n",__FILE__,__LINE__,jz_udc_active);
//static int stable_count = 0;
//static int suspend_count = 0;
	if (jz_udc_active)
		udc_old_state = 1;
	if (!jz_udc_active && udc_old_state)      //udc irq timeout! do suspend
        {
          //suspend_count++;
          //stable_count++;
          //if(stable_count > 2)
          {
            dprintk("udc suspend!\n");
            udc_old_state = 0;
            cur_pnp_stat.protl_stat = NOT_CONNECT;		
            del_timer(&udc_long_timer);
            wake_up_process(kudcd_task);
            return;
          }
	}
        //else
          //stable_count = 0;
	jz_udc_active = 0;
        //dprintk("%s %d jz_udc_active is %d stable_count is %d suspend_count is %d \n",__FILE__,__LINE__,jz_udc_active,stable_count,suspend_count);
	udc_long_timer.expires = jiffies + UDC_GET_STAT_TIMER_COUNTER * HZ; /* about 3 s */
	add_timer(&udc_long_timer);
}
static int udc_get_pnp_stat(void)
{
	udc_pin_level = __gpio_get_pin(UDC_HOTPLUG_PIN);
	udc_pin_time = 1;

	init_timer(&udc_gpio_timer);
	del_timer(&udc_gpio_timer);
	udc_gpio_timer.function = udc_gpio_timer_routine;
	udc_gpio_timer.expires = jiffies + 1;  /* about 10 ms */
	add_timer(&udc_gpio_timer);

	while(1)
	{
		set_current_state(TASK_INTERRUPTIBLE);
		schedule();

		if (__gpio_get_pin(UDC_HOTPLUG_PIN) != udc_pin_level)
		{
			udc_pin_level = __gpio_get_pin(UDC_HOTPLUG_PIN);
			udc_pin_time = 1;
			dprintk("udc gpio detect restart! \n");
		}

		udc_pin_time ++;
		if (udc_pin_time > MAX_GPIO_TIME)
			break;

		del_timer(&udc_gpio_timer);
		udc_gpio_timer.function = udc_gpio_timer_routine;
		udc_gpio_timer.expires = jiffies + 1;  /* about 10 ms */
		add_timer(&udc_gpio_timer);
	}

	del_timer(&udc_gpio_timer);
	if (__gpio_get_pin(UDC_HOTPLUG_PIN))
		return YES_CONNECT;
	else
		return NOT_CONNECT;
}

static void udc_get_cable(void)
{
	u32 intr_usb;

	__intc_mask_irq(IRQ_UDC);

	/* Now enable PHY to start detect */
#ifdef CONFIG_SOC_JZ4740
	REG_CPM_SCR |= CPM_SCR_USBPHY_ENABLE;
#elif defined(CONFIG_SOC_JZ4750) || defined(CONFIG_SOC_JZ4750D)
	REG_CPM_OPCR |= CPM_OPCR_UDCPHY_ENABLE;
#endif
	/* Clear IRQs */
	REG16(USB_REG_INTRINE) = 0;
	REG16(USB_REG_INTROUTE) = 0;
	REG8(USB_REG_INTRUSBE) = 0;

	/* disable UDC IRQs first */
	REG16(USB_REG_INTRINE) = 0;
	REG16(USB_REG_INTROUTE) = 0;
	REG8(USB_REG_INTRUSBE) = 0;

	/* Disable DMA */
	REG32(USB_REG_CNTL1) = 0;
	REG32(USB_REG_CNTL2) = 0;

	/* Enable HS Mode */
	REG8(USB_REG_POWER) |= USB_POWER_HSENAB;
	/* Enable soft connect */
	REG8(USB_REG_POWER) |= USB_POWER_SOFTCONN;

	dprintk("enable phy! %x %x %x %x %x\n",
	       REG8(USB_REG_POWER),
	       REG_CPM_OPCR,
	       REG16(USB_REG_INTRINE),
	       REG16(USB_REG_INTROUTE),
	       REG8(USB_REG_INTRUSBE));

	init_timer(&udc_gpio_timer);
	del_timer(&udc_gpio_timer);
	udc_gpio_timer.function = udc_gpio_timer_routine;
	udc_gpio_timer.expires = jiffies + 11;  /* about 100 ms */
	add_timer(&udc_gpio_timer);
	/* Sleep a short time to see result */
	set_current_state(TASK_INTERRUPTIBLE);
	schedule();

	del_timer(&udc_gpio_timer);
	intr_usb = REG8(USB_REG_INTRUSB);
	if ((intr_usb & USB_INTR_RESET) ||
	    (intr_usb & USB_INTR_RESUME) ||
	    (intr_usb & USB_INTR_SUSPEND))
	{
		cur_pnp_stat.protl_stat = YES_CONNECT;
		dprintk("cable is usb! \n");
	}
	else
	{
		cur_pnp_stat.protl_stat = NOT_CONNECT;
		dprintk("%s %d cable is power! \n",__FILE__,__LINE__);
	}

	/* Detect finish ,clean every thing */
	/* Disconnect from usb */
	REG8(USB_REG_POWER) &= ~USB_POWER_SOFTCONN;
	/* Disable the USB PHY */
#ifdef CONFIG_SOC_JZ4740
	REG_CPM_SCR &= ~CPM_SCR_USBPHY_ENABLE;
#elif defined(CONFIG_SOC_JZ4750) || defined(CONFIG_SOC_JZ4750D)
	REG_CPM_OPCR &= ~CPM_OPCR_UDCPHY_ENABLE;
#endif
	/* Clear IRQs */
	REG16(USB_REG_INTRINE) = 0;
	REG16(USB_REG_INTROUTE) = 0;
	REG8(USB_REG_INTRUSBE) = 0;
	__intc_ack_irq(IRQ_UDC);
	__intc_unmask_irq(IRQ_UDC);
}
static void control_change_gpio(int flag)
{
	__gpio_as_func0(GPIO_CHANGE_PIN_AL);
	__gpio_enable_pull(GPIO_CHANGE_PIN_AL);
	__gpio_as_output(GPIO_CHANGE_PIN_AL);
	if (flag == 0){
		__gpio_clear_pin(GPIO_CHANGE_PIN_AL);
	}else if (flag == 1){
		__gpio_set_pin(GPIO_CHANGE_PIN_AL);
	}
}
static void send_event_udev(int event)
{
  change_state = event;

	dprintk("Send udev message: cable=%d old=%d protl=%d old=%d \n",
	       cur_pnp_stat.cable_stat,
	       cur_pnp_stat.old_cable_stat,
	       cur_pnp_stat.protl_stat,
	       cur_pnp_stat.old_protl_stat);

	switch (event)
	{
	case EVENT_USB_ADD:
	control_change_gpio(1);
	
		printk("usb cable insert! \n");
		misc_register(&cable_dev);
		kobject_uevent(&cable_dev.this_device->kobj, KOBJ_ADD);
		init_timer(&udc_long_timer);
		del_timer(&udc_long_timer);
		udc_long_timer.function = udc_long_timer_routine;
		udc_long_timer.expires = jiffies + UDC_GET_STAT_TIMER_COUNTER * HZ; /* about 3 s */
		add_timer(&udc_long_timer);
		break;
        case EVENT_USB_REMOVE:
                control_change_gpio(1);
		
		printk("usb cable remove! \n");
		kobject_uevent(&cable_dev.this_device->kobj, KOBJ_REMOVE);
		misc_deregister(&cable_dev);
		del_timer(&udc_long_timer);
		break;
	case EVENT_POWER_ADD:
		printk("power cable insert! \n");
		misc_register(&power_dev);
		kobject_uevent(&power_dev.this_device->kobj, KOBJ_ADD);
		break;
	case EVENT_POWER_REMOVE:
		control_change_gpio(1);
		printk("power cable remove! \n");
		kobject_uevent(&power_dev.this_device->kobj, KOBJ_REMOVE);
		misc_deregister(&power_dev);
		break;
	case EVENT_POWER_TO_USB:
                control_change_gpio(1);
		printk("change power cable to usb! \n");
		kobject_uevent(&power_dev.this_device->kobj, KOBJ_REMOVE);
		misc_deregister(&power_dev);
		misc_register(&cable_dev);
		kobject_uevent(&cable_dev.this_device->kobj, KOBJ_ADD);
		break;
        case EVENT_USB_SUSPEND_POWER:
                control_change_gpio(1);
		printk("usb cable suspend! \n");
		printk("as power cable insert! \n");
		kobject_uevent(&cable_dev.this_device->kobj, KOBJ_REMOVE);
		misc_deregister(&cable_dev);
		misc_register(&power_dev);
		kobject_uevent(&power_dev.this_device->kobj, KOBJ_ADD);
		break;
	};
}

static void udc_pnp_detect(void)
{
	int i;
	int usb_flag;


	if (cur_pnp_stat.cable_stat == YES_CONNECT)       /* already connected! */
	{
		if (udc_get_pnp_stat() == NOT_CONNECT)
		{
			dprintk("cable real out! \n");
			cur_pnp_stat.cable_stat = NOT_CONNECT;
			cur_pnp_stat.protl_stat = NOT_CONNECT;
			/* Deliver this event to user space in udev model */
			if (cur_pnp_stat.old_protl_stat)
			{
				send_event_udev(EVENT_USB_REMOVE);
				//KJH ADD
                                jz_udc_active = 0; /* 0: Have no actions; 1: Have actions */
                                printk("%s %d jz_udc_active is %d\n",__FILE__,__LINE__,jz_udc_active);
                        }
			else
				send_event_udev(EVENT_POWER_REMOVE);
			cur_pnp_stat.old_cable_stat = cur_pnp_stat.cable_stat;
			cur_pnp_stat.old_protl_stat = cur_pnp_stat.protl_stat;
		}
		else
		{
			if (cur_pnp_stat.old_protl_stat != cur_pnp_stat.protl_stat)
			{
                                printk("%s %d jz_udc_active is %d\n",__FILE__,__LINE__,jz_udc_active);
                                //while(1);
				send_event_udev(EVENT_USB_SUSPEND_POWER);
				cur_pnp_stat.old_cable_stat = cur_pnp_stat.cable_stat;
				cur_pnp_stat.old_protl_stat = cur_pnp_stat.protl_stat;
			}
			else              //change power to cable
			{
#if 0     //not support yet!
				udc_get_cable();
				if (cur_pnp_stat.old_protl_stat != cur_pnp_stat.protl_stat)
					send_event_udev(EVENT_POWER_TO_USB);
				cur_pnp_stat.old_cable_stat = cur_pnp_stat.cable_stat;
				cur_pnp_stat.old_protl_stat = cur_pnp_stat.protl_stat;
#endif
			}
		}
	}
	else
	{
		if (udc_get_pnp_stat() == YES_CONNECT)
		{
			dprintk("cable real in! \n");
			cur_pnp_stat.cable_stat = YES_CONNECT;
			//udc_get_cable();
			//KJH ADD
			for (i=0; i<3; i++)
			{
				udc_get_cable();
				if (cur_pnp_stat.protl_stat == NOT_CONNECT)
				{
					continue;
				}
				else
				{
					usb_flag++;
					continue;
				}
			}


			/* Deliver this event to user space in udev model */
			if (cur_pnp_stat.protl_stat && usb_flag >=2)
				send_event_udev(EVENT_USB_ADD);
			else				
				send_event_udev(EVENT_POWER_ADD);
			cur_pnp_stat.old_cable_stat = cur_pnp_stat.cable_stat;
			cur_pnp_stat.old_protl_stat = cur_pnp_stat.protl_stat;
		}
		else
			dprintk("cable false in! \n");

	}
}

void udc_pnp_set_gpio(void)
{
	if (cur_pnp_stat.cable_stat == YES_CONNECT)
		__gpio_as_irq_fall_edge(UDC_HOTPLUG_PIN);
	else
		__gpio_as_irq_rise_edge(UDC_HOTPLUG_PIN);

	/* clear interrupt pending status */
	__gpio_ack_irq(UDC_HOTPLUG_PIN); 
	/* unmask interrupt */
	__gpio_unmask_irq(UDC_HOTPLUG_PIN); 
}
EXPORT_SYMBOL(udc_pnp_set_gpio);

//maddrone
static int udc_thread_run_flag = 0;
static int udc_pnp_thread(void *unused)
{
	//printk(KERN_NOTICE "UDC starting pnp monitor thread\n");
	printk("UDC starting pnp monitor thread\n");
	while(1)
	{
		set_current_state(TASK_INTERRUPTIBLE);
		udc_thread_run_flag = 1;
		schedule();

		dprintk("pnp thread wake up! \n");
		/* wake up here */
		udc_pnp_detect();
		/* Reset gpio state last */
		udc_pnp_set_gpio();
	}
}

static irqreturn_t udc_pnp_irq(int irq, void *dev_id)
{
	printk("udc_pnp_irq----\n");
    /* clear interrupt pending status */
    __gpio_ack_irq(UDC_HOTPLUG_PIN); 
	/* mask interrupt */
    __gpio_mask_irq(UDC_HOTPLUG_PIN); 
	/* wake up pnp detect thread */
	wake_up_process(kudcd_task);

	return IRQ_HANDLED;
}
static int proc_change_read_proc(
			char *page, char **start, off_t off,
			int count, int *eof, void *data)
{
	int value;
	if (change_state == EVENT_POWER_ADD){
		value = 1;
		sprintf(page,"%d\n",value);
	}else{
		value = 0;
		sprintf(page,"%d\n",value);
	}
}
static int proc_change_write_proc(
			struct file *file, const char *buffer,
			unsigned long count, void *data)
{
	int tmp_value =  simple_strtoul(buffer, 0, 10);
	if (change_state == EVENT_POWER_ADD){
		if (tmp_value == 1){
			control_change_gpio(0);
		}else if (tmp_value == 0){
			control_change_gpio(1);
		}else;
	}
	return count;
}

/*
 * Module init and exit
 */
static int __init udc_hotplug_init(void)
{
    int retval;
	struct proc_dir_entry *res;
	
	/* Init pnp stat first */
	cur_pnp_stat.cable_stat = NOT_CONNECT;
	cur_pnp_stat.protl_stat = NOT_CONNECT;
	cur_pnp_stat.old_cable_stat = NOT_CONNECT;
	cur_pnp_stat.old_protl_stat = NOT_CONNECT;
	cur_pnp_stat.object_stat1 = NOT_CONNECT;
	cur_pnp_stat.object_stat2 = NOT_CONNECT;
	udc_old_state = 0;
        change_state = 0;


	/* create pnp thread and register IRQ */
	kudcd_task = kthread_run(udc_pnp_thread, NULL, "kudcd");
	if (IS_ERR(kudcd_task)) {
		printk(KERN_ERR "jz_udc_hotplug: Failed to create system monitor thread.\n");
		return PTR_ERR(kudcd_task);
	}

        retval = request_irq(UDC_HOTPLUG_IRQ, udc_pnp_irq,
                             IRQF_DISABLED, "udc_pnp", NULL);
        if (retval) {
                printk("Could not get udc hotplug irq %d\n", UDC_HOTPLUG_IRQ);
		return retval;
        }
        //create change entry
        res = create_proc_entry("jz/change_power", 0, NULL);
        if(res)
        {
          res->owner = THIS_MODULE;
          res->read_proc = proc_change_read_proc;
          res->write_proc = proc_change_write_proc;
        }



        /* get current pin level */
	__gpio_disable_pull(UDC_HOTPLUG_PIN);
    __gpio_as_input(UDC_HOTPLUG_PIN);
	udelay(1);
    udc_pin_level = __gpio_get_pin(UDC_HOTPLUG_PIN);
	
	//wait until the kudc thread run
	while(udc_thread_run_flag != 1)
	{
		schedule_timeout(1);
	}

        if (udc_pin_level) {
		dprintk("Cable already in! \n");
		wake_up_process(kudcd_task);
        }
        else {
		__gpio_as_irq_rise_edge(UDC_HOTPLUG_PIN);
		dprintk("Cable not in! \n");
        }

	printk("JZ UDC hotplug driver registered\n");

	return 0;
}

static void __exit udc_hotplug_exit(void)
{
	free_irq(UDC_HOTPLUG_IRQ, NULL);
}

module_init(udc_hotplug_init);
module_exit(udc_hotplug_exit);

EXPORT_SYMBOL(jz_udc_active);

MODULE_AUTHOR("Lucifer <yliu@ingenic.cn>");
MODULE_DESCRIPTION("JzSOC OnChip udc hotplug driver");
MODULE_LICENSE("GPL");
