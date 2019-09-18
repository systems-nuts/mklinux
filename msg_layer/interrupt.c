/*******************************************************************************
 *                                                                             *
 * Copyright (C) 2003 - 2019                                                   *
 *         Dolphin Interconnect Solutions AS                                   *
 *                                                                             *
 *    All rights reserved                                                      *
 *                                                                             *
 *                                                                             *
 *******************************************************************************/ 

/* Linux kernel */
#include <linux/version.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/delay.h>
#include <linux/time.h>
/* Genif */
#include "genif.h"

/* Constants */
#define NO_FLAGS                     0
#define PRIORITY                     0
#define MODULE_ID                    0x4463
#define LINK_NO                      0
#define SERVER                       1
#define LOCAL_ADAPTER_NUMBER         0
#define MAX_NUM_CHANNELS 	1
#define SEND_OFFSET		1
#define RECV_OFFSET		(MAX_NUM_CHANNELS+SEND_OFFSET)
#if CONFIG_ARM64
#define TARGET_NODE		4
#else
#define TARGET_NODE		8
#endif
#define SEG_SIZE		2000
#define SERVER 1
#define CLIENT 0



/* Adapter number can be set via kernel module argument. */
uint32_t local_adapter_number = LOCAL_ADAPTER_NUMBER;
uint32_t remote_node_id;
uint32_t remote_interrupt_number;
int server = SERVER;

/* for dolphin PCIE interconnect */

sci_l_segment_handle_t local_send_seg_hdl[MAX_NUM_CHANNELS] = {NULL}, local_recv_seg_hdl[MAX_NUM_CHANNELS] = {NULL};

sci_r_segment_handle_t remote_send_seg_hdl[MAX_NUM_CHANNELS] = {NULL}, remote_recv_seg_hdl[MAX_NUM_CHANNELS] = {NULL};

vkaddr_t *send_vaddr[MAX_NUM_CHANNELS] = {NULL}, *recv_vaddr[MAX_NUM_CHANNELS] = {NULL}, *send_remote_vaddr[MAX_NUM_CHANNELS] = {NULL}, *recv_remote_vaddr[MAX_NUM_CHANNELS] = {NULL};

int local_send_intr_no[MAX_NUM_CHANNELS] = {0}, remote_send_intr_no[MAX_NUM_CHANNELS] = {0}, local_recv_intr_no[MAX_NUM_CHANNELS] = {0}, remote_recv_intr_no[MAX_NUM_CHANNELS] = {0};
volatile int send_connected_flag[MAX_NUM_CHANNELS] = {0}, recv_connected_flag[MAX_NUM_CHANNELS] = {0};
sci_map_handle_t send_map_handle[MAX_NUM_CHANNELS] = {NULL}, recv_map_handle[MAX_NUM_CHANNELS] = {NULL};
probe_status_t send_report, recv_report;

sci_l_interrupt_handle_t local_send_intr_hdl[MAX_NUM_CHANNELS] = {NULL}, local_recv_intr_hdl[MAX_NUM_CHANNELS] = {NULL};
sci_r_interrupt_handle_t remote_send_intr_hdl[MAX_NUM_CHANNELS] = {NULL}, remote_recv_intr_hdl[MAX_NUM_CHANNELS] = {NULL};
ktime_t start, end;
ktime_t tristart, triend;

sci_binding_t sci_resources_binding;
static sci_l_interrupt_handle_t local_interrupt_handle;

static int send_init(void);
static int recv_init(void);
static int send_cleanup(void);
static int recv_cleanup(void);


int local_cbfunc (void *arg, sci_l_segment_handle_t local_segment_handle,
                        unsigned32 reason, unsigned32 source_node,
                       unsigned32 local_adapter_number)
{
//      printk(" In %s: reason = %d for %p\n", __func__, reason, local_segment_handle);
        return 0;
}


int send_connect_cbfunc(void *arg,
                        sci_r_segment_handle_t remote_segment_handle,
                        unsigned32 reason,
                        unsigned32 status)
{

	if (status == 0) {
                
              if (remote_recv_seg_hdl[0] == remote_segment_handle) {
				printk("Reason = %d, status = %d \n", reason, status);
               send_connected_flag[0] = 1;
			}
            
	}

	return 0;
}

int recv_connect_cbfunc(void *arg,
                        sci_r_segment_handle_t remote_segment_handle,
                        unsigned32 reason,
                        unsigned32 status)
{
	int i = 0;

	if (status == 0) {
		
			if (remote_send_seg_hdl[0] == remote_segment_handle) {
				printk("Reason = %d, status = %d \n", reason, status);
				recv_connected_flag[0] = 1;
			}
		
	}

	return 0;
}


signed32 send_intr_cb (unsigned32 local_adapter_number,
                       void *arg, unsigned32 interrupt_number)
{
	int i =0;
    end = ktime_get();
	int average = (ktime_to_ns(ktime_sub(end,start)));				
	printk("time for interrupt = %d ns\n", average);

	return 0;
}

signed32 recv_intr_cb (unsigned32 local_adapter_number,
                       void *arg, unsigned32 interrupt_number)
{
	int i =0;
       /* trigger the interrupt */
        int status = sci_trigger_interrupt_flag(remote_send_intr_hdl[0],
		                                NO_FLAGS);
		if (status != 0) {
		        printk("%s: ERROR: in sci_trigger_interrupt_flag: %d\n", status);
		}
	return 0;
}
#if SERVER
static int send_init(void)
{
	int status = 0, value = 0;
	unsigned int local_segid = 0, remote_segid = 0;

        printk("send start initial\n");
	status = sci_bind (&sci_resources_binding);
	if (status != 0) {
		printk(" Error in sci_bind: %d\n", status);
	}

	/* Query node ID */
	status = sci_query_adapter_number(local_adapter_number, Q_ADAPTER_NODE_ID,
					NO_FLAGS, &value);
	if (status != 0) {
                printk(" Error in sci_create_segment: %d\n", status);
        } else {
		printk(" adapter number = %d\n", value);
	}

	local_segid = (value << 8) + TARGET_NODE + SEND_OFFSET + 0;
	remote_segid = (TARGET_NODE << 8) + value + RECV_OFFSET + 0;

	printk(" Segid: Local - %d, remote - %d\n", local_segid, remote_segid);

	/* Create a local segment with above ID */
	status = sci_create_segment(sci_resources_binding, MODULE_ID, local_segid,
                   NO_FLAGS, SEG_SIZE, local_cbfunc, NULL, &local_send_seg_hdl[0]);
	if (status != 0) {
		printk(" Error in sci_create_segment: %d\n", status);
	}

	status = sci_export_segment(local_send_seg_hdl[0], 
					local_adapter_number,
                   			NO_FLAGS);
	if (status != 0) {
		printk(" Error in sci_export_segment: %d\n", status);
	}

	send_vaddr[0] = (vkaddr_t *) sci_local_kernel_virtual_address (local_send_seg_hdl[0]);
	if (send_vaddr != NULL)
	{
		printk(" local segment kernel virtual address is: %x\n", send_vaddr[0]);
	}

	status = sci_set_local_segment_available (local_send_seg_hdl[0],
                                 	local_adapter_number);
	if (status != 0)
	{
		printk(" Error in sci_set_local_segment_available: %d\n", status);
	}

	status = sci_probe_node(MODULE_ID, NO_FLAGS, TARGET_NODE,
               local_adapter_number, &send_report);
        if (status != 0)
        {
                printk(" Error in sci_set_local_segment_available: %d\n", status);
        }
	else
		printk("probe status = %d\n", send_report);


	/* Create and initialize iterrupt */
	status = sci_allocate_interrupt_flag(sci_resources_binding, local_adapter_number,
                                  0, NO_FLAGS, send_intr_cb ,
                                  NULL, &local_send_intr_hdl[0]);
        if (status != 0)
        {
                printk(" Local interrupt cannot be created %d\n", status);
        }

	local_send_intr_no[0] = sci_interrupt_number(local_send_intr_hdl[0]);
	printk("Local interrupt number = %d\n", local_send_intr_no[0]);

 
	status = sci_is_local_segment_available(local_send_seg_hdl[0],
						local_adapter_number);
	if (status == 0)
	{
		printk(" Local segment not available to connect to\n");
	}

	do{
		status = sci_connect_segment(sci_resources_binding, TARGET_NODE, local_adapter_number,
                    MODULE_ID, remote_segid, NO_FLAGS, send_connect_cbfunc , NULL,
                    &remote_recv_seg_hdl[0]);
		if (status != 0)
		{
			msleep(1000);
			printk(" Error in sci_connect_segment: %d\n", status);
		}
		msleep(1000);
	}
	while (send_connected_flag[0] == 0);

	/* Resetting connect flag */
	send_connected_flag[0] = 0;

	status = sci_map_segment(remote_recv_seg_hdl[0], NO_FLAGS,
                0, SEG_SIZE, &send_map_handle[0]);
	if (status != 0)
	{
		printk(" Error in sci_map_segment: %d\n", status);
	}

 	send_remote_vaddr[0] = sci_kernel_virtual_address_of_mapping(send_map_handle[0]);
	if (send_remote_vaddr != NULL)
	{
		printk(" Remote virtual address: %\n", send_remote_vaddr[0]);
	}

	*send_remote_vaddr[0] = local_send_intr_no[0];

	printk("Remote memory value = %d\n", *send_remote_vaddr[0]);

	while (*send_vaddr[0] == 0)
	{
		msleep(100);
	}

	remote_recv_intr_no[0] = *send_vaddr[0];

	status = sci_connect_interrupt_flag(sci_resources_binding, TARGET_NODE,
                                        local_adapter_number, remote_recv_intr_no[0],
                                        NO_FLAGS, &remote_recv_intr_hdl[0]);
        if (status != 0)
        {
               	printk("Unable to connect to remote interrupt: %d\n", status);
        }

         /* trigger the interrupt */
		start = ktime_get();
		status = sci_trigger_interrupt_flag(remote_recv_intr_hdl[0],
			                        NO_FLAGS);
		triend = ktime_get();
		int sb = (ktime_to_ns(ktime_sub(triend,start)));		
			
		printk(" time for trigger interrupt = %d ns\n", sb);
		if (status != 0) {
			printk("%s: ERROR: in sci_trigger_interrupt_flag: %d\n", __func__, status);
		}
	return status;
	
}

static int send_cleanup(void)
{
	int status = 0;

        /* Remove interrupt */
        status = sci_disconnect_interrupt_flag(&remote_recv_intr_hdl[0], NO_FLAGS);
        if (status != 0) {
                printk(" Error in sci_disconnect_interrupt_flag: %d\n", status);
        }

	/* Deintialize path */
	status = sci_unmap_segment(&send_map_handle[0],NO_FLAGS);
	if (status != 0) {
		printk(" Error in sci_unmap_segment: %d\n", status);
	}

	status = sci_disconnect_segment(&remote_recv_seg_hdl[0], NO_FLAGS);
	if (status != 0) {
		printk(" Error in sci_disconnect_segment: %d\n", status);
	}

	status = sci_remove_interrupt_flag(&local_send_intr_hdl[0], NO_FLAGS);
        if (status != 0) {
                printk(" Error in sci_remove_interrupt_flag: %d\n", status);
        }

	status = sci_set_local_segment_unavailable (local_send_seg_hdl[0],
						 local_adapter_number);
	if (status != 0) {
		printk(" Error in sci_set_local_segment_unavailable: %d\n", status);
	}

	status = sci_unexport_segment(local_send_seg_hdl[0], local_adapter_number,
                     			NO_FLAGS);
	if (status != 0) {
		printk(" Error in sci_unexport_segment: %d\n", status);
	}

	status = sci_remove_segment(&local_send_seg_hdl[0],NO_FLAGS);
	if (status != 0) {
		printk(" Error in sci_remove_segment: %d\n", status);
	}

	status = sci_unbind (&sci_resources_binding);
	if (status != 0) {
		printk(" Error in sci_bind: %d\n", status);
	}

	return status;
}

#endif
#if CLIENT
static int recv_init(void)
{
	int status = 0, value = 0;
	unsigned int local_segid = 0, remote_segid = 0;

	status = sci_bind (&sci_resources_binding);
	if (status != 0)
	{
		printk(" Error in sci_bind: %d\n", status);
	}

	/* Query node ID */
	status = sci_query_adapter_number(local_adapter_number, Q_ADAPTER_NODE_ID,
					NO_FLAGS, &value);
	if (status != 0)
        {
                printk(" Error in sci_query_adapter_number: %d\n", status);
        }
	else
		printk(" adapter number = %d\n", value);

	local_segid = (value << 8) + TARGET_NODE + RECV_OFFSET + 0;
	remote_segid = (TARGET_NODE << 8) + value + SEND_OFFSET + 0;

	printk(" Segid: Local - %d, remote - %d\n", local_segid, remote_segid);

	/* Create a local segment with above ID */
	status = sci_create_segment(sci_resources_binding, MODULE_ID, local_segid,
                   NO_FLAGS, SEG_SIZE, local_cbfunc, NULL, &local_recv_seg_hdl[0]);
	if (status != 0)
	{
		printk(" Error in sci_create_segment: %d\n", status);
	}

	status = sci_export_segment(local_recv_seg_hdl[0], 
					local_adapter_number,
                   			NO_FLAGS);
	if (status != 0)
	{
		printk(" Error in sci_export_segment: %d\n", status);
	}

	recv_vaddr[0] = (vkaddr_t *) sci_local_kernel_virtual_address (local_recv_seg_hdl[0]);
	if (send_vaddr != NULL)
	{
		printk(" local segment kernel virtual address is: %x\n", recv_vaddr[0]);
	}

	status = sci_set_local_segment_available (local_recv_seg_hdl[0],
                                 	local_adapter_number);
	if (status != 0)
	{
		printk(" Error in sci_set_local_segment_available: %d\n", status);
	}

	status = sci_probe_node(MODULE_ID, NO_FLAGS, TARGET_NODE,
               local_adapter_number, &recv_report);
        if (status != 0)
        {
                printk(" Error in sci_set_local_segment_available: %d\n", status);
        }
	else
		printk("probe status = %d\n", recv_report);


	/* Create and initialize iterrupt */
	status = sci_allocate_interrupt_flag(sci_resources_binding, local_adapter_number,
                                  0, NO_FLAGS, recv_intr_cb ,
                                  NULL, &local_recv_intr_hdl[0]);
        if (status != 0)
        {
                printk(" Local interrupt cannot be created %d\n", status);
        }

	local_recv_intr_no[0] = sci_interrupt_number(local_recv_intr_hdl[0]);
	printk("Local interrupt number = %d\n", local_recv_intr_no[0]);

 
	status = sci_is_local_segment_available(local_recv_seg_hdl[0],
						local_adapter_number);
	if (status == 0)
	{
		printk(" Local segment not available to connect to\n");
	}

	do{
		status = sci_connect_segment(sci_resources_binding, TARGET_NODE, local_adapter_number,
                    MODULE_ID, remote_segid, NO_FLAGS, recv_connect_cbfunc , NULL,
                    &remote_send_seg_hdl[0]);
		if (status != 0)
		{
			msleep(1000);
			printk(" Error in sci_connect_segment: %d\n", status);
		}
		msleep(1000);
	}
	while (recv_connected_flag[0] == 0);

	/*resetting connection flag */
	recv_connected_flag[0] = 0;

	status = sci_map_segment(remote_send_seg_hdl[0], NO_FLAGS,
                0, SEG_SIZE, &recv_map_handle[0]);
	if (status != 0)
	{
		printk(" Error in sci_map_segment: %d\n", status);
	}

 	recv_remote_vaddr[0] = sci_kernel_virtual_address_of_mapping(recv_map_handle[0]);
	if (recv_remote_vaddr != NULL)
	{
		printk(" Remote virtual address: %x\n", recv_remote_vaddr[0]);
	}

	*recv_remote_vaddr[0] = local_recv_intr_no[0];

	printk("Remote memory value = %d\n", *recv_remote_vaddr[0]);

	while (*recv_vaddr[0] == 0) {
		msleep(100);
	}

	remote_send_intr_no[0] = *recv_vaddr[0];

	status = sci_connect_interrupt_flag(sci_resources_binding, TARGET_NODE,
                                        local_adapter_number, remote_send_intr_no[0],
                                        NO_FLAGS, &remote_send_intr_hdl[0]);
        if (status != 0) {
               	printk("Unable to connect to remote interrupt: %d\n", status);
        }

	return status;
}
static int recv_cleanup(void)
{
	int status = 0;

        /* Remove interrupt */
        status = sci_disconnect_interrupt_flag(&remote_send_intr_hdl[0], NO_FLAGS);
        if (status != 0) {
                printk(" Error in sci_disconnect_interrupt_flag: %d\n", status);
        }

	/* Deintialize path */
	status = sci_unmap_segment(&recv_map_handle[0],NO_FLAGS);
	if (status != 0) {
		printk(" Error in sci_unmap_segment: %d\n", status);
	}

	status = sci_disconnect_segment(&remote_send_seg_hdl[0], NO_FLAGS);
	if (status != 0) {
		printk(" Error in sci_disconnect_segment: %d\n", status);
	}

	status = sci_remove_interrupt_flag(&local_recv_intr_hdl[0], NO_FLAGS);
        if (status != 0) {
                printk(" Error in sci_remove_interrupt_flag: %d\n", status);
        }

	status = sci_set_local_segment_unavailable (local_recv_seg_hdl[0],
						 local_adapter_number);
	if (status != 0) {
		printk(" Error in sci_set_local_segment_unavailable: %d\n", status);
	}

	status = sci_unexport_segment(local_recv_seg_hdl[0], local_adapter_number,
                     			NO_FLAGS);
	if (status != 0) {
		printk(" Error in sci_unexport_segment: %d\n", status);
	}

	status = sci_remove_segment(&local_recv_seg_hdl[0],NO_FLAGS);
	if (status != 0) {
		printk(" Error in sci_remove_segment: %d\n", status);
	}

	status = sci_unbind (&sci_resources_binding);
	if (status != 0) {
		printk(" Error in sci_unbind: %d\n", status);
	}

	return status;
}
#endif


/* Main function */
int main(void)
{
    /* Initialize with module id */
    if (!sci_initialize(MODULE_ID)) {
	printk(KERN_ERR "sci_query: %s: Cannot initialize GENIF\n", __FUNCTION__);
	return -1;
    }

    #if SERVER
	return send_init();
	#endif
	#if CLIENT
	return recv_init();
    #endif
  
}

/* Init */
int interrupt_init(void) {
    printk("sci_interrupt: init\n");

    if (main() < 0) {
	printk("sci_interrupt: init failed\n");
	return -1;
    }

    return 0;
}

/* Cleanup */
void interrupt_cleanup(void)
{
    #if SERVER
	send_cleanup();
	#endif
	#if CLIENT
	recv_cleanup();
	#endif

    sci_terminate (MODULE_ID);
	printk("Successfully unloaded module\n");
}


#ifdef MODULE

/* Register init and exit functions */
module_init(interrupt_init);
module_exit(interrupt_cleanup);


MODULE_AUTHOR("Dolphin ICS");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("SCI interrupt.");

#endif
