#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/moduleparam.h>
#include <linux/stat.h>
#include <linux/fs.h>
#include <linux/errno.h>
#include <linux/mutex.h>
#include <linux/list.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/workqueue.h>
#include <linux/param.h>
#include <linux/wait.h>
#include <linux/version.h>
#include "timed_messaging_system.h"


#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 0, 0)
#define get_minor(filep) iminor(filep->f_inode)
#else
#define get_minor(filep) iminor(filep->f_entry->d_inode)
#endif

MODULE_AUTHOR("Luca Tabacchino");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("This module provides a device file that allows timed messaging");

static int major;

//Module params
static unsigned int maxMessageSize = 512;
module_param(maxMessageSize,uint,0660);
static unsigned int maxStorageSize = 4096;
module_param(maxStorageSize,uint,0660);

static struct minorStruct minors[MINORS]; 

static int dev_open(struct inode *inode, struct file *file){

	struct sessionStruct *sessionStruct;
	int minor;
	
	minor = get_minor(file);
	
	if(minor >= MINORS){
		return -ENODEV; //No such device
	}

	//Init sessionStruct object
	sessionStruct = kmalloc(sizeof(struct sessionStruct), GFP_KERNEL);
	if (sessionStruct == NULL){
		printk(KERN_INFO "Error in kmalloc \n");
		return -ENOMEM; //Cannot allocate memory
	}

	mutex_init(&(sessionStruct->mtx));
	sessionStruct->writeWorkqueue = alloc_workqueue(WRITE_WORK_QUEUE, WQ_MEM_RECLAIM,0);
	if (sessionStruct->writeWorkqueue == NULL){
		printk(KERN_INFO "Error in alloc_workqueue \n");
		kfree(sessionStruct);
		return -ENOMEM; //Cannot allocate memory
	}

	sessionStruct->writeTimeout = 0;
	sessionStruct->readTimeout = 0;
	INIT_LIST_HEAD(&(sessionStruct->pendingWrites));
	INIT_LIST_HEAD(&(sessionStruct->list));
	// Link session_struct object to struct file via private_data field
	file->private_data = (void *) sessionStruct;
	
	// sessionStruct object is added to list of opened sessions.
	mutex_lock(&(minors[minor].mtx));
	list_add_tail(&(sessionStruct->list), &(minors[minor].sessions));
	mutex_unlock(&(minors[minor].mtx));
	return 0;
}

static ssize_t deliver_message(char *userBuff, int minor, struct messageStruct *messageStruct, ssize_t len){

	int notCopied;
	
	if (len > messageStruct->size) {
		len = messageStruct->size;
	}
	
	notCopied = copy_to_user(userBuff, messageStruct->buff, len); 

	list_del(&(messageStruct->list));
	minors[minor].currentSize -= messageStruct->size;
	mutex_unlock(&(minors[minor].mtx));
	kfree(messageStruct->buff);
	kfree(messageStruct);
	return len - notCopied; //number of copied bytes
}

static ssize_t dev_read(struct file *file, char *buff, size_t len, loff_t * off){

	int minor, ret;
	struct messageStruct *messageStruct;
	struct sessionStruct *session;
	struct pendingReadStruct *pendingRead;
	unsigned long readTimeout, sleepTime;

	session = (struct sessionStruct *)file->private_data;
	minor = get_minor(file);

	mutex_lock(&(minors[minor].mtx));

	// Get first message stored in the device file 
	messageStruct = list_first_entry_or_null(&(minors[minor].messageFifoOrder), struct messageStruct, list);

	// Deliver message if it is available
	if (messageStruct != NULL) {
		ret = deliver_message(buff, minor, messageStruct, len);
		return ret;	
	}

	// Message not available (empty queue)
	mutex_unlock(&(minors[minor].mtx));
	mutex_lock(&(session->mtx));
	readTimeout = session->readTimeout;
	mutex_unlock(&(session->mtx));
	
	//Non blocking read
	if (!readTimeout) {
		return -ENOMSG; //No message of desired type
	}

	// Blocking read
	sleepTime = readTimeout;
	// Allocate pending_read_struct 
	pendingRead = kmalloc(sizeof(struct pendingReadStruct), GFP_KERNEL);
	if (pendingRead == NULL) {
		return -ENOMEM; //Cannot allocate memory
	}
	// Initit pending_read_struct 
	pendingRead->availableMessage = 0;
	pendingRead->flushing = 0;
	INIT_LIST_HEAD(&(pendingRead->list));
	mutex_lock(&(minors[minor].mtx));
	
	//Enqueue pending read to others
	list_add_tail(&(pendingRead->list), &(minors[minor].pendingReads));
	mutex_unlock(&(minors[minor].mtx));

	// sleep waiting available message
	while (sleepTime) {
		ret =
		    wait_event_interruptible_timeout(minors[minor].readWorkqueue,
						     pendingRead->availableMessage || pendingRead->flushing,
						     sleepTime);
		if (ret == -ERESTARTSYS) {	//interrupted by a signal
			//remove pending read
			if (pendingRead->availableMessage || pendingRead->flushing) {
				kfree(pendingRead); 
				return ret;
			} else{
				mutex_lock(&(minors[minor].mtx));
				list_del(&(pendingRead->list));
				mutex_unlock(&(minors[minor].mtx));
				kfree(pendingRead);
				return ret;
			}
		}
		
		// After time expiration queue is empty
		if (ret == 0) {
			ret = -ETIME; //Timer expired
			//remove pending read
			mutex_lock(&(minors[minor].mtx));
			list_del(&(pendingRead->list));
			mutex_unlock(&(minors[minor].mtx));
			kfree(pendingRead);
			return ret;
		}
		
		// Invoked dev_flush
		if (pendingRead->flushing) {	
			ret = -ECANCELED; //Operation canceled
			//free pendingRead struct
			kfree(pendingRead);
			return ret;
		}
		
		// Check if message is available

		// Check if list is not empty due a concurrency 
		mutex_lock(&(minors[minor].mtx));
		messageStruct = list_first_entry_or_null(&(minors[minor].messageFifoOrder), struct messageStruct, list);
		// If list is empty return to sleep */
		if (messageStruct == NULL) {	
			pendingRead->availableMessage = 0;
			list_add_tail(&(pendingRead->list), &(minors[minor].pendingReads));
			mutex_unlock(&(minors[minor].mtx));
			sleepTime = ret;
		// Message available 	
		} else {	
			kfree(pendingRead);
			ret = deliver_message(buff, minor, messageStruct, len);
			return ret;
		}
	}
}

static int write_message(struct minorStruct *minor, char *kernelBuff, size_t len){

	struct messageStruct *message;

	if (minor->currentSize + len > maxStorageSize) {
		kfree(kernelBuff);
		return -ENOSPC; //No space left on device
	}
	message = kmalloc(sizeof(struct messageStruct), GFP_KERNEL);
	if (message == NULL) {
		kfree(kernelBuff);
		return -ENOMEM; //Cannot allocate memory
	}
	message->size = len;
	message->buff = kernelBuff;
	INIT_LIST_HEAD(&(message->list));
	list_add_tail(&(message->list), &(minor->messageFifoOrder));
	minor->currentSize += len;

	return len;
}

static void awake_sleeping_reader(struct minorStruct *minor){

	struct pendingReadStruct *pendingRead;

	pendingRead = list_first_entry_or_null(&(minor->pendingReads), struct pendingReadStruct, list);
	if (pendingRead != NULL) {
		list_del(&(pendingRead->list));
		pendingRead->availableMessage = 1;
		wake_up_interruptible(&(minor->readWorkqueue));
	}
	return;
}

static void write_deferred_message(struct work_struct *work_struct){

	int ret;
	struct delayed_work *delayedWork;
	struct pendingWriteStruct *pendingWriteStruct;

	delayedWork = container_of(work_struct, struct delayed_work, work);
	pendingWriteStruct = container_of(delayedWork, struct pendingWriteStruct, delayedWork);
	// dequeue from pending write list  
	mutex_lock(&(pendingWriteStruct->session->mtx));
	list_del(&(pendingWriteStruct->list));
	mutex_unlock(&(pendingWriteStruct->session->mtx));

	mutex_lock(&(minors[pendingWriteStruct->minor].mtx));
	ret = write_message(&minors[pendingWriteStruct->minor],
			     pendingWriteStruct->kernelBuff, pendingWriteStruct->len);
	//post of message success
	if (ret >= 0) {
		awake_sleeping_reader(&(minors[pendingWriteStruct->minor]));
	}
	mutex_unlock(&(minors[pendingWriteStruct->minor].mtx));

	kfree(pendingWriteStruct);
	return;
}

static ssize_t dev_write(struct file *file, const char *buff, size_t len, loff_t * off){
			 
	char *kernelBuff;
	int minor, ret, notCopied;
	struct pendingWriteStruct *pendingWriteStruct;
	struct sessionStruct *session;

	session = (struct sessionStruct *)file->private_data;
	
	// Allocate a kernel buffer 
	kernelBuff = kmalloc(len, GFP_KERNEL);
	if (kernelBuff == NULL) {
		return -ENOMEM; //Cannot allocate memory
	}

	if (len > maxMessageSize) {
		// Copy message to kernel buffer until maxMessageSize
		notCopied = copy_from_user(kernelBuff, buff, maxMessageSize);
	} else{
		// Copy message to kernel buffer 
		notCopied = copy_from_user(kernelBuff, buff, len);
	}

	

	minor = get_minor(file);

	mutex_lock(&(session->mtx));
	// check if exists write timeout
	if (session->writeTimeout) {
		// allocate pending write struct
		pendingWriteStruct = kmalloc(sizeof(struct pendingWriteStruct), GFP_KERNEL);
		if (pendingWriteStruct == NULL) {
			kfree(kernelBuff);
			mutex_unlock(&(session->mtx));
			return -ENOMEM; //Cannot allocate memory
		}
		// init pending write struct
		pendingWriteStruct->minor = minor;
		pendingWriteStruct->session = session;
		pendingWriteStruct->kernelBuff = kernelBuff;
		pendingWriteStruct->len = len;
		INIT_LIST_HEAD(&(pendingWriteStruct->list));
		INIT_DELAYED_WORK(&(pendingWriteStruct->delayedWork), write_deferred_message);
		//Enqueue pending write to others
		list_add_tail(&(pendingWriteStruct->list),
			      &(session->pendingWrites));
		mutex_unlock(&(session->mtx));
		queue_delayed_work(session->writeWorkqueue, &(pendingWriteStruct->delayedWork), session->writeTimeout);
		return 0;	// nothing writed actually
	}

	mutex_unlock(&(session->mtx));

	// if not exists write timeout write immediately
	mutex_lock(&(minors[minor].mtx));
	ret = write_message(&minors[minor], kernelBuff, len);
	// Post message success
	if (ret >= 0) {	
		awake_sleeping_reader(&(minors[minor]));
	}
	mutex_unlock(&(minors[minor].mtx));

	return len-notCopied;
}

static void revoke_delayed_message(struct sessionStruct *sessionStruct){

    struct list_head *ptr;
    struct list_head *temp;
    struct pendingWriteStruct *pendingWriteStruct;
    int ret;

    list_for_each_safe(ptr, temp, &(sessionStruct->pendingWrites)){
        pendingWriteStruct = list_entry(ptr, struct pendingWriteStruct, list);    

        ret = cancel_delayed_work(&pendingWriteStruct->delayedWork);  
        //true if the canceled work is pending and canceled, false if it wasn't pending
        if (ret){
            list_del(&(pendingWriteStruct->list));
            kfree(pendingWriteStruct->kernelBuff);
            kfree(pendingWriteStruct);
        }
    }
}

static long dev_ioctl (struct file *file, unsigned int command, unsigned long timeout){

    struct sessionStruct *sessionStruct;

    sessionStruct = (struct sessionStruct *) file->private_data;
    
    //timeout in second is converted in jiffies using HZ macro
    switch(command){
        case SET_SEND_TIMEOUT:
            mutex_lock(&(sessionStruct->mtx));
            sessionStruct->writeTimeout = timeout * HZ;
            mutex_unlock(&(sessionStruct->mtx));
            break;
        case SET_RECV_TIMEOUT:
            mutex_lock(&(sessionStruct->mtx));
            sessionStruct->readTimeout = timeout * HZ;
            mutex_unlock(&(sessionStruct->mtx));
            break;
        case REVOKE_DELAYED_MESSAGES:
            mutex_lock(&(sessionStruct->mtx));
            revoke_delayed_message(sessionStruct);
            mutex_unlock(&(sessionStruct->mtx));
            break;
        default:
            printk(KERN_INFO "%s: command not valid. Valid command are: %s, %s, %s \n", MODNAME, "SET_SEND_TIMEOUT", 
                "SET_RECV_TIMEOUT", "REVOKE_DELAYED_MESSAGES");
            return -EPERM;  //operation not permitted
	}
	
	return 0;
}

static int dev_release(struct inode *inode, struct file *file){

    int minor;
    struct sessionStruct *sessionStruct;
    sessionStruct = (struct sessionStruct *)file->private_data;

    flush_workqueue(sessionStruct->writeWorkqueue);
    destroy_workqueue(sessionStruct->writeWorkqueue);
    minor = get_minor(file);
    mutex_lock(&(minors[minor].mtx));
    list_del(&(sessionStruct->list));
    mutex_unlock(&(minors[minor].mtx));

    kfree(sessionStruct);

    return 0;
}

static void readers_awake(struct minorStruct *minorStruct){

    struct list_head *ptr;
    struct list_head *temp;
    struct pendingReadStruct *pendingReadStruct;

    list_for_each_safe(ptr, temp, &(minorStruct->pendingReads)){
        pendingReadStruct = list_entry(ptr, struct pendingReadStruct, list);
        pendingReadStruct->flushing = 1;
        list_del(&(pendingReadStruct->list));
        wake_up_interruptible(&(minorStruct->readWorkqueue));
    }
}

static int dev_flush(struct file *file, fl_owner_t id){

    int minor;
    struct list_head *ptr;
    struct list_head *temp;
    struct sessionStruct *sessionStruct;

    minor = get_minor(file);
    mutex_lock(&(minors[minor].mtx));

    list_for_each_safe(ptr,temp, &(minors[minor].sessions)){
        sessionStruct = list_entry(ptr, struct sessionStruct, list);
        mutex_lock(&(sessionStruct->mtx));
        revoke_delayed_message(sessionStruct);
        mutex_unlock(&(sessionStruct->mtx));
    }

    readers_awake(&(minors[minor]));
    mutex_unlock(&(minors[minor].mtx));

    return 0;
}

static struct file_operations fops = {
  .owner = THIS_MODULE,//do not forget this
  .write = dev_write,
  .read = dev_read,
  .open =  dev_open,
  .release = dev_release,
  .unlocked_ioctl = dev_ioctl,
  .flush = dev_flush,
};


int install_module(void)
{
	int i;

	// init minor_struct array 
	for (i = 0; i < MINORS; i++) {
		minors[i].currentSize = 0;
		mutex_init(&(minors[i].mtx));
		INIT_LIST_HEAD(&(minors[i].pendingReads));
		init_waitqueue_head(&(minors[i].readWorkqueue));
		INIT_LIST_HEAD(&(minors[i].messageFifoOrder));
		INIT_LIST_HEAD(&(minors[i].sessions));
	}

	// register driver
	major = __register_chrdev(0, 0, MINORS, DEVICE_NAME, &fops);
	if (major < 0) {
		printk(KERN_INFO "%s: Driver installation failed\n", MODNAME);
		return major;
	}
	printk(KERN_INFO "%s: Driver correctly installed, MAJOR = %d\n",
	       MODNAME, major);
	printk(KERN_INFO "%s: maxMessageSize = %d\n",
	       MODNAME, maxMessageSize);
	printk(KERN_INFO "%s: maxStorageSize = %d\n",
	       MODNAME, maxStorageSize);
	return 0;
}

void remove_module(void)
{
	int i;
	struct list_head *ptr;
	struct list_head *tmp;
	struct messageStruct *msg;

	for (i = 0; i < MINORS; i++) {
		// flush device file content
		list_for_each_safe(ptr, tmp, &(minors[i].messageFifoOrder)) {
			msg = list_entry(ptr, struct messageStruct, list);
			list_del(&(msg->list));
			kfree(msg->buff);
			kfree(msg);
		}
	}

	// unregister driver
	unregister_chrdev(major, DEVICE_NAME);
	printk(KERN_INFO "%s: Driver correctly uninstalled\n", MODNAME);
	return;
}

module_init(install_module);
module_exit(remove_module);



