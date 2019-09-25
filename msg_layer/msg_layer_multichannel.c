/*
 * pcn_kmesg.c - Kernel Module for Popcorn Messaging Layer over Socket
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/semaphore.h>
#include <linux/file.h>
#include <linux/ktime.h>

#include <linux/pcn_kmsg.h>

#include <linux/fdtable.h>

#include <linux/net.h>
#include <net/sock.h>
#include <linux/tcp.h>
#include <linux/in.h>
#include <asm/uaccess.h>
#include <linux/socket.h>

#include <linux/delay.h>
#include <linux/time.h>
#include <asm/atomic.h>
#include <linux/completion.h>

#include <popcorn/init.h>
#include <linux/cpumask.h>
#include <linux/sched.h>
#include <linux/moduleparam.h>
#include <linux/vmalloc.h>

#include "genif.h"

/* Macro definitions */
#define MAX_NUM_CHANNELS 	6
#define SEND_OFFSET		1
#define RECV_OFFSET		(MAX_NUM_CHANNELS+SEND_OFFSET)
#if CONFIG_ARM64
#define TARGET_NODE		4
#else
#define TARGET_NODE		8
#endif
#define NO_FLAGS		0
#define SEG_SIZE		20000
#define MAX_NUM_BUF		20
#define RECV_THREAD_POOL	2

#define ENABLE_DMA		0
#define SEND_QUEUE_POOL		0
#define CHANNEL                 1
/* for debug */
#define TEST_MSG_LAYER 		1
#define TEST_SERVER		0

typedef struct _pool_buffer{
	char* buff;
	int is_free;
	int status;
}pool_buffer_t;

typedef struct _send_wait{
        struct list_head list;
        struct semaphore _sem;
        void * msg;
        int error;
        int dst_cpu;
	pool_buffer_t *assoc_buf;
}send_wait;

typedef struct _recv_data{
	int channel_num;
	int is_worker;
}recv_data_t;

/* for input parameter from outside*/
static int paysize =0;
module_param(paysize, int, 0);
static int segsize=0;
module_param(segsize, int, 0);

ktime_t dqend[25], dqstart[25];
static int dqi=0;

ktime_t eqend[25], eqstart[25];
static int eqi=0;

ktime_t sendrstart[25], sendrend[25];
static int sendri=0;

ktime_t pciend[25], pcistart[25];
static int pcii=0;

ktime_t recvend[25], recvstart[25];
static int recvi=0;

ktime_t trigend[25], trigstart[25];
static int trigi=0;

ktime_t recvtrigend[25], recvtrigstart[25];
static int recvtrigi=0;



ktime_t readend[25], readstart[25];
static int readi=0;
static int n=0;
static int xa=0;

static int recvindex=0;
static int index=0;
static int connection_handler(void* arg0);
static int send_thread(int arg0);
static int send_process(send_wait * send_data, int channel_num);

/* PCI function declarations */
static int pcie_send_init(int channel_num);
static int pcie_recv_init(int channel_num);
static int pcie_send_cleanup(int channel_num);
static int pcie_recv_cleanup(int channel_num);

#if ENABLE_DMA
static int dma_init(int channel_num);
static int dma_cleanup(int channel_num);
#endif

struct task_struct *handler[MAX_NUM_CHANNELS];
struct task_struct *sender_handler[MAX_NUM_CHANNELS];
struct task_struct *pool_thread_handler[MAX_NUM_CHANNELS*RECV_THREAD_POOL];


int is_connection_done=PCN_CONN_WATING;
 
inline int pcn_connection_status(void)
{
	return is_connection_done;
}

#if SEND_QUEUE_POOL
struct completion send_q_empty[MAX_NUM_CHANNELS];
spinlock_t send_q_mutex[MAX_NUM_CHANNELS];
static send_wait send_wait_q[MAX_NUM_CHANNELS];
static atomic_t send_channel;
#else
struct completion send_q_empty;
spinlock_t send_q_mutex;
static send_wait send_wait_q;
#endif

static volatile pool_buffer_t send_buf[MAX_NUM_BUF];
struct semaphore pool_buf_cnt;

/* for dolphin PCIE interconnect */
unsigned module_id = 0, local_adapter_number = 0;
sci_binding_t send_binding[MAX_NUM_CHANNELS] = {NULL}, recv_binding[MAX_NUM_CHANNELS] = {NULL};
sci_l_segment_handle_t local_send_seg_hdl[MAX_NUM_CHANNELS] = {NULL}, local_recv_seg_hdl[MAX_NUM_CHANNELS] = {NULL};
sci_r_segment_handle_t remote_send_seg_hdl[MAX_NUM_CHANNELS] = {NULL}, remote_recv_seg_hdl[MAX_NUM_CHANNELS] = {NULL};
vkaddr_t *send_vaddr[MAX_NUM_CHANNELS] = {NULL}, *recv_vaddr[MAX_NUM_CHANNELS] = {NULL}, *send_remote_vaddr[MAX_NUM_CHANNELS] = {NULL}, *recv_remote_vaddr[MAX_NUM_CHANNELS] = {NULL};
int local_send_intr_no[MAX_NUM_CHANNELS] = {0}, remote_send_intr_no[MAX_NUM_CHANNELS] = {0}, local_recv_intr_no[MAX_NUM_CHANNELS] = {0}, remote_recv_intr_no[MAX_NUM_CHANNELS] = {0};
volatile int send_connected_flag[MAX_NUM_CHANNELS] = {0}, recv_connected_flag[MAX_NUM_CHANNELS] = {0};
sci_map_handle_t send_map_handle[MAX_NUM_CHANNELS] = {NULL}, recv_map_handle[MAX_NUM_CHANNELS] = {NULL};
probe_status_t send_report, recv_report;

#if ENABLE_DMA

/* dma variables */
sci_dma_queue_t dma_queue[MAX_NUM_CHANNELS];
ioaddr64_t local_io[MAX_NUM_CHANNELS];
volatile int dma_done[MAX_NUM_CHANNELS] = {0};
int subuser_id[MAX_NUM_CHANNELS] = {1};

struct completion dma_complete[MAX_NUM_CHANNELS];

#endif

struct completion dma_completion;

struct completion send_intr_flag[MAX_NUM_CHANNELS]; 
struct completion recv_intr_flag[MAX_NUM_CHANNELS]; 

struct semaphore send_connDone[MAX_NUM_CHANNELS];
struct semaphore recv_connDone[MAX_NUM_CHANNELS];

sci_l_interrupt_handle_t local_send_intr_hdl[MAX_NUM_CHANNELS] = {NULL}, local_recv_intr_hdl[MAX_NUM_CHANNELS] = {NULL};
sci_r_interrupt_handle_t remote_send_intr_hdl[MAX_NUM_CHANNELS] = {NULL}, remote_recv_intr_hdl[MAX_NUM_CHANNELS] = {NULL};

static int __init initialize(void);
int pci_kmsg_send_long(unsigned int dest_cpu, struct pcn_kmsg_long_message *lmsg, unsigned int payload_size);

#if CONFIG_ARM64
unsigned int my_cpu = 0;
#else
unsigned int my_cpu = 1;
#endif

#if TEST_MSG_LAYER
pcn_kmsg_cbftn callbacks[PCN_KMSG_TYPE_MAX];
#else
extern pcn_kmsg_cbftn callbacks[PCN_KMSG_TYPE_MAX];
extern send_cbftn send_callback;
#endif

#if TEST_MSG_LAYER

#define MSG_LENGTH 4096
#define NUM_MSGS 25

#define PROF_HISTOGRAM  0

static atomic_t recv_count;
static atomic_t exec_count;
static atomic_t send_count;

static atomic_t timer_send_count;
static atomic_t timer_recv_count;

static volatile pool_buffer_t recv_buf[MAX_NUM_BUF];
struct semaphore recv_buf_cnt;

struct test_msg_t
{
	struct pcn_kmsg_hdr hdr;
	unsigned char payload[MSG_LENGTH];
};

static int test_thread(void* arg0);
struct task_struct *test_handler;

pcn_kmsg_cbftn handle_selfie_test(struct pcn_kmsg_message* inc_msg);
#endif

char *msg_names[] = {
	"TEST",
	"TEST_LONG",
	"CHECKIN",
	"MCAST",
	"PROC_SRV_CLONE_REQUEST",
	"PROC_SRV_CREATE_PROCESS_PAIRING",
	"PROC_SRV_EXIT_PROCESS",
	"PROC_SRV_BACK_MIG_REQUEST",
	"PROC_SRV_VMA_OP",
	"PROC_SRV_VMA_LOCK",
	"PROC_SRV_MAPPING_REQUEST",
	"PROC_SRV_NEW_KERNEL",
	"PROC_SRV_NEW_KERNEL_ANSWER",
	"PROC_SRV_MAPPING_RESPONSE",
	"PROC_SRV_MAPPING_RESPONSE_VOID",
	"PROC_SRV_INVALID_DATA",
	"PROC_SRV_ACK_DATA",
	"PROC_SRV_THREAD_COUNT_REQUEST",
	"PROC_SRV_THREAD_COUNT_RESPONSE",
	"PROC_SRV_THREAD_GROUP_EXITED_NOTIFICATION",
	"PROC_SRV_VMA_ACK",
	"PROC_SRV_BACK_MIGRATION",
	"PCN_PERF_START_MESSAGE",
	"PCN_PERF_END_MESSAGE",
	"PCN_PERF_CONTEXT_MESSAGE",
	"PCN_PERF_ENTRY_MESSAGE",
	"PCN_PERF_END_ACK_MESSAGE",
	"START_TEST",
	"REQUEST_TEST",
	"ANSWER_TEST",
	"MCAST_CLOSE",
	"SHMTUN",
	"REMOTE_PROC_MEMINFO_REQUEST",
	"REMOTE_PROC_MEMINFO_RESPONSE",
	"REMOTE_PROC_STAT_REQUEST",
	"REMOTE_PROC_STAT_RESPONSE",
	"REMOTE_PID_REQUEST",
	"REMOTE_PID_RESPONSE",
	"REMOTE_PID_STAT_REQUEST",
	"REMOTE_PID_STAT_RESPONSE",
	"REMOTE_PID_CPUSET_REQUEST",
	"REMOTE_PID_CPUSET_RESPONSE",
	"REMOTE_SENDSIG_REQUEST",
	"REMOTE_SENDSIG_RESPONSE",
	"REMOTE_SENDSIGPROCMASK_REQUEST",
	"REMOTE_SENDSIGPROCMASK_RESPONSE",
	"REMOTE_SENDSIGACTION_REQUEST",
	"REMOTE_SENDSIGACTION_RESPONSE",
	"REMOTE_IPC_SEMGET_REQUEST",
	"REMOTE_IPC_SEMGET_RESPONSE",
	"REMOTE_IPC_SEMCTL_REQUEST",
	"REMOTE_IPC_SEMCTL_RESPONSE",
	"REMOTE_IPC_SHMGET_REQUEST",
	"REMOTE_IPC_SHMGET_RESPONSE",
	"REMOTE_IPC_SHMAT_REQUEST",
	"REMOTE_IPC_SHMAT_RESPONSE",
	"REMOTE_IPC_FUTEX_WAKE_REQUEST",
	"REMOTE_IPC_FUTEX_WAKE_RESPONSE",
	"REMOTE_PFN_REQUEST",
	"REMOTE_PFN_RESPONSE",
	"REMOTE_IPC_FUTEX_KEY_REQUEST",
	"REMOTE_IPC_FUTEX_KEY_RESPONSE",
	"REMOTE_IPC_FUTEX_TOKEN_REQUEST",
	"REMOTE_PROC_CPUINFO_RESPONSE",
	"REMOTE_PROC_CPUINFO_REQUEST",
	"PROC_SRV_CREATE_THREAD_PULL",
	"PCN_KMSG_TERMINATE",
	"SELFIE_TEST",
	"FILE_MIGRATE_REQUEST",
	"FILE_OPEN_REQUEST",
	"FILE_OPEN_REPLY",
	"FILE_STATUS_REQUEST",
	"FILE_STATUS_REPLY",
	"FILE_OFFSET_REQUEST",
	"FILE_OFFSET_REPLY",
	"FILE_CLOSE_NOTIFICATION",
	"FILE_OFFSET_UPDATE",
	"FILE_OFFSET_CONFIRM",
	"FILE_LSEEK_NOTIFICATION",
	"SCHED_PERIODIC"
};

int ciradd(int num)
{

   return (num +1) == MAX_NUM_CHANNELS ? 0 : num+1;
}



/* PCIe Callback functions */
int local_cbfunc (void *arg, sci_l_segment_handle_t local_segment_handle,
			unsigned32 reason, unsigned32 source_node, 
                       unsigned32 local_adapter_number)
{
	printk(" In %s: reason = %d for %p\n", __func__, reason, local_segment_handle);
	return 0;
}

int send_connect_cbfunc(void *arg,
                        sci_r_segment_handle_t remote_segment_handle,
                        unsigned32 reason,
                        unsigned32 status)
{
	int i = 0;

	if (status == 0) {
                for (i = 0; i<MAX_NUM_CHANNELS; i++) {
                        if (remote_recv_seg_hdl[i] == remote_segment_handle) {
				printk("Reason = %d, status = %d for %d\n", reason, status, i);
                                send_connected_flag[i] = 1;
				break;
			}
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
		for (i = 0; i<MAX_NUM_CHANNELS; i++) {
			if (remote_send_seg_hdl[i] == remote_segment_handle) {
				printk("Reason = %d, status = %d for %d\n", reason, status, i);
				recv_connected_flag[i] = 1;
				break;
			}
		}
	}

	return 0;
}


signed32 send_intr_cb (unsigned32 local_adapter_number,
                       void *arg, unsigned32 interrupt_number)
{
	int i =0;

	for (i = 0;  i<MAX_NUM_CHANNELS; i++) {
		if (interrupt_number == local_send_intr_no[i]) {
			//printk("Remote send interrupt for %d %d\n", i, interrupt_number);
			complete(&send_intr_flag[i]);
			break;
		}
	}
	return 0;
}

signed32 recv_intr_cb (unsigned32 local_adapter_number,
                       void *arg, unsigned32 interrupt_number)
{
	int i =0;

	for (i = 0;  i<MAX_NUM_CHANNELS; i++) {
		if (interrupt_number == local_recv_intr_no[i]) {
			//printk("Remote recv interrupt for %d %d\n", i, interrupt_number);
			complete(&recv_intr_flag[i]);
			break;
		}
	}
	return 0;
}

#if ENABLE_DMA
int dma_cb(void IN *arg, dis_dma_status_t dmastatus)
{
	sci_dma_queue_t *temp = (sci_dma_queue_t *)arg;
	int i = 0;
 
	for (i = 0; i<MAX_NUM_CHANNELS; i++) {
		if (dma_queue[i] == *temp) {
			//printk("DMA transfer status = %d %lx %lx\n", dmastatus, *temp, dma_queue[i]);
			complete(&dma_complete[i]);   
                        complete(&dma_completion); 
        		xa =i;                
                     
		}
	}

	return 0;
}
#endif

/* Queue functions */

#if SEND_QUEUE_POOL
static void enq_send(send_wait *strc, int index)
{
	spin_lock(&send_q_mutex[index]);
	//INIT_LIST_HEAD(&(strc->list));
	list_add_tail(&(strc->list), &(send_wait_q[index].list));
	complete(&send_q_empty[index]);
	spin_unlock(&send_q_mutex[index]);
}

static send_wait * dq_send(int index)
{
	send_wait *tmp;
	wait_for_completion(&send_q_empty[index]);
	spin_lock(&send_q_mutex[index]);
	if (list_empty(&send_wait_q[index].list)){
		printk("List %d is empty...\n", index);
		spin_unlock(&send_q_mutex[index]);
		return NULL;	
	} else {
		tmp = list_first_entry (&send_wait_q[index].list, send_wait, list);
		list_del(send_wait_q[index].list.next);
		spin_unlock(&send_q_mutex[index]);
		return tmp;
	}
}

#else
static void enq_send(send_wait *strc)
{
	spin_lock(&send_q_mutex);
	INIT_LIST_HEAD(&(strc->list));
	list_add_tail(&(strc->list), &(send_wait_q.list));
	complete(&send_q_empty);
	spin_unlock(&send_q_mutex);
}

static send_wait * dq_send(void)
{
	send_wait *tmp;
	wait_for_completion(&send_q_empty);
	spin_lock(&send_q_mutex);
	if (list_empty(&send_wait_q.list)){
		printk("List is empty...\n");
		spin_unlock(&send_q_mutex);
		return NULL;	
	} else {
		tmp = list_first_entry (&send_wait_q.list, send_wait, list);
		list_del(send_wait_q.list.next);
		spin_unlock(&send_q_mutex);
		return tmp;
	}
}
#endif


// Initialize callback table to null, set up control and data channels
int __init initialize()
{
	int status = 0, i = 0, j = 0, channel_num=0; 
	recv_data_t* recv_data;
	struct sched_param param = {.sched_priority = 10};

	printk("MSG_LAYER: Initialization\n");
#if SEND_QUEUE_POOL
	for (i = 0; i<MAX_NUM_CHANNELS; i++) {
		INIT_LIST_HEAD(&send_wait_q[i].list);
		init_completion(&(send_q_empty[i]));
		spin_lock_init(&(send_q_mutex[i]));

		atomic_set(&send_channel, 0);
	}
#else
	INIT_LIST_HEAD(&send_wait_q.list);
	init_completion(&(send_q_empty));
#endif

	for (i = 0; i<MAX_NUM_BUF; i++) {
		send_buf[i].buff = vmalloc(segsize);

		if (send_buf[i].buff == NULL) {
			printk("************* Failed to allocate buffer pool **************\n");
		}

		send_buf[i].is_free = 1;
		send_buf[i].status = 0;
		smp_wmb();

		printk("allocated buffer %p\n", send_buf[i].buff);
	}

	sema_init(&pool_buf_cnt, MAX_NUM_BUF);


#if TEST_MSG_LAYER

	for (i = 0; i<MAX_NUM_BUF; i++) {
		recv_buf[i].buff = vmalloc(segsize);

		if (recv_buf[i].buff == NULL) {
			printk("************* Failed to allocate buffer pool **************\n");
		}

		recv_buf[i].is_free = 1;
		smp_wmb();

		printk("allocated buffer %p\n", recv_buf[i].buff);
	}

	sema_init(&recv_buf_cnt, MAX_NUM_BUF);

#endif

	for (i = 0; i<MAX_NUM_CHANNELS; i++) {
		sema_init(&send_connDone[i],0);
		sema_init(&recv_connDone[i],0);
		init_completion(&send_intr_flag[i]);
		init_completion(&recv_intr_flag[i]);

#if ENABLE_DMA
		init_completion(&dma_complete[i]);
#endif
		complete(&send_intr_flag[i]);	
	}

        init_completion(&dma_completion);
	/* Initilaize the adapter */
	do {
		status = sci_initialize (module_id);
		if (status == 0)
		{
			printk(" Error in sci_initialize: %d\n", status);
			msleep(100);
		}
	}  while (status == 0);

	for (i = 0; i<CHANNEL; i++) {
		recv_data = kmalloc(sizeof(recv_data_t), GFP_KERNEL);
		if (recv_data == NULL) {
			printk("MSG_LAYER: Failed to allocate memory\n");
			return 0;
		}
 
		recv_data->channel_num = i;
		recv_data->is_worker = 0;

		handler[i] = kthread_run(connection_handler, recv_data, "pcn_recv_thread");
		if (handler[i] < 0) {
			printk(KERN_INFO "kthread_run failed! Messaging Layer not initialized\n");
			return (long long int)handler;
		}

		sched_setscheduler(handler[i], SCHED_FIFO, &param);
		set_cpus_allowed_ptr(handler[i], cpumask_of(i));
        
		for (channel_num =0; channel_num < MAX_NUM_CHANNELS; channel_num++)
        { 
	        status = pcie_send_init(channel_num);
	        if (status != 0) {
		        printk("Failed to initialize pcie connection\n");
	        }

#if ENABLE_DMA
	        status = dma_init(channel_num);
	        if (status != 0) {
		        printk("Failed in dma_init: %d\n", status);
	        }
#endif
	
 	        up(&send_connDone[channel_num]);
	        printk("%s: INFO: Connection Done...PCN_SEND Thread\n", __func__);
        }
			
	}

	for (i = 0; i<CHANNEL; i++) {
		for (j = 0; j<(RECV_THREAD_POOL-1); j++) {

			recv_data = kmalloc(sizeof(recv_data_t), GFP_KERNEL);
			if (recv_data == NULL) {
				printk("MSG_LAYER: Failed to allocate memory\n");
				return 0;
			}

			recv_data->channel_num = i;
			recv_data->is_worker = 1;

			pool_thread_handler[i+j] = kthread_run(connection_handler, recv_data, "pcn_recv_pool");
			if (pool_thread_handler[i+j] < 0){
				printk(KERN_INFO "kthread_run failed! Messaging Layer not initialized\n");
				return (long long int)pool_thread_handler[i+j];
			}

			sched_setscheduler(pool_thread_handler[i+j], SCHED_FIFO, &param);
			set_cpus_allowed_ptr(pool_thread_handler[i+j], cpumask_of(i%NR_CPUS));
		}		
	}

	for (i = 0; i<MAX_NUM_CHANNELS; i++)
	        down_interruptible(&send_connDone[i]);

	for (i = 0; i<MAX_NUM_CHANNELS; i++)
        	down_interruptible(&recv_connDone[i]);

	is_connection_done=PCN_CONN_CONNECTED;

#if TEST_MSG_LAYER
	atomic_set(&exec_count, 0);
	atomic_set(&recv_count, 0);
	atomic_set(&send_count, 0);
	atomic_set(&timer_send_count, 0);
	atomic_set(&timer_recv_count, 0);

	pcn_kmsg_register_callback(PCN_KMSG_TYPE_SELFIE_TEST,
		handle_selfie_test);

	for (i = 0; i<MAX_NUM_CHANNELS; i++) {
		test_handler = kthread_run(test_thread, (void *)NULL, "pcn_test");
		if (test_handler < 0) {
			printk(KERN_INFO "kthread_run failed! Messaging Layer not initialized\n");
			return (long long int)test_handler;
		}
	}

#else /* TEST_MSG_LAYER */

	send_callback = pci_kmsg_send_long;
	smp_mb();

	/* Make init popcorn call */
	_init_RemoteCPUMask();

	printk(" Value of send ptr = %lx\n", send_callback);
#endif

	printk(KERN_INFO "Popcorn Messaging Layer Initialized\n");
	return 0;
}

#if TEST_MSG_LAYER

#if PROF_HISTOGRAM
ktime_t start[(NUM_MSGS*MAX_NUM_CHANNELS)+1];
ktime_t end[(NUM_MSGS*MAX_NUM_CHANNELS)+1];
unsigned long long time[(NUM_MSGS*MAX_NUM_CHANNELS)+1];
#else
ktime_t start, end;
static int time_started = 0;
#endif

pcn_kmsg_cbftn handle_selfie_test(struct pcn_kmsg_message* inc_msg)
{
#if !TEST_SERVER
	//int payload_size = MSG_LENGTH;
	int payload_size = paysize;

	pci_kmsg_send_long(1,(struct pcn_kmsg_long_message*)inc_msg, payload_size);
#endif

	return;
}

int test_thread(void* arg0)
{
	int i = 0;
	int temp_count = 0;

#if TEST_SERVER
	printk("Test function %s: called\n",__func__);

	pcn_kmsg_register_callback(PCN_KMSG_TYPE_SELFIE_TEST,
		handle_selfie_test);

	msleep(1000);

	struct test_msg_t *msg;
	//int payload_size = MSG_LENGTH;
	int payload_size = paysize;

	msg = (struct test_msg_t *) vmalloc(sizeof(struct test_msg_t));
	msg->hdr.type= PCN_KMSG_TYPE_SELFIE_TEST;
	memset(msg->payload,'b',payload_size);

#if !PROF_HISTOGRAM
	if (time_started == 0) {
		time_started = 1;
		start = ktime_get();
	}
#endif

	for (i = 0; i<(NUM_MSGS); i++) {
#if PROF_HISTOGRAM
		temp_count = atomic_inc_return(&timer_send_count);
		start[temp_count] = ktime_get();

		//printk("start_time = %lld\n", ktime_to_ns(start[temp_count]));
#endif
                pcistart[pcii] = ktime_get();
		pci_kmsg_send_long(1,(struct pcn_kmsg_long_message*)msg, payload_size);
                pciend[pcii] = ktime_get();
                pcii++;

/*		if (!(i%(NUM_MSGS/5))) {
			//printk("scheduling out\n");
			msleep(1);
		}*/
	}

	vfree(msg);
	printk("Finished Testing\n");
#endif
	
	return 0;
}

#endif /* TEST_MSG_LAYER */


int send_process(send_wait *send_data, int channel_num)
{
	int status = 0;
	
	struct pcn_kmsg_message* pcn_msg;

	


#if TEST_MSG_LAYER
		if (atomic_read(&send_count) == NUM_MSGS*MAX_NUM_CHANNELS)
			return  0;
#endif


		pcn_msg = (struct pcn_kmsg_message*) send_data->msg;
               
#if ENABLE_DMA
                eqstart[eqi] = ktime_get();
		memcpy(send_vaddr[channel_num], pcn_msg, pcn_msg->hdr.size);

		status = dis_start_dma_transfer(subuser_id[channel_num], send_vaddr[channel_num],
						local_io[channel_num], pcn_msg->hdr.size, 0, remote_recv_seg_hdl[channel_num],
						dma_cb, &dma_queue[channel_num], &dma_queue[channel_num], DMA_PUSH);
		if (status != 0) {
			printk(" Error in dis_start_dma_transfer: %d\n", status);
		}
               

		wait_for_completion(&dma_complete[channel_num]);
                eqend[eqi] =ktime_get();
                eqi++;
#else						
		
		memcpy(send_remote_vaddr[channel_num], pcn_msg, pcn_msg->hdr.size);
#endif
 
		trigstart[trigi] = ktime_get();
                status = sci_trigger_interrupt_flag(remote_recv_intr_hdl[channel_num],
			                        NO_FLAGS);
		if (status != 0) {
			printk("%s: ERROR: in sci_trigger_interrupt_flag: %d\n", __func__, status);
		}
                trigend[trigi] = ktime_get();
                trigi++;

#if TEST_MSG_LAYER
		atomic_inc(&send_count);

		if (atomic_read(&send_count) == NUM_MSGS*MAX_NUM_CHANNELS)
                {
			printk("after send all messages\n");
                         end = ktime_get();
                  int average = (ktime_to_ns(ktime_sub(end,start)))/1000;
                  average = average/(NUM_MSGS*MAX_NUM_CHANNELS);
  
                  printk("Average time for sendside  = %lld, payload size = %d\n", average, paysize);
                }

#endif

		send_data->assoc_buf->is_free = 1;
		smp_wmb();
		up(&pool_buf_cnt);

		kfree(send_data);
	
		
#if TEST_MSG_LAYER
	while (1) {
		msleep(10);
		if (kthread_should_stop()) {
			printk("coming out of send thread\n");
			return 0;
		}
	}
#endif /*TEST_MSG_LAYER*/

	return 0;
}

static int connection_handler_cnt = 0;
int connection_handler(void* arg0)
{
	struct pcn_kmsg_message *pcn_msg, *temp;
	int status = 0, i = 0, channel_num = 0, retry =0;
	unsigned long long average = 0;
	pcn_kmsg_cbftn ftn;
	recv_data_t* thread_data;

#if PROF_HISTOGRAM
	int temp_count = 0;
	int j = 0;
#endif

	thread_data = arg0;
	channel_num = thread_data->channel_num;

	msleep(100);
	printk("%s: INFO: Channel  %d %d\n", __func__, thread_data->channel_num, thread_data->is_worker);
	if (thread_data->is_worker == 0) {
		printk("%s: INFO: Initializing recv channel %d\n", __func__, channel_num);
	        for (channel_num = 0; channel_num < MAX_NUM_CHANNELS; channel_num++)
                {
          	    status = pcie_recv_init(channel_num);
		    if (status != 0) {
			printk("%s: ALERT: Failed to initialize pcie connection in the channel %d\n", __func__, channel_num);
			return;
		    }

		    up(&recv_connDone[channel_num]);
		    printk("%s: INFO Receive connection successfully completed..!! in the channel  %d\n", __func__, channel_num);
                 }
            

	}
	kfree(thread_data);

	while (1) {
		if (kthread_should_stop()) {
			printk("coming out of send thread\n");
			return 0;
		}

		/* Wait on remote to complete using the channel */
                #if !TEST_SERVER
		recvstart[recvi] = ktime_get();
                #endif
                wait_for_completion(&recv_intr_flag[channel_num]);
                #if !TEST_SERVER
                recvend[recvi] = ktime_get();
                recvi++;
                #endif
		/* Ajith :  the wait for completion will wake up only one thread on interrupt callback */

#if TEST_MSG_LAYER
                if (atomic_read(&recv_count) == NUM_MSGS*MAX_NUM_CHANNELS)
                        break;
#endif

        connection_handler_cnt++;
        if (connection_handler_cnt > 1)
        	printk("%s: ALERT: detected connection_handler_cnt %d\n", __func__, connection_handler_cnt);
		temp = (struct pcn_kmsg_message*)recv_vaddr[channel_num];
		if (!temp)
			printk("%s: ERROR: temp is zero\n", __func__);


#if TEST_MSG_LAYER
		down_interruptible(&recv_buf_cnt);
do_retry:
		for (i = 0; i<MAX_NUM_BUF; i++) {
			if ( atomic_cmpxchg( ((atomic_t *) &recv_buf[i].is_free), 1, 0) == 1 ) {
				smp_wmb();
				break;
			}
			/*if (recv_buf[i].is_free != 0) {
				recv_buf[i].is_free = 0;
				smp_wmb();
				break;
			}*/
		}

		if (i == MAX_NUM_BUF) {
			if ( !(retry % 1000))
				printk(KERN_ERR"%s: ERROR: Couldnt find a free buffer. Retry %d\n", __func__, retry);
			retry++;
			goto do_retry;
		}

		pcn_msg = recv_buf[i].buff;
#else
do_retry:
		pcn_msg = (struct pcn_kmsg_message *) vmalloc(temp->hdr.size);
		if (pcn_msg == NULL) {
			if ( !(retry % 1000))
				printk(KERN_ERR"%s: ERROR: Failed to allocate recv buffer size %d\n", __func__, temp->hdr.size);
			retry++;
			goto do_retry;
		}
#endif
                #if !TEST_SERVER
                readstart[readi] = ktime_get();
                #endif
		memcpy(pcn_msg, recv_vaddr[channel_num], temp->hdr.size);
		        recvindex = recvindex%MAX_NUM_CHANNELS;
                #if !TEST_SERVER
                readend[readi] = ktime_get();
                readi++;
                #endif
   //             printk("recv msg: %s\n", (*pcn_msg).payload[0]);
	       /* trigger the interrupt */
     //            printk("recv triggered interrupt number: %d\n", remote_send_intr_hdl[channel_num]);
	        #if !ENABLE_DMA
                #if !TEST_SERVER
                recvtrigstart[trigi] = ktime_get();
                #endif
				if (recvindex ==0 )
             	{
					status = sci_trigger_interrupt_flag(remote_send_intr_hdl[0],
		                                NO_FLAGS);
				}
				recvindex++;
                #if !TEST_SERVER
                recvtrigend[trigi] = ktime_get();
                recvtrigi++;
                #endif
                #endif
		if (status != 0) {
		        printk("%s: ERROR: in sci_trigger_interrupt_flag: %d\n", status);
		}
		connection_handler_cnt--; // safe to release the control here

#if TEST_MSG_LAYER
		atomic_inc(&recv_count);
#endif

		if (pcn_msg->hdr.type < 0) {
			printk("%s: ERROR: Received invalid message type %d\n", pcn_msg->hdr.type);

#if TEST_MSG_LAYER
			recv_buf[i].is_free = 1;
			smp_wmb();

			up(&recv_buf_cnt);
#else
			vfree(pcn_msg);
#endif
		} else {
			ftn = callbacks[pcn_msg->hdr.type];
			if (ftn != NULL) {
				ftn(pcn_msg);
#if TEST_MSG_LAYER
				recv_buf[i].is_free = 1;
				smp_wmb();
				up(&recv_buf_cnt);
#endif
			} else {
				printk("%s: ERROR: Received message type %d size %d has no registered callback!\n",
						__func__, pcn_msg->hdr.type,pcn_msg->hdr.size);
#if TEST_MSG_LAYER
				recv_buf[i].is_free = 1;
				smp_wmb();
				up(&recv_buf_cnt);
#else
				vfree(pcn_msg);
#endif
			}
		}

#if TEST_MSG_LAYER

		atomic_inc(&exec_count);

#if PROF_HISTOGRAM

		temp_count = atomic_inc_return(&timer_recv_count);

		end[temp_count] = ktime_get();

		if (atomic_read(&exec_count) == (NUM_MSGS*MAX_NUM_CHANNELS)) {

			average = ktime_to_ns(ktime_sub(ktime_get(),start[1])) >> 10;

			printk("Time for each msg -----\n");
			for (j = 0; j<= (MAX_NUM_CHANNELS*NUM_MSGS); j++) {
				time[j] = ktime_to_ns(ktime_sub(end[j],start[j])) >> 10;
				printk("%lld\n", time[j]);
			}
			average = average/(NUM_MSGS*MAX_NUM_CHANNELS);
			printk("Average time for sending msg = %lld\n", average); 
		}
#else
		if (atomic_read(&exec_count) == (NUM_MSGS*MAX_NUM_CHANNELS)) {
		//	end = ktime_get();

		//	average = ktime_to_ns(ktime_sub(end,start)) >> 10;		
		//	average = average/(NUM_MSGS*MAX_NUM_CHANNELS);

//			printk("Average time for sending msg = %lld, payload size = %d\n", average, paysize); 

		}
#endif
#endif /*TEST_MSG_LAYER*/ 
	}

#if TEST_MSG_LAYER
	while (1) {
		msleep(10);
		if (kthread_should_stop()) {
			printk("coming out of recv thread\n");
			return 0;
		}
	}
#endif /*TEST_MSG_LAYER*/

	return status;
}

#if TEST_MSG_LAYER
int pcn_kmsg_register_callback(enum pcn_kmsg_type type, pcn_kmsg_cbftn callback)
{
	if (type >= PCN_KMSG_TYPE_MAX) 
		return -1; //invalid type

	printk("%s: registering %d \n",__func__, type);
	callbacks[type] = callback;
	return 0;
}

int pcn_kmsg_unregister_callback(enum pcn_kmsg_type type)
{
	if (type >= PCN_KMSG_TYPE_MAX) 
		return -1;

	printk("Unregistering callback %d\n", type);
	callbacks[type] = NULL;
	return 0;
}
#endif

int pci_kmsg_send_long(unsigned int dest_cpu, struct pcn_kmsg_long_message *lmsg, unsigned int payload_size)
{
	int i = 0, retry = 0;
	send_wait *send_data = NULL;
	struct pcn_kmsg_long_message *pcn_msg = NULL;
	pcn_kmsg_cbftn ftn;

	if (pcn_connection_status() != PCN_CONN_CONNECTED) {
		printk("PCN_CONNECTION is not yet established\n");
		return -1;
	}

	lmsg->hdr.from_cpu = my_cpu;
	lmsg->hdr.size = (payload_size + sizeof(struct pcn_kmsg_hdr));

	if ( lmsg->hdr.size > segsize) {
		printk("%s: ALERT: trying to send a message bigger than the supported size %d (%pS) %s\n",
				__func__, segsize, __builtin_return_address(0), msg_names[lmsg->hdr.type]);
	}

#if !TEST_MSG_LAYER
	if (dest_cpu==my_cpu) {	
		pcn_msg = lmsg;

		printk("%s: INFO: Send message: dest_cpu == my_cpu\n", __func__);

		if (pcn_msg->hdr.type < 0 || pcn_msg->hdr.type >= PCN_KMSG_TYPE_MAX) {
			printk(KERN_ERR"Received invalid message type %d\n", pcn_msg->hdr.type);
			vfree(pcn_msg);
		}
		else {
			ftn = callbacks[pcn_msg->hdr.type];
			if (ftn != NULL) {
				ftn(pcn_msg);
			}
			else {
				printk(KERN_ERR"%s: ERROR: Recieved message type %d size %d has no registered callback!\n",
						__func__, pcn_msg->hdr.type,pcn_msg->hdr.size);
				vfree(pcn_msg);
			}
		}
		return lmsg->hdr.size;
	}
#endif

	send_data = kmalloc(sizeof(send_wait),GFP_ATOMIC); // TODO use a cache
	if (send_data == NULL) {
		printk("%s: ERROR: Failed to allocate send data kmalloc\n", __func__);
		return -1;
	}

	down_interruptible(&pool_buf_cnt); // released in the sender thread, it blocks all possible other senders

do_retry:
	for (i = 0; i<MAX_NUM_BUF; i++) {
		if ( atomic_cmpxchg( ((atomic_t *) &send_buf[i].is_free), 1, 0) == 1 ) {
			smp_wmb();
			break;
		}
		/*if (send_buf[i].is_free != 0) { //this is not atomic
			send_buf[i].is_free = 0;
			smp_wmb();
			break;
		} */
	}

	if (i == MAX_NUM_BUF) {
		if ( !(retry % 1000) )
			printk("%s: WARN: Couldnt find a free buffer. Retry %d\n", __func__, retry);
		if ( !(retry % 10000) )
			for (i=0; i<MAX_NUM_BUF; i++)
				printk("%s: WARN: i %d is_free %d buff 0x%lx status %d\n",
						__func__, i, send_buf[i].is_free, send_buf[i].buff, send_buf[i].status);
		retry++;
		goto do_retry;
       }

	send_data->assoc_buf = &send_buf[i];
	send_data->assoc_buf->status = 0;
	send_data->msg=send_buf[i].buff;

	memset(send_buf[i].buff, 0, segsize); // NOTE probably not needed
	memcpy(send_data->msg,lmsg,lmsg->hdr.size);
	send_data->dst_cpu = dest_cpu;
        index = index%MAX_NUM_CHANNELS;

        if (n < MAX_NUM_CHANNELS)
        {
            n++;
        }
        else
        {
            
            wait_for_completion(&dma_completion);
            send_process(send_data, xa);
            n--;
            
        }
  
        if (n >0)
        {
            
            send_process(send_data, index);
            n--;
        }
        else
        {
            printk("No data to send\n");
        }
        
#if SEND_QUEUE_POOL
	channel_select = atomic_inc_return(&send_channel)%MAX_NUM_CHANNELS;
	enq_send(send_data, channel_select);
#else
	 index++;
#endif

	return 1;
}


static int pcie_send_init(int channel_num)
{
	int status = 0, value = 0;
	unsigned int local_segid = 0, remote_segid = 0;

	status = sci_bind (&send_binding[channel_num]);
	if (status != 0) {
		printk(" Error in sci_bind: %d\n", status);
	}

	/* Query node ID */
	status = sci_query_adapter_number(local_adapter_number, Q_ADAPTER_NODE_ID,
					NO_FLAGS, &value);
	if (status != 0) {
                printk(" Send Error in sci_create_segment: %d\n", status);
        } else {
		printk(" Send adapter number = %d\n", value);
	}

	local_segid = (value << 8) + TARGET_NODE + SEND_OFFSET + channel_num;
	remote_segid = (TARGET_NODE << 8) + value + RECV_OFFSET + channel_num;

	printk(" Segid: Local - %d, remote - %d\n", local_segid, remote_segid);

	/* Create a local segment with above ID */
	status = sci_create_segment( send_binding[channel_num], module_id, local_segid,
                   NO_FLAGS, segsize, local_cbfunc, NULL, &local_send_seg_hdl[channel_num]);
	if (status != 0) {
		printk("Send Error in sci_create_segment: %d\n", status);
	}

	status = sci_export_segment(local_send_seg_hdl[channel_num], 
					local_adapter_number,
                   			NO_FLAGS);
	if (status != 0) {
		printk(" send Error in sci_export_segment: %d\n", status);
	}

	send_vaddr[channel_num] = (vkaddr_t *) sci_local_kernel_virtual_address (local_send_seg_hdl[channel_num]);
	if (send_vaddr != NULL)
	{
		printk(" local segment kernel virtual address is: %p\n", send_vaddr[channel_num]);
	}

	status = sci_set_local_segment_available (local_send_seg_hdl[channel_num],
                                 	local_adapter_number);
	if (status != 0)
	{
		printk(" send error in sci_set_local_segment_available: %d\n", status);
	}

	status = sci_probe_node(module_id, NO_FLAGS, TARGET_NODE,
               local_adapter_number, &send_report);
        if (status != 0)
        {
                printk(" send Error in sci_set_local_segment_available: %d\n", status);
        }
	else
		printk("send probe status = %d\n", send_report);


	/* Create and initialize iterrupt */
	status = sci_allocate_interrupt_flag(send_binding[channel_num], local_adapter_number,
                                  0, NO_FLAGS, send_intr_cb ,
                                  NULL, &local_send_intr_hdl[channel_num]);
        if (status != 0)
        {
                printk(" send Local interrupt cannot be created %d\n", status);
        }

	local_send_intr_no[channel_num] = sci_interrupt_number(local_send_intr_hdl[channel_num]);
	printk("Send Local interrupt number = %d\n", local_send_intr_no);

 
	status = sci_is_local_segment_available(local_send_seg_hdl[channel_num],
						local_adapter_number);
	if (status == 0)
	{
		printk(" Local segment not available to connect to\n");
	}


	do{
		status = sci_connect_segment(send_binding[channel_num], TARGET_NODE, local_adapter_number,
                    module_id, remote_segid, NO_FLAGS, send_connect_cbfunc , NULL,
                    &remote_recv_seg_hdl[channel_num]);
		if (status != 0)
		{
			msleep(1000);
			printk(" Error in sci_connect_segment: %d\n", status);
		}
		msleep(1000);
	}
	while (send_connected_flag[channel_num] == 0);

	/* Resetting connect flag */
	send_connected_flag[channel_num] = 0;

	status = sci_map_segment(remote_recv_seg_hdl[channel_num], NO_FLAGS,
                0, segsize, &send_map_handle[channel_num]);
	if (status != 0)
	{
		printk(" Error in sci_map_segment: %d\n", status);
	}

 	send_remote_vaddr[channel_num] = sci_kernel_virtual_address_of_mapping(send_map_handle[channel_num]);
	if (send_remote_vaddr != NULL)
	{
		printk(" Remote virtual address: %x\n", send_remote_vaddr);
	}

	*send_remote_vaddr[channel_num] = local_send_intr_no[channel_num];

	printk("Remote memory value for send interrupt number = %d\n", *send_remote_vaddr[channel_num]);

	while (*send_vaddr[channel_num] == 0)
	{
		msleep(100);
	}

	remote_recv_intr_no[channel_num] = *send_vaddr[channel_num];

	printk("send triggered interrupt number = %d\n", remote_recv_intr_no[channel_num]);
	status = sci_connect_interrupt_flag(send_binding[channel_num], TARGET_NODE,
                                        local_adapter_number, remote_recv_intr_no[channel_num],
                                        NO_FLAGS, &remote_recv_intr_hdl[channel_num]);
        if (status != 0)
        {
               	printk("send Unable to connect to remote interrupt: %d\n", status);
        }

	return status;
}
 

static int pcie_recv_init(int channel_num)
{
	int status = 0, value = 0;
	unsigned int local_segid = 0, remote_segid = 0;

	status = sci_bind (&recv_binding[channel_num]);
	if (status != 0)
	{
		printk(" Error in sci_bind: %d\n", status);
	}

	/* Query node ID */
	status = sci_query_adapter_number(local_adapter_number, Q_ADAPTER_NODE_ID,
					NO_FLAGS, &value);
	if (status != 0)
        {
                printk("recv Error in sci_query_adapter_number: %d\n", status);
        }
	else
		printk("recv adapter number = %d\n", value);

	local_segid = (value << 8) + TARGET_NODE + RECV_OFFSET + channel_num;
	remote_segid = (TARGET_NODE << 8) + value + SEND_OFFSET + channel_num;

	printk(" Segid: Local - %d, remote - %d\n", local_segid, remote_segid);

	/* Create a local segment with above ID */
	status = sci_create_segment( recv_binding[channel_num], module_id, local_segid,
                   NO_FLAGS, segsize, local_cbfunc, NULL, &local_recv_seg_hdl[channel_num]);
	if (status != 0)
	{
		printk(" Recv Error in sci_create_segment: %d\n", status);
	}

	status = sci_export_segment(local_recv_seg_hdl[channel_num], 
					local_adapter_number,
                   			NO_FLAGS);
	if (status != 0)
	{
		printk("recv Error in sci_export_segment: %d\n", status);
	}

	recv_vaddr[channel_num] = (vkaddr_t *) sci_local_kernel_virtual_address (local_recv_seg_hdl[channel_num]);
	if (send_vaddr != NULL)
	{
		printk(" local segment kernel virtual address is: %x\n", recv_vaddr);
	}

	status = sci_set_local_segment_available (local_recv_seg_hdl[channel_num],
                                 	local_adapter_number);
	if (status != 0)
	{
		printk("Recv Error in sci_set_local_segment_available: %d\n", status);
	}

	status = sci_probe_node(module_id, NO_FLAGS, TARGET_NODE,
               local_adapter_number, &recv_report);
        if (status != 0)
        {
                printk(" recv Error in sci_set_local_segment_available: %d\n", status);
        }
	else
		printk("recv probe status = %d\n", recv_report);


	/* Create and initialize iterrupt */
	status = sci_allocate_interrupt_flag(recv_binding[channel_num], local_adapter_number,
                                  0, NO_FLAGS, recv_intr_cb ,
                                  NULL, &local_recv_intr_hdl[channel_num]);
        if (status != 0)
        {
                printk(" recv Local interrupt cannot be created %d\n", status);
        }

	local_recv_intr_no[channel_num] = sci_interrupt_number(local_recv_intr_hdl[channel_num]);
	printk("Recv Local interrupt number = %d\n", local_recv_intr_no[channel_num]);

 
	status = sci_is_local_segment_available(local_recv_seg_hdl[channel_num],
						local_adapter_number);
	if (status != 0)
	{
		printk(" recv Local segment not available to connect to\n");
	}

	do{
		status = sci_connect_segment(recv_binding[channel_num], TARGET_NODE, local_adapter_number,
                    module_id, remote_segid, NO_FLAGS, recv_connect_cbfunc , NULL,
                    &remote_send_seg_hdl[channel_num]);
		if (status != 0)
		{
			msleep(1000);
			printk(" Recv  Error in sci_connect_segment: %d\n", status);
		}
		msleep(1000);
	}
	while (recv_connected_flag[channel_num] == 0);

	/*resetting connection flag */
	recv_connected_flag[channel_num] = 0;

	status = sci_map_segment(remote_send_seg_hdl[channel_num], NO_FLAGS,
                0, segsize, &recv_map_handle[channel_num]);
	if (status != 0)
	{
		printk("recv Error in sci_map_segment: %d\n", status);
	}

 	recv_remote_vaddr[channel_num] = sci_kernel_virtual_address_of_mapping(recv_map_handle[channel_num]);
	if (recv_remote_vaddr != NULL)
	{
		printk(" Remote virtual address: %x\n", recv_remote_vaddr[channel_num]);
	}

	*recv_remote_vaddr[channel_num] = local_recv_intr_no[channel_num];

	printk(" Recv  interrupt number = %d\n", *recv_remote_vaddr[channel_num]);

	while (*recv_vaddr[channel_num] == 0) {
		msleep(100);
	}

	remote_send_intr_no[channel_num] = *recv_vaddr[channel_num];

	printk(" Recv triggered interrupt number = %d\n", remote_send_intr_no[channel_num]);
	status = sci_connect_interrupt_flag(recv_binding[channel_num], TARGET_NODE,
                                        local_adapter_number, remote_send_intr_no[channel_num],
                                        NO_FLAGS, &remote_send_intr_hdl[channel_num]);
        if (status == 0) {
               	printk("Recv Unable to connect to remote interrupt: %d\n", status);
        }

	return status;
}

static int pcie_send_cleanup(int channel_num)
{
	int status = 0;

        /* Remove interrupt */
        status = sci_disconnect_interrupt_flag(&remote_recv_intr_hdl[channel_num], NO_FLAGS);
        if (status != 0) {
                printk(" Error in sci_disconnect_interrupt_flag: %d\n", status);
        }

	/* Deintialize path */
	status = sci_unmap_segment(&send_map_handle[channel_num],NO_FLAGS);
	if (status != 0) {
		printk(" Error in sci_unmap_segment: %d\n", status);
	}

	status = sci_disconnect_segment(&remote_recv_seg_hdl[channel_num], NO_FLAGS);
	if (status != 0) {
		printk(" Error in sci_disconnect_segment: %d\n", status);
	}

	status = sci_remove_interrupt_flag(&local_send_intr_hdl[channel_num], NO_FLAGS);
        if (status != 0) {
                printk(" Error in sci_remove_interrupt_flag: %d\n", status);
        }

	status = sci_set_local_segment_unavailable (local_send_seg_hdl[channel_num],
						 local_adapter_number);
	if (status != 0) {
		printk(" Error in sci_set_local_segment_unavailable: %d\n", status);
	}

	status = sci_unexport_segment(local_send_seg_hdl[channel_num], local_adapter_number,
                     			NO_FLAGS);
	if (status != 0) {
		printk(" Error in sci_unexport_segment: %d\n", status);
	}

	status = sci_remove_segment(&local_send_seg_hdl[channel_num],NO_FLAGS);
	if (status != 0) {
		printk(" Error in sci_remove_segment: %d\n", status);
	}

	status = sci_unbind (&send_binding[channel_num]);
	if (status != 0) {
		printk(" Error in sci_bind: %d\n", status);
	}

	return status;
}

static int pcie_recv_cleanup(int channel_num)
{
	int status = 0;

        /* Remove interrupt */
        status = sci_disconnect_interrupt_flag(&remote_send_intr_hdl[channel_num], NO_FLAGS);
        if (status != 0) {
                printk(" Error in sci_disconnect_interrupt_flag: %d\n", status);
        }

	/* Deintialize path */
	status = sci_unmap_segment(&recv_map_handle[channel_num],NO_FLAGS);
	if (status != 0) {
		printk(" Error in sci_unmap_segment: %d\n", status);
	}

	status = sci_disconnect_segment(&remote_send_seg_hdl[channel_num], NO_FLAGS);
	if (status != 0) {
		printk(" Error in sci_disconnect_segment: %d\n", status);
	}

	status = sci_remove_interrupt_flag(&local_recv_intr_hdl[channel_num], NO_FLAGS);
        if (status != 0) {
                printk(" Error in sci_remove_interrupt_flag: %d\n", status);
        }

	status = sci_set_local_segment_unavailable (local_recv_seg_hdl[channel_num],
						 local_adapter_number);
	if (status != 0) {
		printk(" Error in sci_set_local_segment_unavailable: %d\n", status);
	}

	status = sci_unexport_segment(local_recv_seg_hdl[channel_num], local_adapter_number,
                     			NO_FLAGS);
	if (status != 0) {
		printk(" Error in sci_unexport_segment: %d\n", status);
	}

	status = sci_remove_segment(&local_recv_seg_hdl[channel_num],NO_FLAGS);
	if (status != 0) {
		printk(" Error in sci_remove_segment: %d\n", status);
	}

	status = sci_bind (&recv_binding[channel_num]);
	if (status != 0) {
		printk(" Error in sci_bind: %d\n", status);
	}

	return status;
}

#if ENABLE_DMA
static int dma_init(int channel_num)
{
	int status = 0;

	status = sci_create_dma_queue (send_binding[channel_num], &dma_queue[channel_num], local_adapter_number,
                 			10, segsize, NO_FLAGS);
	if (status != 0) {
		printk(" Error in sci_create_dma_queue: %d\n", status);
	}

	local_io[channel_num] = sci_local_io_addr(local_send_seg_hdl[channel_num], local_adapter_number);
	if (status != 0) {
		printk(" Error in sci_map_dma_buffer: %d\n", status);
	}

	status = dis_register_dma_cb(subuser_id[channel_num], dma_cb);
	if (status != 0) {
		printk(" Error in dis_register_dma_cb: %d\n", status);
	}

	status = dis_get_dma_state(local_adapter_number);
	printk(" DMA state: %d\n", status);

	return status;
}

static int dma_cleanup(int channel_num)
{
	int status = 0;

	status = sci_free_dma_queue (&dma_queue[channel_num], NO_FLAGS);
	if (status != 0) {
		printk(" Error in sci_free_dma_queue: %d\n", status);
	}

	return status;
}
#endif



static void __exit unload(void)
{
	int status = 0, i = 0, j = 0, f=0;

	printk("Stopping kernel threads\n");

	/* To move out of recv_queue*/
	for (i = 0; i<MAX_NUM_CHANNELS; i++) {

#if SEND_QUEUE_POOL
		complete_all(&send_q_empty[i]);
#else
		complete_all(&send_q_empty);
#endif		
		complete_all(&recv_intr_flag[i]);
	}

	msleep(100);

	for (i = 0; i<MAX_NUM_CHANNELS; i++) {
		kthread_stop(sender_handler[i]);
		kthread_stop(handler[i]);

		for (j = 0; j<(RECV_THREAD_POOL-1); j++) {
			kthread_stop(pool_thread_handler[i+j]);
		}
	}

	/*cleanup*/
	for (i = 0; i<MAX_NUM_CHANNELS; i++) {

#if ENABLE_DMA
		status = dma_cleanup(i);
#endif
		status = pcie_send_cleanup(i);
		status = pcie_recv_cleanup(i);
	}

	for (i = 0; i<MAX_NUM_BUF; i++) {
		vfree(send_buf[i].buff);

#if TEST_MSG_LAYER
		vfree(recv_buf[i].buff);
#endif

	}
        for(f =0; f < 25; f++)
        {
             #if TEST_SERVER
             int average = (ktime_to_ns(ktime_sub(dqend[f],dqstart[f])));              
                     

                 printk("dq time for sending msg = %lld ns, payload size = %d  rount: %d\n", average, paysize, f);
                 
                 average = (ktime_to_ns(ktime_sub(sendrend[f],sendrstart[f])))/1000;

                 printk("wait interrupt for sendmsg = %lld us, payload size = %d  rount: %d\n", average, paysize, f);
                      
                 average = (ktime_to_ns(ktime_sub(eqend[f],eqstart[f])))/1000;

                 printk("dma transfer time for sendmsg = %lld us, payload size = %d  rount: %d\n", average, paysize, f);
  
                 average = (ktime_to_ns(ktime_sub(pciend[f],pcistart[f])))/1000;

                 printk("send pci time  = %lld us, payload size = %d  rount: %d\n", average, paysize, f);

                 average = (ktime_to_ns(ktime_sub(trigend[f], trigstart[f])));

                 printk("send trigger interrupt time  = %lld ns, payload size = %d  rount: %d\n", average, paysize, f);


             #endif
             #if !TEST_SERVER
             int average = (ktime_to_ns(ktime_sub(readend[f],readstart[f])));


                 printk("recv read data time = %lld ns, payload size = %d  rount: %d\n", average, paysize, f);

                 average = (ktime_to_ns(ktime_sub(recvend[f],recvstart[f])))/1000;

                 printk("wait interrupt for recvmsg = %lld us, payload size = %d  rount: %d\n", average, paysize, f);




                 average = (ktime_to_ns(ktime_sub(recvtrigend[f], recvtrigstart[f])));

                 printk("recv trigger interrupt time  = %lld ns, payload size = %d  rount: %d\n", average, paysize, f);


             #endif

        } 
	sci_terminate(module_id);	

	printk("Successfully unloaded module\n");
}

module_init(initialize);
module_exit(unload);
MODULE_LICENSE("GPL");
