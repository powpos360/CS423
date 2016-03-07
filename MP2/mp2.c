#define LINUX

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/timer.h>
#include <linux/proc_fs.h>
#include <linux/workqueue.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <asm/uaccess.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/string.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("23");
MODULE_DESCRIPTION("CS-423 MP2");

#define DEBUG 1
#define FILENAME "status"
#define DIRECTORY "mp2"
#define MAX_BUF_SIZE 128

typedef struct mp2_task_struct {
	struct task_struct* linux_task;
	struct timer_list wakeup_timer;
	pid_t pid;
	// 0 = SLEEPING
	// 1 = READY
	// 2 = RUNNING
	int state;
	unsigned long proc_time;
	unsigned long period;
	struct list_head process_node;
} task_node_t;

static struct proc_dir_entry *proc_dir;
static struct proc_dir_entry *proc_entry;
static struct timer_list my_timer;
static struct mutex my_mutex;
static struct kmem_cache *task_cache;

LIST_HEAD(taskList);
task_node_t *current_running_task;

// Called when user application use "cat" or "fopen"
// The function read the status file and print the information related out
static ssize_t mp2_read(struct file *file, char __user * buffer, size_t count, loff_t * data){
    size_t copied = 0;
    char * buf = NULL;
    struct list_head *pos = NULL;
    task_node_t *tmp = NULL;
    char currData[MAX_BUF_SIZE];
    int currByte;
    buf = (char*) kmalloc(1024, GFP_KERNEL);
    //mutex_lock(&my_mutex);
    list_for_each(pos, &taskList) {
        tmp = list_entry(pos, task_node_t, process_node);
        memset(currData, 0, MAX_BUF_SIZE);
        currByte = sprintf(currData, "%u: %lu, %lu\n", tmp->pid, tmp->period, tmp->proc_time);
        strcat(buf, currData);
        copied += currByte;
    }
    //mutex_unlock(&my_mutex);

    
    if(*data>0)
    {
        return 0;
    }
    copy_to_user(buffer, buf, copied);
    kfree(buf);
    *data += copied;

    return copied;
	
}

void init_node(task_node_t* new_task, char* buf) 
{
	int i = 0;
	char *pch;
    char *dataHolder = (char*)kmalloc(strlen(buf)+1, GFP_KERNEL);
	if(dataHolder)
	{
		strcpy(dataHolder, buf);
	}
	
	pch = strsep(&dataHolder, " ");
	
	for(i = 0; i < 3 && pch!=NULL; i ++)
	{
		if(i==0)
		{
			sscanf(pch, "%u", &(new_task->pid));
		}
		else if(i==1)
		{
			sscanf(pch, "%lu", &(new_task->period));
		}
		else
		{
			sscanf(pch, "%lu", &(new_task->proc_time));
		}
		pch = strsep(&dataHolder, " ,");
	}	
	
	new_task -> state = 0;
	new_task -> linux_task = (struct task_struct*)kmalloc(sizeof(struct task_struct), GFP_KERNEL);
}


int add_to_list(char *buf)
{
	struct list_head *pos;
	task_node_t *entry;
	task_node_t *new_task = kmem_cache_alloc(task_cache, GFP_KERNEL);

	init_node(new_task, buf);

    list_for_each(pos, &taskList) {
        entry = list_entry(pos, task_node_t, process_node);
        if (entry->period > new_task->period) {
		    list_add_tail(&new_task->process_node, pos);
			return -1;
        }
    }

	list_add_tail(&(new_task->process_node), &taskList);	
	return -1;
}

void destruct_node(struct list_head *pos) {
	task_node_t *entry;
	list_del(pos);
	entry = list_entry(pos, task_node_t, process_node);
	kfree(entry->linux_task);
	kmem_cache_free(task_cache, entry);
}

int delete_from_list(char *pid)
{
    struct list_head *pos;
    struct list_head *next;
    task_node_t *curr;
	char curr_pid[20];
    mutex_lock(&my_mutex);

    list_for_each_safe(pos, next, &taskList){
        curr = list_entry(pos, task_node_t, process_node);
        memset(curr, 0, 20);
		sprintf(curr_pid, "%u", curr->pid);
		if(strcmp(curr_pid, pid)==0)
        {
			destruct_node(pos);
        }
    }

    mutex_unlock(&my_mutex);
    return -1;
}

void dispatching_thread(void)
{
	task_node_t *next_task;
	task_node_t *entry;
	task_node_t *prev_task;
	struct sched_param new_sparam; 
	struct sched_param old_sparam; 
	struct list_head *pos;
	if(current_running_task)
	{
		list_for_each(pos, &taskList) {
			entry = list_entry(pos, task_node_t, process_node);
			if (entry->period < current_running_task->period && entry->state==1) {
				next_task = entry;
				break;
			}
		}
	}
	
	prev_task = current_running_task;
	prev_task->state = 1;
	
	//old task
	old_sparam.sched_priority=0; 
	sched_setscheduler(prev_task->linux_task, SCHED_NORMAL, &old_sparam);
	
	// new task
	wake_up_process(next_task->linux_task); 
	new_sparam.sched_priority=99;
	sched_setscheduler(next_task->linux_task, SCHED_FIFO, &new_sparam);

	current_running_task = next_task;
	current_running_task->state=2;
}

int admission_control(struct file *file, const char __user *buffer, size_t count, loff_t * data)
{
    return -1;
}

// Called when user application registered a process
// The function get the pid from the user and put it on the linked list, which actually write it in the status file
static ssize_t mp2_write(struct file *file, const char __user *buffer, size_t count, loff_t * data){
	char * buf = (char*)kmalloc(count+1, GFP_KERNEL);
	int ret = -1;


	if (count > MAX_BUF_SIZE - 1) {
		count = MAX_BUF_SIZE - 1;
	}

	copy_from_user(buf, buffer, count);
	buf[count] = '\0';

	printk(KERN_ALERT "MP2_WRITE CALLED, INPUT:%s\n", buf);
	
	// Check the starting char of buf, if:
	// 1.register: R,PID,PERIOD,COMPUTATION
	if (buf[0] == 'R') {
		ret = add_to_list(buf+2);
		printk(KERN_ALERT "REGISTERED PID:%s", buf+2);
	}
	else if (buf[0] == 'Y') {
	// 2.yield: Y,PID
	}
	else if (buf[0] == 'D') {
	// 3.unregister: D,PID
		ret = delete_from_list(buf+2);
		printk(KERN_ALERT "UNREGISTERED PID: %s", buf+2);
	}
	else {
		return 0;
	}

	return ret;
}

// Get the scheduled work and work on updating the cpu use time for processes corrsponding to nodes on the linked list
static void my_worker(struct work_struct * work) {
//      unsigned long cpu_time;
/*    struct list_head *pos;
    list_node_t *tmp = NULL;
    unsigned int base = 10;
    int pid;
    printk(KERN_ALERT "my_woker func called");
    mutex_lock(&my_mutex);
    list_for_each(pos, &taskList) {
        tmp = list_entry(pos, list_node_t, node);
        kstrtoint(tmp->data, base, &pid);
        printk(KERN_ALERT "%d", pid);
     if(get_cpu_use(pid, &cpu_time) == 0) {
            // update each node
            tmp->cpu_time = cpu_time;
        }
    }
    mutex_unlock(&my_mutex);
*/
}

// Called when timer expired, this will stark the work queue and restart timer ticking
void _interrupt_handler (unsigned long arg){
    mod_timer(&my_timer, jiffies + msecs_to_jiffies(5000));
    //_update_workqueue_init();
}

// Set default member variable value for timer, start timer ticking
static void _create_my_timer(void) {
    init_timer(&my_timer);
    my_timer.data = 0;
    my_timer.expires = jiffies + msecs_to_jiffies(5000);
    my_timer.function = _interrupt_handler;
    add_timer(&my_timer);
    printk(KERN_ALERT "TIMER CREATED");
}

static const struct file_operations mp2_file = {
    .owner = THIS_MODULE,
    .read = mp2_read,
    .write = mp2_write,
};

// mp2_init - Called when module is loaded
int __init mp2_init(void)
{
    #ifdef DEBUG
    printk(KERN_ALERT "MP2 MODULE LOADING\n");
    #endif
    // create proc directory and file entry
    proc_dir = proc_mkdir(DIRECTORY, NULL);
    proc_entry = proc_create(FILENAME, 0666, proc_dir, & mp2_file);
	current_running_task = NULL;

    // create Linux Kernel Timer
    //_create_my_timer();

	// create cache for slab allocator
	task_cache = kmem_cache_create("task_cache", sizeof(task_node_t), 0, SLAB_HWCACHE_ALIGN, NULL);

    // init mutex lock
    mutex_init(&my_mutex);

    printk(KERN_ALERT "MP2 MODULE LOADED\n");
    return 0;
}

// mp2_exit - Called when module is unloaded
void __exit mp2_exit(void)
{
    struct list_head *pos;
    struct list_head *next;

    #ifdef DEBUG
    printk(KERN_ALERT "MP2 MODULE UNLOADING\n");
    #endif

    // delete timer
    del_timer(&my_timer);

    // remove every node on linked list and remove the list     
    list_for_each_safe(pos, next, &taskList){
        list_del(pos);
    	kmem_cache_free(task_cache, list_entry(pos, task_node_t, process_node));
	}

    // remove file entry and repository  
    remove_proc_entry(FILENAME, proc_dir);
    remove_proc_entry(DIRECTORY, NULL);

    printk(KERN_ALERT "MP2 MODULE UNLOADED\n");
}

// Register init and exit funtions
module_init(mp2_init);
module_exit(mp2_exit);

