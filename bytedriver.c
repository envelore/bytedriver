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
#include <linux/mutex.h>


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
   	struct mutex readingMutex;
    struct mutex writingMutex;
    uid_t user;
    struct buffer* next;
};

static struct buffer* fisrtBuffer = NULL;

static void addNewBuffer(void) {
    struct buffer* newBuffer = (struct buffer*)kcalloc(1, sizeof(struct buffer), GFP_KERNEL);
    if (newBuffer == NULL) {
        pr_info("addNewBuffer(): havn't got memory for buffer");
    }
    newBuffer->memory = (char*)kcalloc(sizeOfBuffer, sizeof(char), GFP_KERNEL);
    if (newBuffer->memory == NULL) {
        kfree(newBuffer);
        pr_info("addNewBuffer(): havn't got memory for buffer memory");
    }
    newBuffer->writePosition = 0;
    newBuffer->readPosition = 0;
    newBuffer->user = current_uid().val;
    
    struct buffer* i = fisrtBuffer;
    if (i != NULL) {
        while (i->next != NULL) {
            i = i->next;
        }
        i->next = newBuffer;
    } else {
        fisrtBuffer = newBuffer;
        pr_info("Now firstBuffer pointer is %s equal NULL", (fisrtBuffer == NULL)?"":"NOT");
        pr_info("BTW, newBuffer is %s equal NULL", (newBuffer == NULL)?"":"NOT");
    }
    pr_info("addNewBuffer(): new buffer for user id:%d has been created", newBuffer->user);
}

static struct buffer* searchBufferByID(uid_t searchID) {
    struct buffer* result = fisrtBuffer;
    pr_info("searchBufferByID(%d): searching buffer for userID:%d...", searchID, searchID);
    if (result != NULL) {
        while (result != NULL) {
            pr_info("searchBufferByID(%d): i see buffer for userID:%d", searchID, result->user);
            if (result->user == searchID)
                return result;
            else 
                result = result->next;
        }
        return result;
    } else {
        return result;
    }
}

//---------------------------------------------------------------------------
 static bool isBufferEmpty(struct buffer* buf) {
    if (buf->readPosition == buf->writePosition)
        return true;
    else
        return false;
}

static bool isBufferFull(struct buffer* buf) {
    if ((buf->writePosition == buf->readPosition - 1) ||
        ((buf->writePosition == sizeOfBuffer - 1) &&
         (buf->readPosition == 0)))
        return true;
    else
        return false;
}
//----------------------------------------------------------------------------

static int my_open(struct inode *i, struct file *f) {
    uid_t user = current_uid().val;
    if (searchBufferByID(user) == NULL) {
        pr_info("searchByID(%d): havn't found user's buffer", user);
        addNewBuffer();
    }

	printk(KERN_INFO "Driver: open(uid: %d)\n", current_uid().val);
  	return 0;
}

static int my_close(struct inode *i, struct file *f) {
	printk(KERN_INFO "Driver: close(uid: %d)\n", current_uid().val);
  	return 0;
}
//----------------------------------------------------------------------------
static ssize_t my_read(struct file *f,		// path to the device
						char __user *buf,	// a pointer to buffer from userspace
						size_t len,			// size of data asking from userspace
						loff_t *off)		// ??????
{
    ssize_t countOfReadedBytes = 0;
    struct buffer* currentBuffer = searchBufferByID(current_uid().val);
    if (currentBuffer == NULL) {
        printk(KERN_INFO "Read(): PROBLEMZ WITH BUFFER POINTER, EXITED, SIR");
        return 0;
    }
    mutex_lock(&currentBuffer->readingMutex);
	printk(KERN_INFO "Driver: read(length: %ld, uid: %d)\n", len, current_uid().val);
    char* blockOfKernelMemory = (char*)kcalloc(len, sizeof(char), GFP_KERNEL);
    if (blockOfKernelMemory == NULL) {
        goto readexit;
    }
    if (isBufferEmpty(currentBuffer)) {
        goto readexit;                      // stop work cause the buffer is empty
    } else {
	    while (countOfReadedBytes != len) {
		    if (isBufferEmpty(currentBuffer)) {          // if the buffer is empty go sleep
		    	wait_event_interruptible(queueForRead, !isBufferEmpty(currentBuffer));
		    }
            blockOfKernelMemory[countOfReadedBytes] = currentBuffer->memory[currentBuffer->readPosition];
            printk(KERN_INFO "buffer: read(wrPos: %d, readPos: %d)\n", 
                                        currentBuffer->writePosition,
                                        currentBuffer->readPosition);
            countOfReadedBytes++;
		    if (currentBuffer->readPosition == sizeOfBuffer - 1) {
                currentBuffer->readPosition = 0;
            } else {
                currentBuffer->readPosition++;
            }
            wake_up_interruptible(&queueForWrite);
        }
        copy_to_user(buf, blockOfKernelMemory, countOfReadedBytes);
readexit:   kfree(blockOfKernelMemory);
        mutex_unlock(&currentBuffer->readingMutex);
        return countOfReadedBytes;
    }
}

static ssize_t my_write(struct file *f,		// path to the device
						const char __user *buf,	// a pointer to buffer from userspace
						size_t len,			// size of data asking from userspace
						loff_t *off)		// ??????
{
    ssize_t countOfWrittenBytes = 0;
    struct buffer* currentBuffer = searchBufferByID(current_uid().val);
    if (currentBuffer == NULL) {
        printk(KERN_INFO "Write(): PROBLEMZ WITH BUFFER POINTER, EXITED, SIR");
        return 0;
    }
    mutex_lock(&currentBuffer->writingMutex);
	printk(KERN_INFO "Driver: write(length: %ld, uid: %d)\n", len, current_uid().val);
    char* blockOfKernelMemory = (char*)kcalloc(len, sizeof(char), GFP_KERNEL);
    if (blockOfKernelMemory == NULL) {
        printk(KERN_INFO "Error: it hadn't provided any memory.");
        goto writeexit;
    }
	if (isBufferFull(currentBuffer)) {
        goto writeexit;
	} else {
        copy_from_user(blockOfKernelMemory, buf, len);
        while (countOfWrittenBytes != len) {
            if (isBufferFull(currentBuffer)) {
                wait_event_interruptible(queueForWrite, !isBufferFull(currentBuffer));
            }
            printk(KERN_INFO "buffer: write(wrPos: %d, readPos: %d)\n", 
                                        currentBuffer->writePosition,
                                        currentBuffer->readPosition);
            currentBuffer->memory[currentBuffer->writePosition] = blockOfKernelMemory[countOfWrittenBytes];
            countOfWrittenBytes++;
            if (currentBuffer->writePosition == sizeOfBuffer - 1) {
                currentBuffer->writePosition = 0;
            } else {
                currentBuffer->writePosition++;
            }
            wake_up_interruptible(&queueForRead);
        }
writeexit:  kfree(blockOfKernelMemory);
        mutex_unlock(&currentBuffer->writingMutex);
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
        goto e0;
    } else {pr_info("alloc_chrdev_region - ok... ");}
    if (IS_ERR(cl = class_create(THIS_MODULE, "chardrv")))
    {
        goto e1;
    } else {pr_info("class_create - ok... ");}
    if (device_create(cl, NULL, first, NULL, "envel") == NULL)
    {
        goto e2;
    }  else {pr_info("device_create - ok... ");}
    cdev_init(&c_dev, &pugs_fops);
    pr_info("cdev_init - ok... ");
    if (cdev_add(&c_dev, first, 1) == -1)
    {
        goto e3;
    }  else {
        pr_info("cdev_add - ok... ");
    }
    //sys_chmod("/dev/envel", 0777);
    pr_alert("ENVELORE: Driver started.\n");
    return 0;

e3: device_destroy(cl, first);
e2: class_destroy(cl);
e1: unregister_chrdev_region(first, 1);
e0: return -1;
}

static void __exit envelore_exit(void) /* Destructor */
{
    cdev_del(&c_dev);
    device_destroy(cl, first);
    class_destroy(cl);
    unregister_chrdev_region(first, 1);
    struct buffer* i = fisrtBuffer;
    struct buffer* temp;
    if (i != NULL) {
        kfree(i->memory);
        temp = i->next;
        kfree(i);
        i = temp;
    }
    pr_alert("ENVELORE: Driver stoped");
}


module_init(envelore_init);
module_exit(envelore_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Vladimir Nukhtilin <envelore@yandex.ru>");
MODULE_DESCRIPTION("My First Character Driver");
