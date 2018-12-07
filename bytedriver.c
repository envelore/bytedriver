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
#include <linux/cred.h>

static dev_t first; 			// Global variable for the first device number
static struct cdev c_dev; 		// Global variable for the character device structure
static struct class *cl;	 	// Global variable for the device class
static DECLARE_WAIT_QUEUE_HEAD(queueForRead);
static DECLARE_WAIT_QUEUE_HEAD(queueForWrite);

//---------------------------------------------------------------------------
static int sizeOfBuffer = 20;	            // parametric size of the buffer
module_param(sizeOfBuffer, int, 0); 

struct buffer {
    char* memory;
    int writePosition;
    int readPosition;
    bool readingMutex;
    bool writingMutex;
    uid_t user;
    struct buffer* next;
} initBuffer;

static struct buffer* addNewBuffer(void) {
    struct buffer* newBuffer = (struct buffer*)kcalloc(1, sizeof(struct buffer), GFP_KERNEL);
    if (newBuffer == NULL) {
        return NULL;
    }
    newBuffer->memory = (char*)kcalloc(sizeOfBuffer, sizeof(char), GFP_KERNEL);
    if (newBuffer->memory == NULL) {
        kfree(newBuffer);
        return NULL;
    }
    newBuffer->writePosition = 0;
    newBuffer->readPosition = 0;
    newBuffer->writingMutex = true;
    newBuffer->readingMutex = true;
    newBuffer->user = current_uid().val;
    
    struct buffer* i = &initBuffer;
     while (i->next != NULL) {
        i = i->next;
    }
    i->next = newBuffer;

    return newBuffer;
}

static struct buffer* searchBufferByID(uid_t searchID) {
    struct buffer* result = initBuffer.next;
    if (initBuffer.user == searchID)
        return &initBuffer;
    while ((result != NULL) || (result->user != searchID)) {
        result = result->next;
    }
    return result;
}

static char* driverBuffer;	                        // the buffer
static int writePos = 0;
static int readPos = 0;
static bool readMutex = true;
static bool writeMutex = true;
static uid_t currentUser;
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
static void exportBufferImage(struct buffer* image)
{
    image->memory = driverBuffer;
    image->writePosition = writePos;
    image->readPosition = readPos;
    image->writingMutex = writeMutex;
    image->readingMutex = readMutex;
    image->user = current_uid().val;
}

static void importBufferImage(struct buffer* image)
{
    driverBuffer = image->memory;
    writePos = image->writePosition;
    readPos = image->readPosition;
    writeMutex = image->writingMutex;
    readMutex = image->readingMutex;
    currentUser = image->user;
}

//----------------------------------------------------------------------------
static int my_open(struct inode *i, struct file *f) {
    if (current_uid().val != currentUser) {
        if (readMutex && writeMutex == true) {
            exportBufferImage(currentUser);
            if (searchBufferByID(current_uid().val) == NULL) {
                importBufferImage(addNewBuffer());
            } else {
                importBufferImage(current_uid().val);
            }
        } else {
            return -1;
        }
    }
	printk(KERN_INFO "Driver: open(wr: %d, rd: %d, uid: %d)\n", writePos, readPos, current_uid().val);
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
    if (!readMutex) {                     // checking that no another process are using the buffer
        return 0;
    }
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
		    if (isBufferEmpty()) {          // if the buffer is empty go sleep
		    	wait_event_interruptible(queueForRead, !isBufferEmpty());
		    }
            blockOfKernelMemory[countOfReadedBytes] = driverBuffer[readPos];
            printk(KERN_INFO "buffer: read(wrPos: %d, readPos: %d)\n", writePos, readPos);
            countOfReadedBytes++;
		    if (readPos == sizeOfBuffer - 1) {
                readPos = 0;
            } else {
                readPos++;
            }
            wake_up_interruptible(&queueForWrite);
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
                wait_event_interruptible(queueForWrite, !isBufferFull());
            }
            printk(KERN_INFO "buffer: write(wrPos: %d, readPos: %d)\n", writePos, readPos);
            driverBuffer[writePos] = blockOfKernelMemory[countOfWrittenBytes];
            countOfWrittenBytes++;
            if (writePos == sizeOfBuffer - 1) {
                writePos = 0;
            } else {
                writePos++;
            }
            wake_up_interruptible(&queueForRead);
        }
        kfree(blockOfKernelMemory);
        writeMutex = true;
        return countOfWrittenBytes;
	}
}
//------------------------------------------------------------------------------



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
    currentUser = current_uid().val;
    exportBufferImage(&initBuffer);
    initBuffer.next = NULL;

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
