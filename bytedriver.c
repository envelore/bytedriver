#include <linux/module.h>
#include <linux/version.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/kdev_t.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <linux/slab.h>

static dev_t first; 			// Global variable for the first device number
static struct cdev c_dev; 		// Global variable for the character device structure
static struct class *cl;	 	// Global variable for the device class
static DECLARE_WAIT_QUEUE_HEAD(queueForWait);

//---------------------------------------------------------------------------
static int sizeOfBuffer = 20;	            // parametric size of the buffer
//module_param(sizeOfBuffer, int, 0); 

static char* driverBuffer;	                        // the buffer
static char* writingPointer;	        // a pointer for writing into the buffer
static char* readingPointer;	        // a pointer for reading from the buffer
static int writePos = 0;
static int readPos = 0;
static bool readMutex = true;
static bool writeMutex = true;
//---------------------------------------------------------------------------
 static bool isBufferEmpty(void) {
    if (readPos == writePos)
        return true;
    else
        return false;
}

static bool isBufferFull(void) {
    if ((writePos == readPos - 1) ||
        ((writePos == sizeOfBuffer - 1) &&
         (readPos == 0)))
        return true;
    else
        return false;
}
//----------------------------------------------------------------------------
static int my_open(struct inode *i, struct file *f) {
	printk(KERN_INFO "Driver: open(wrPos: %d, readPos: %d)\n", writePos, readPos);
  	return 0;
}

static int my_close(struct inode *i, struct file *f) {
	printk(KERN_INFO "Driver: close(wrPos: %d, readPos: %d)\n", writePos, readPos);
  	return 0;
}
//----------------------------------------------------------------------------
static ssize_t my_read(struct file *f,		// path to the device
						char __user *buf,	// a pointer to buffer from userspace
						size_t len,			// size of data asking from userspace
						loff_t *off)		// ??????
{
    ssize_t countOfReadedBytes = 0;
   /* if (!readMutex) {                       // checking that no another process are using the buffer
        return 0;
    }*/
    readMutex = false;
    countOfReadedBytes = 0;
	printk(KERN_INFO "Driver: read(length: %ld)\n", len);
    char* blockOfKernelMemory = (char*)kcalloc(len, sizeof(char), GFP_KERNEL);
    if (blockOfKernelMemory == NULL) {
        printk(KERN_INFO "Error: it hadn't provided any memory.");
        kfree(blockOfKernelMemory);
        readMutex = true;
        return -1;
    }
    if (isBufferEmpty()) {
        readMutex = true;
        kfree(blockOfKernelMemory);
        return countOfReadedBytes;          // stop work cause the buffer is empty
    } else {
	    while (countOfReadedBytes != len) {
		    if (isBufferEmpty()) {
                //while (isBufferEmpty()) {}	        // if the buffer is empty go sleep
		    	wait_event_interruptible(queueForWait, !isBufferEmpty());
                /*copy_to_user(buf, blockOfKernelMemory, countOfReadedBytes);
                readMutex = true;
                kfree(blockOfKernelMemory);
                return countOfReadedBytes; */
		    }
            blockOfKernelMemory[countOfReadedBytes] = driverBuffer[readPos];
            countOfReadedBytes++;
		    if (readPos == sizeOfBuffer - 1) {
                readPos = 0;
            } else {
                readPos++;
            }
        }
        copy_to_user(buf, blockOfKernelMemory, countOfReadedBytes);
        kfree(blockOfKernelMemory);
        readMutex = true;
        return countOfReadedBytes;
    }
}

static ssize_t my_write(struct file *f,		// path to the device
						const char __user *buf,	// a pointer to buffer from userspace
						size_t len,			// size of data asking from userspace
						loff_t *off)		// ??????
{
    ssize_t countOfWrittenBytes = 0;
    if (driverBuffer == NULL) {
        printk(KERN_ALERT "Driver writting: ISSUE FROM BUFFER");
        return 0;
    }
	/*if (!writeMutex) {
        return 0;
    }*/
    writeMutex = false;
	printk(KERN_INFO "Driver: write(length: %ld)\n", len);
    char* blockOfKernelMemory = (char*)kcalloc(len, sizeof(char), GFP_KERNEL);
    if (blockOfKernelMemory == NULL) {
        printk(KERN_INFO "Error: it hadn't provided any memory.");
        kfree(blockOfKernelMemory);
        writeMutex = true;
        return -1;
    }
	if (isBufferFull()) {
        writeMutex = true;
        kfree(blockOfKernelMemory);
		return countOfWrittenBytes;
	} else {
        copy_from_user(blockOfKernelMemory, buf, len);
        while (countOfWrittenBytes != len) {
            if (isBufferFull()) {
              /*  writeMutex = true;
                kfree(blockOfKernelMemory);
		        return countOfWrittenBytes;*/
                wait_event_interruptible(queueForWait, !isBufferFull());
            }
            //*writingPointer = blockOfKernelMemory[countOfWrittenBytes];
            driverBuffer[writePos] = blockOfKernelMemory[countOfWrittenBytes];
            countOfWrittenBytes++;
            /*if (writingPointer == &driverBuffer[sizeOfBuffer - 1]) {
                writingPointer = &driverBuffer[0];
                writePos = 0;
            } else {
                writingPointer += sizeof(char);
                writePos++;
            }*/
            if (writePos == sizeOfBuffer - 1) {
                writePos = 0;
            } else {
                writePos++;
            }
        }
        kfree(blockOfKernelMemory);
        writeMutex = true;
        return countOfWrittenBytes;
	}
}


//------------------------------------------------------------------------------

  static struct file_operations pugs_fops =
{
    .owner = THIS_MODULE,
    .open = my_open,
    .release = my_close,
    .read = my_read,
    .write = my_write
};

static int __init envelore_init(void) /* Constructor */
{
    //DECLARE_WAIT_QUEUE_HEAD(queueForWait);
    //init_waitqueue_head(queueForWait);
   if (alloc_chrdev_region(&first, 0, 1, "enveloredevice") < 0)
    {
        pr_alert("first cond fail :( ");
        return -1;
    } else {pr_info("alloc_chrdev_region - ok... ");}
    if (IS_ERR(cl = class_create(THIS_MODULE, "chardrv")))
    {
        pr_alert("2nd cond fail :( ");
        unregister_chrdev_region(first, 1);
        return PTR_ERR(cl);
    } else {pr_info("class_create - ok... ");}
    if (device_create(cl, NULL, first, NULL, "envel") == NULL)
    {
        pr_alert("3d cond fail :( ");
        class_destroy(cl);
        unregister_chrdev_region(first, 1);
        return -1;
    }  else {pr_info("device_create - ok... ");}
    cdev_init(&c_dev, &pugs_fops);
    pr_info("cdev_init - ok... ");
    if (cdev_add(&c_dev, first, 1) == -1)
    {
        pr_alert("4th cond fail :( ");
        device_destroy(cl, first);
        class_destroy(cl);
        unregister_chrdev_region(first, 1);
        return -1;
    }  else {
        pr_info("cdev_add - ok... ");
    }
    driverBuffer = (char*)kcalloc(sizeOfBuffer, sizeof(char), GFP_KERNEL);
    if (driverBuffer == NULL) {
        pr_err("MEMORY KCALLOC PROBLEMZ");
    }

    writingPointer = driverBuffer;
    readingPointer = driverBuffer;
    pr_alert("ENVELORE: Driver started.\n");
    return 0;
}

static void __exit envelore_exit(void) /* Destructor */
{
    cdev_del(&c_dev);
    device_destroy(cl, first);
    class_destroy(cl);
    unregister_chrdev_region(first, 1);
    kfree(driverBuffer);
    pr_alert("ENVELORE: Driver stoped");
}


module_init(envelore_init);
module_exit(envelore_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Vladimir Nuhtilin <envelore@yandex.ru>");
MODULE_DESCRIPTION("My First Character Driver");
