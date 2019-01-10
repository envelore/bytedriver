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
#include <linux/spinlock.h>
#include <linux/ioctl.h>


static dev_t first; 			// Global variable for the first device number
static struct cdev c_dev; 		// Global variable for the character device structure
static struct class *cl;	 	// Global variable for the device class
static DECLARE_WAIT_QUEUE_HEAD(queueForRead);
static DECLARE_WAIT_QUEUE_HEAD(queueForWrite);

//---------------------------------------------------------------------------
static int sizeOfNewBuffer = 20;	            // parametric size of the buffer
module_param(sizeOfNewBuffer, int, 0); 

struct buffer {
    char* memory;
    int writePosition;
    int readPosition;
   	struct mutex readingMutex;
    struct mutex writingMutex;
    uid_t user;
    struct buffer* next;
    int countOfOpening;
    spinlock_t* comparing;
    bool alarm;
    int sizeOfBuffer;
};

static struct buffer* fisrtBuffer = NULL;

static void addNewBuffer(void) {
    struct buffer* newBuffer = (struct buffer*)kcalloc(1, sizeof(struct buffer), GFP_KERNEL);
    if (newBuffer == NULL) {
        pr_info("addNewBuffer(): havn't got memory for buffer");
    }
    newBuffer->memory = (char*)kcalloc(sizeOfNewBuffer, sizeof(char), GFP_KERNEL);
    if (newBuffer->memory == NULL) {
        kfree(newBuffer);
        pr_info("addNewBuffer(): havn't got memory for buffer memory");
    }
    newBuffer->comparing = (spinlock_t*)kcalloc(1, sizeof(spinlock_t), GFP_KERNEL);
    newBuffer->sizeOfBuffer = sizeOfNewBuffer;
    newBuffer->writePosition = 0;
    newBuffer->readPosition = 0;
    newBuffer->user = current_uid().val;
    newBuffer->alarm = false;
    pr_info("addNewBuffer(): Initializing spinlock...");
    spin_lock_init(newBuffer->comparing);
    pr_info("addNewBuffer(): Spinlock has been initialized successfully!");
    struct buffer* i = fisrtBuffer;
    if (i != NULL) {
        while (i->next != NULL) {
            i = i->next;
        }
        i->next = newBuffer;
    } else {
        fisrtBuffer = newBuffer;
        pr_info("Now firstBuffer pointer is %sequal NULL", (fisrtBuffer == NULL)?"":"NOT ");
        pr_info("BTW, newBuffer is %sequal NULL", (newBuffer == NULL)?"":"NOT ");
    }
    pr_info("addNewBuffer(): new buffer for user id:%d has been created, size = %d", newBuffer->user,
                                                                            newBuffer->sizeOfBuffer);
}

static struct buffer* searchBufferByID(uid_t searchID) {
    struct buffer* result = fisrtBuffer;
    //pr_info("searchBufferByID(%d): searching buffer for userID:%d...", searchID, searchID);
    if (result != NULL) {
        while (result != NULL) {
            //pr_info("searchBufferByID(%d): found for userID:%d", searchID, result->user);
            if (result->user == searchID) {
                //pr_info("searchBufferByID(%d): returned buffer userID:%d", searchID, result->user);
                return result;
            }
            else 
                result = result->next;
        }
        //pr_info("searchBufferByID(%d): didn't find for userID:%d", searchID);
        return result;
    } else {
        //pr_info("searchBufferByID(%d): anyone buffer hadn't been created", searchID);
        return result;
    }
}

//---------------------------------------------------------------------------
 static bool isBufferEmpty(struct buffer* buf) {
    if (buf->readPosition == buf->writePosition) {
        return true; 
    } else {
        return false;
    }
}

static bool isBufferFull(struct buffer* buf) {
    if ((buf->writePosition == buf->readPosition - 1) ||
        ((buf->writePosition == buf->sizeOfBuffer - 1) &&
         (buf->readPosition == 0))) {
        return true;
    } else {
        return false;
    }
}
//----------------------------------------------------------------------------

static long my_ioctl(struct file *f, unsigned int cmd, unsigned long arg) {
    struct buffer* currentBuffer = searchBufferByID(current_uid().val);
    if (currentBuffer == NULL) {
        printk(KERN_INFO "ioctl(): PROBLEMZ WITH BUFFER POINTER, EXITED, SIR");
        return -ENOTTY;
    }
    if(cmd != 17) {
        printk(KERN_INFO "ioctl(): em, it's wrong cmd...");
        return -ENOTTY;
    } else {
        sizeOfNewBuffer = (int)arg;
        printk(KERN_INFO "ioctl(): hey, i've changed sizeOfNewBuffer = %d", sizeOfNewBuffer);
        return 0;
    } 
} 

static int my_open(struct inode *i, struct file *f) {
    printk(KERN_INFO "Driver: open(uid: %d)\n", current_uid().val);
    uid_t user = current_uid().val;
    if (searchBufferByID(user) == NULL) {
        pr_info("Open(): havn't found user's buffer");
        addNewBuffer();
    }
    searchBufferByID(user)->countOfOpening++;
  	return 0;
}

static int my_close(struct inode *i, struct file *f) {
	printk(KERN_INFO "Driver: close(uid: %d)\n", current_uid().val);
    searchBufferByID(current_uid().val)->countOfOpening--;
    printk("Close(): hey, i checked that isBufferEmpty() = %s", 
                    isBufferEmpty(searchBufferByID(current_uid().val)) ? "true" : "false");
    searchBufferByID(current_uid().val)->alarm = true;
    wake_up_interruptible(&queueForRead);
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
	printk(KERN_INFO "Driver: start read(length: %ld, uid: %d)\n", len, current_uid().val);
    char* blockOfKernelMemory = (char*)kcalloc(len, sizeof(char), GFP_KERNEL);
    if (blockOfKernelMemory == NULL) {
        goto readexit;
    }
   /* if (isBufferEmpty(currentBuffer)) {
        goto readexit;                      // stop work cause the buffer is empty
    } else {*/
	    while (countOfReadedBytes != len) {
            spin_lock(currentBuffer->comparing);
            if ((currentBuffer->countOfOpening < 2)&&(isBufferEmpty(currentBuffer))) {
                //pr_info("read(): flag 5");
                spin_unlock(currentBuffer->comparing);
                //pr_info("read(): flag 6");
                goto readeof;
            }
            spin_unlock(currentBuffer->comparing);
            blockOfKernelMemory[countOfReadedBytes] = currentBuffer->memory[currentBuffer->readPosition];
           /* printk(KERN_INFO "buffer: read(wrPos: %d, readPos: %d, done: %d, remained: %d, byte: %c)\n", 
                                        currentBuffer->writePosition,
                                        currentBuffer->readPosition,
                                        countOfReadedBytes,
                                        len - countOfReadedBytes,
                                        blockOfKernelMemory[countOfReadedBytes]);*/
            
            /*if (blockOfKernelMemory[countOfReadedBytes] == '\0') {
                goto readeof;
            }*/
            countOfReadedBytes++;
            spin_lock(currentBuffer->comparing);
            if (currentBuffer->readPosition == currentBuffer->sizeOfBuffer - 1) {
                currentBuffer->readPosition = 0;
            } else {
                currentBuffer->readPosition++;
            }
            wake_up_interruptible(&queueForWrite);
            if (isBufferEmpty(currentBuffer)) {
                if (currentBuffer->countOfOpening < 2) {
                   // pr_info("read(): flag 14");
                    spin_unlock(currentBuffer->comparing);
                    //pr_info("read(): flag 15");
                    goto readeof;
                }
                //pr_info("read(): flag 16");
                spin_unlock(currentBuffer->comparing);
		    	//pr_info("read(): flag 17");
                wait_event_interruptible(queueForRead, !isBufferEmpty(currentBuffer)||(currentBuffer->alarm == true));
                currentBuffer->alarm = false;
                //pr_info("read(): flag 18");  
		    } else {
                spin_unlock(currentBuffer->comparing);
            }            
        }
readeof:    pr_alert("buffer: read(wrPos: %d, readPos: %d, done: %d, remained: %d)\n", 
                                        currentBuffer->writePosition,
                                        currentBuffer->readPosition,
                                        countOfReadedBytes,
                                        len - countOfReadedBytes);
        copy_to_user(buf, blockOfKernelMemory, countOfReadedBytes);
readexit:   kfree(blockOfKernelMemory);
        mutex_unlock(&currentBuffer->readingMutex);
    //    pr_info("read(): flag 21");
        return countOfReadedBytes;
   /* }*/
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
            
            currentBuffer->memory[currentBuffer->writePosition] = blockOfKernelMemory[countOfWrittenBytes];
            countOfWrittenBytes++;
            
           /* printk(KERN_INFO "buffer: written(wrPos: %d, readPos: %d, done: %d, remained: %d, byte: %c)\n", 
                                        currentBuffer->writePosition,
                                        currentBuffer->readPosition,
                                        countOfWrittenBytes,
                                        len - countOfWrittenBytes,
                                        currentBuffer->memory[currentBuffer->writePosition]);*/
            //if (blockOfKernelMemory[countOfWrittenBytes - 1] == '\0') 
            //    break;
            spin_lock(currentBuffer->comparing);
            if (currentBuffer->writePosition == currentBuffer->sizeOfBuffer - 1) {
                currentBuffer->writePosition = 0;
            } else {
                currentBuffer->writePosition++;
            }
            wake_up_interruptible(&queueForRead);
            if (isBufferFull(currentBuffer)) {
                spin_unlock(currentBuffer->comparing);  
                wait_event_interruptible(queueForWrite, !isBufferFull(currentBuffer));
                
            } else { 
                spin_unlock(currentBuffer->comparing);
            }
        }
        wake_up_interruptible(&queueForRead);
        printk(KERN_ALERT "buffer: written(wrPos: %d, readPos: %d, done: %d, remained: %d)\n", 
                                        currentBuffer->writePosition,
                                        currentBuffer->readPosition,
                                        countOfWrittenBytes,
                                        len - countOfWrittenBytes);
       /* currentBuffer->memory[currentBuffer->writePosition] = '\0';
        printk(KERN_INFO "buffer: LAST written(wrPos: %d, readPos: %d, done: %d, remained: %d, byte: %c)\n", 
                                        currentBuffer->writePosition,
                                        currentBuffer->readPosition,
                                        countOfWrittenBytes,
                                        len - countOfWrittenBytes,
                                        currentBuffer->memory[currentBuffer->writePosition]);*/
        
writeexit:  kfree(blockOfKernelMemory);
        mutex_unlock(&currentBuffer->writingMutex);
        wake_up_interruptible(&queueForRead);
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
    .write = my_write,
    .unlocked_ioctl = my_ioctl
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