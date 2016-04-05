#define LINUX

#include "mp3_given.h"
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/timer.h>
#include <linux/proc_fs.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <asm/uaccess.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/kthread.h>
#include <linux/spinlock.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/workqueue.h>
#include <linux/vmalloc.h>
#include <linux/mm.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("23");
MODULE_DESCRIPTION("CS-423 MP3");

#define DEBUG 1
#define FILENAME "status"
#define DIRECTORY "mp3"
#define MAX_BUF_SIZE 128
#define SLEEPING_STATE 0
#define READY_STATE 1
#define RUNNING_STATE 2
#define TOTAL_PAGE_NUM 128

static void _measure_info_worker(struct work_struct *);
void _delayed_workqueue_init(void);
// A self-defined structure represents PCB
// Index by pid, used as a node in the task linked list
typedef struct mp3_task_struct {
	struct task_struct* linux_task;
	pid_t pid;
    struct list_head process_node;
    unsigned long min_flt;
    unsigned long maj_flt;
    unsigned long cpu_utilization;	
} task_node_t;

static struct proc_dir_entry *proc_dir;
static struct proc_dir_entry *proc_entry;
static struct mutex my_mutex;
static struct kmem_cache *task_cache;
static struct workqueue_struct * delayed_workqueue;
struct delayed_work *measure_info_obj;
unsigned long * shared_mem_buffer;
unsigned int buffer_iterator;
static struct cdev *my_cdev;
static dev_t my_cdev_num;

LIST_HEAD(taskList);

void update_process_info(void)
{
    task_node_t *entry;
	struct list_head *pos;
    unsigned long utime, stime;

	mutex_lock(&my_mutex);
    list_for_each(pos, &taskList) {
        entry = list_entry(pos, task_node_t, process_node);
        get_cpu_use(entry->pid, &(entry->min_flt), &(entry->maj_flt), &utime, &stime);
        entry->cpu_utilization = utime+stime;   
    }
    mutex_unlock(&my_mutex);
}

void write_process_to_shared_mem_buffer(void)
{
    task_node_t *entry;
    struct list_head *pos;
    unsigned long maj_flt_count, min_flt_count, cpu_utilization_count;
  
    maj_flt_count = 0;
    min_flt_count = 0;
	cpu_utilization_count = 0;
    
    mutex_lock(&my_mutex);
    list_for_each(pos, &taskList) {
		entry = list_entry(pos, task_node_t, process_node);
        maj_flt_count += entry->maj_flt;
		min_flt_count += entry->min_flt;
		cpu_utilization_count += entry->cpu_utilization;
	}
	mutex_unlock(&my_mutex);

	shared_mem_buffer[buffer_iterator++] = jiffies;
	shared_mem_buffer[buffer_iterator++] = min_flt_count;
	shared_mem_buffer[buffer_iterator++] = maj_flt_count;
	shared_mem_buffer[buffer_iterator++] = cpu_utilization_count;

	buffer_iterator = (buffer_iterator+4) % (TOTAL_PAGE_NUM*PAGE_SIZE/(sizeof (unsigned long)));
}

static void _measure_info_worker(struct work_struct * measure_into_obj)
{
	update_process_info();
	write_process_to_shared_mem_buffer();
    _delayed_workqueue_init();
}

void _delayed_workqueue_init(void)
{
    if(list_empty(&taskList))
    {
        measure_info_obj = (struct delayed_work *)kmalloc(sizeof(struct delayed_work), GFP_ATOMIC);
    }
    INIT_DELAYED_WORK((struct delayed_work *)measure_info_obj, _measure_info_worker);
    queue_delayed_work(delayed_workqueue, measure_info_obj, msecs_to_jiffies(50));
}

// 3.unregister: D,PID
// Called when user application use "cat" or "fopen"
// The function read the status file and print the information related out
static ssize_t mp3_read(struct file *file, char __user * buffer, size_t count, loff_t * data)
{
    size_t copied = 0;
    char * buf = NULL;
    struct list_head *pos = NULL;
    task_node_t *tmp = NULL;
    char currData[MAX_BUF_SIZE];
    int currByte;
    buf = (char*) kmalloc(1024, GFP_KERNEL);
    
    // read each node on the list and print the information as [pid: period, proc_time] to user
	mutex_lock(&my_mutex);
    list_for_each(pos, &taskList) {
        tmp = list_entry(pos, task_node_t, process_node);
        memset(currData, 0, MAX_BUF_SIZE);
        currByte = sprintf(currData, "%u\n", tmp->pid);
        strcat(buf, currData);
        copied += currByte;
    }
    mutex_unlock(&my_mutex);
    
    if(*data>0)
    {
        return 0;
    }
    copy_to_user(buffer, buf, copied);
    kfree(buf);
    *data += copied;

    return copied;
	
}


// Called when a new self-defined task node is allocated
// Store user input, set task state and create timer for it
static void init_node(task_node_t* new_task, char* buf) 
{
    // set up member variables
	sscanf(buf, "%u", &(new_task->pid));
    new_task -> linux_task = find_task_by_pid(new_task->pid);
}

// Add a newly created task node into the existing task linked list
// Ordered bt task period (shortest period first)
static int add_to_list(char *buf)
{
	task_node_t *new_task = kmem_cache_alloc(task_cache, GFP_KERNEL);

	init_node(new_task, buf);

	mutex_lock(&my_mutex);
	list_add_tail(&(new_task->process_node), &taskList);	
	mutex_unlock(&my_mutex);
	return -1;
}

// Free a allocated task node, remove it from the list
static void destruct_node(struct list_head *pos)
{
	task_node_t *entry;

	mutex_lock(&my_mutex);
	entry = list_entry(pos, task_node_t, process_node);
	printk(KERN_ALERT "START DESTRUCT TASK: %u",entry->pid);

    // if the current running task would like to unregister itself,
    // set current_running_task to NULL
	kmem_cache_free(task_cache, entry);
	mutex_unlock(&my_mutex);
}

// Helper function that traverse the entire task linked list and 
// find a task according to its pid
static struct list_head *find_task_node_by_pid(char *pid)
{
    struct list_head *pos;
    struct list_head *next;
    task_node_t *curr;
    char curr_pid[20];

    mutex_lock(&my_mutex);

    list_for_each_safe(pos, next, &taskList){
        curr = list_entry(pos, task_node_t, process_node);
        memset(curr_pid, 0, 20);
        sprintf(curr_pid, "%u", curr->pid);
        if(strcmp(curr_pid, pid)==0)
        {
            mutex_unlock(&my_mutex);
            return pos;
        }
    }

    mutex_unlock(&my_mutex);
    return NULL;
}

static int reg(char *buf){
    _delayed_workqueue_init();
    return add_to_list(buf);
}

static void dereg(char *buf){
    struct list_head *pos;
    pos = find_task_node_by_pid(buf);
    destruct_node(pos);

    //if the PCB list is empty, delete the work queue
    if(list_empty(&taskList))
    {
        kfree(measure_info_obj);
        measure_info_obj = NULL;
    }
}

// Called when user application registered a process
// The function get the pid from the user and add/remove it on/from the linked list
static ssize_t mp3_write(struct file *file, const char __user *buffer, size_t count, loff_t * data){
	char * buf = (char*)kmalloc(count+1, GFP_KERNEL);
	int ret = -1;
    
	if (count > MAX_BUF_SIZE - 1) {
		count = MAX_BUF_SIZE - 1;
	}

	copy_from_user(buf, buffer, count);
	buf[count] = '\0';

	printk(KERN_ALERT "MP3_WRITE CALLED, INPUT:%s\n", buf);
	
	// Check the starting char of buf, if:
	// 1.register: R PID
	if (buf[0] == 'R') {
		ret = reg(buf+2);
		printk(KERN_ALERT "REGISTERED PID:%s", buf+2);
	}
	else if (buf[0] == 'U') {
	// 2.unregister: U PID
        dereg(buf+2);
		printk(KERN_ALERT "UNREGISTERED PID: %s", buf+2);
	}
	else {
		kfree(buf);
		return 0;
	}
	kfree(buf);
	return ret;
}

static const struct file_operations mp3_file = {
    .owner = THIS_MODULE,
    .read = mp3_read,
    .write = mp3_write,
};

static int cdev_open(struct inode * id, struct file *f)
{
	printk(KERN_ALERT "char dev open\n");
	return 0;
}

static int cdev_release(struct inode * id, struct file *f)
{
	printk(KERN_ALERT "char dev release\n");
	return 0;
}

static int cdev_mmap(struct file *f, struct vm_area_struct *vma)
{
	unsigned long length = TOTAL_PAGE_NUM*PAGE_SIZE;
	unsigned long *vmalloc_area_ptr = shared_mem_buffer;
	unsigned long start = vma->vm_start;
	while (length > 0) {
		unsigned long pfn = vmalloc_to_pfn(vmalloc_area_ptr);
		remap_pfn_range(vma, start, pfn, PAGE_SIZE, vma->vm_page_prot);
		start += PAGE_SIZE;
		vmalloc_area_ptr += PAGE_SIZE/(sizeof (unsigned long));
		length -= PAGE_SIZE;
	}
	return 0;
}

static const struct file_operations cdev_ops = {
    .owner = THIS_MODULE,
    .open = cdev_open,
    .mmap = cdev_mmap,
    .release = cdev_release
};

void init_cdev(void)
{
	alloc_chrdev_region(&my_cdev_num, 0, 1, "mp3_cdev");	
	my_cdev = cdev_alloc();
	cdev_init(my_cdev, &cdev_ops);
	cdev_add(my_cdev, my_cdev_num, 1);
}

// mp3_init - Called when module is loaded
int __init mp3_init(void)
{
    #ifdef DEBUG
    printk(KERN_ALERT "MP3 MODULE LOADING\n");
    #endif
    // create proc directory and file entry
    proc_dir = proc_mkdir(DIRECTORY, NULL);
    proc_entry = proc_create(FILENAME, 0666, proc_dir, &mp3_file);
    
	//create workqueue
	delayed_workqueue = create_workqueue("delayed workqueue");
    
	//create the shared memory buffer
	//TODO: activate PG_reserved bit
	shared_mem_buffer = (unsigned long *)vmalloc(TOTAL_PAGE_NUM * PAGE_SIZE);    
	buffer_iterator = 0;
	init_cdev();

	// create cache for slab allocator
	task_cache = kmem_cache_create("task_cache", sizeof(task_node_t), 0, SLAB_HWCACHE_ALIGN, NULL);

    // init mutex lock
    mutex_init(&my_mutex);

	printk(KERN_ALERT "MP3 MODULE LOADED\n");
    return 0;
}

// mp3_exit - Called when module is unloaded
void __exit mp3_exit(void)
{
    struct list_head *pos;
    struct list_head *next;

    #ifdef DEBUG
    printk(KERN_ALERT "MP3 MODULE UNLOADING\n");
    #endif

    // remove every node on linked list and remove the list     
    list_for_each_safe(pos, next, &taskList){
		destruct_node(pos);
	}

    // remove file entry and repository  
    remove_proc_entry(FILENAME, proc_dir);
    remove_proc_entry(DIRECTORY, NULL);

    flush_workqueue(delayed_workqueue);
    destroy_workqueue(delayed_workqueue);
   
    vfree(shared_mem_buffer);

	// destroy memory cache
	kmem_cache_destroy(task_cache);

    printk(KERN_ALERT "MP3 MODULE UNLOADED\n");
}

// Register init and exit funtions
module_init(mp3_init);
module_exit(mp3_exit);

