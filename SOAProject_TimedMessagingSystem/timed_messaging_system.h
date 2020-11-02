#include <linux/ioctl.h>


//The Magic Number is a unique number or character that will differentiate our set of ioctl calls from the other ioctl calls
#define IOC_MAGIC 'k' // defines the magic number
#define SET_SEND_TIMEOUT _IO(IOC_MAGIC, 0)
#define SET_RECV_TIMEOUT _IO(IOC_MAGIC, 1)
#define REVOKE_DELAYED_MESSAGES _IO(IOC_MAGIC, 2)

#ifdef __KERNEL__

#define MODNAME "TIMED-MESSAGING-SYSTEM"
#define DEVICE_NAME "timed-messaging-system-device"
#define WRITE_WORK_QUEUE "timed-messaging-system-workqueue"
#define MINORS 8



// Instance of a device file
struct minorStruct {
	unsigned int currentSize;
	struct mutex mtx;
	struct list_head messageFifoOrder;          /* List of messages stored in the device file */
	struct list_head sessions;      /* List of sessions openend in device file */
	struct list_head pendingReads; /* List of pending reads */
	wait_queue_head_t readWorkqueue;      /* Used from blocking readers to wait for messages */
};


// Message information
struct messageStruct {
	unsigned int size;
	char *buff;
	struct list_head list;
};


// I/O session auxiliary information
struct sessionStruct {
	struct mutex mtx;
	struct workqueue_struct *writeWorkqueue; /* Used to defer writes*/
	unsigned long writeTimeout;       /* 0 means immediate storing */
	unsigned long readTimeout;        /* 0 means non-blocking reads */
	struct list_head pendingWrites;
	struct list_head list;
};


// Delayed write information
struct pendingWriteStruct {
	int minor;
	struct sessionStruct *session;
	char *kernelBuff;                     /* Points to the message to post */
	unsigned int len;               /* Size of the message to post */
	struct delayed_work delayedWork;
	struct list_head list;
};

// Read waiting for available messages
struct pendingReadStruct {
	int availableMessage; /* Set from a writer when a new message is available */
	int flushing;      /* Set when someone calls dev_flush() */
	struct list_head list;	
};


static int dev_open(struct inode *, struct file *);

static ssize_t dev_read(struct file *, char *, size_t , loff_t * );

static ssize_t dev_write(struct file *, const char *, size_t , loff_t * );

static long dev_ioctl (struct file *, unsigned int , unsigned long );

static int dev_release(struct inode *, struct file *);

static int dev_flush(struct file *, fl_owner_t );

#endif


