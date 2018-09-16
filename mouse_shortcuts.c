/*
 *@brief 我们的鼠标有左键，中键，右键。鼠标快捷键的设计:
 *  中键表示开始和结束，在开始和结束之间统计左键的次数，根据这个次数来运行我们的程序。
 *  并且这里加上一个超时时间，就是按键的时间间隔是10秒内有效，超过时秒就作废。
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/input.h>
#include <syslog.h>
#include <assert.h>
#include <errno.h>
#include <sys/epoll.h>
#include <sys/wait.h>
#include <sys/param.h>
/*定义日志接口*/
#define ldebug(fmt, arg...)  syslog(LOG_USER, "[%s][DEBUG] "fmt,  __FUNCTION__,  ##arg)
#define linfo(fmt, arg...)   syslog(LOG_USER, "[%s][INFO] "fmt, __FUNCTION__,  ##arg)
#define lerror(fmt, arg...)  syslog( LOG_USER, "[%s][ERR] "fmt,  __FUNCTION__,  ##arg)
/***********/

/*鼠标快捷键配置文件*/
#define MOUSE_SHORTCUTS_CONFIG "/etc/mouse_shortcuts.conf"

/*定义结构体：鼠标左键次数统计*/
typedef struct _left_count {
    int start; //鼠标中间按钮按下，表示开始和结束
    int left_count; //鼠标左键的次数统计
    struct timeval timeval_last; //最后一次鼠标事件统计，如果超时本次统计作废
} left_count;

/*定义结构体：关联鼠标左键次数和我们的要执行命令*/
typedef struct _shortcut {
    int left_count;
#define MAX_COMMAND_STR_LEN (128)
    char command[MAX_COMMAND_STR_LEN];
} shortcut;


#define MAX_EVENT_NAME_LEN (128)
#define BITS_PER_BYTE        8
#define BITS_PER_LONG        64
#define BITS_TO_LONGS(nr)    DIV_ROUND_UP(nr, BITS_PER_BYTE * sizeof(long))
#define DIV_ROUND_UP(n,d) (((n) + (d) - 1) / (d))
#define test_bit(bit, array)  ((array)[(bit)/BITS_PER_LONG] & (1 << ((bit) % BITS_PER_LONG)))




/*这里是根据鼠标左键的次数，对应执行命令。配置最多定义10种*/
#define MAX_MOUSE_SHORTCUTS (10)
shortcut g_shortcuts[MAX_MOUSE_SHORTCUTS];



/*定义鼠标事件类型*/
typedef enum _mouse_event 
{
    EVENT_UNKNOWN = 0,
    EVENT_LEFT,
    EVENT_MIDDLE,
    EVENT_RIGHT,
} mouse_event;

/**
 * @brief 加载快捷键配置，配置文件定义鼠标左键次数和对应执行的操作
 * @param void
 * @return 成功返回成功加载的配置数目，加载的失败返回-1
 * @notes 文件格式必须是   鼠标左键次数:执行的程序名字
**/
int load_shortcuts_conf(void)
{
    FILE *stream = NULL;
    int index = 0;
    size_t len = 0;
    ssize_t read = 0;
    char *line = NULL;
    
    char *token = NULL; 
    char *saveptr = NULL;

    stream = fopen(MOUSE_SHORTCUTS_CONFIG, "r");
    if (stream == NULL)
    {
        lerror("fopen %s err:%s\n", MOUSE_SHORTCUTS_CONFIG, strerror(errno));
        return -1;
    }
    while ((read = getline(&line, &len, stream)) != -1) {
        if (line == NULL || line[0] == '\0' || line[0] == '#' || read < 1)
           continue;
        if ( line[read-1] == '\n' )
            line[read-1] = '\0';

        //first item
        token = strtok_r(line, ":", &saveptr);
        if (token == NULL || saveptr == NULL)
            continue;

        g_shortcuts[index].left_count = atoi(token);
        snprintf(g_shortcuts[index].command, MAX_COMMAND_STR_LEN-1, "%s", saveptr);
        ++index;
        /*至少保留一个结尾*/
       if (index == MAX_COMMAND_STR_LEN -1)
           break;    
    }
    free(line);
    fclose(stream);
    
    assert(index < MAX_COMMAND_STR_LEN);
    g_shortcuts[index].left_count = -1; //END

    return index;
}

/**
 * @brief 根据鼠标左键的数目对应获取 鼠标快捷事件 数组的下标
 * @paran left_count 鼠标左键数目
 * @return 鼠标快捷事件 数组的下标,失败返回-1
 **/
int index_shortcuts_by_leftcount(const int left_count)
{
    assert(left_count >= 0);
    int index = 0;
    for(index= 0;;++index)
    {
        if (g_shortcuts[index].left_count == left_count)
            return index;
        ldebug("index %d, left_count:%d\n", index, g_shortcuts[index].left_count);
        if (g_shortcuts[index].left_count == -1)
        {
            break;
        }

    }
    return -1;
}

/**
 * @brief 判断是否是鼠标的input event句柄
 * @param /dev/input/eventX fd
 * @return 失败返回-1,成功返回鼠标对应的input event 句柄
 * @notes 只要这个input event具有 EV_REL 和 EV_KEY ，我们就认为是鼠标,返回1.失败或者不是就返回0
**/
static int is_mouse_inputdev(const int fd)
{
    assert(fd > 0);
    unsigned long evtype_b[BITS_TO_LONGS(EV_MAX)] = {0};
    
    memset(evtype_b, 0, sizeof(evtype_b));
    if (ioctl(fd, EVIOCGBIT(0, EV_MAX), evtype_b) < 0)
    {
        lerror("ioctl EVIOCGBIT err:%s\n", strerror(errno));
        return 0;
    }

    if (test_bit(EV_REL, evtype_b) && test_bit(EV_KEY, evtype_b))
    {
        return 1;
    }
    
    return 0;
}

/**
 * @brief 打开鼠标的event句柄
 * @param void
 * @return 失败返回-1,成功返回鼠标对应的input event 句柄
**/
int open_mouse_inputdev(void)
{
    int i = 0;
	int fd = -1;
    char dev_name[MAX_EVENT_NAME_LEN] = {0};
    char inputdev_path[MAX_EVENT_NAME_LEN] = {0};

#define MAX_NR_EVENT_DEV (1000)    
    for (i = 0; i < MAX_NR_EVENT_DEV; i++)
    {
        memset(dev_name, 0, MAX_EVENT_NAME_LEN);
        memset(inputdev_path, 0, MAX_EVENT_NAME_LEN);
        snprintf(dev_name, MAX_EVENT_NAME_LEN-1, "/dev/input/event%d", i);
        //文件不存在，直接跳过
        if (access(dev_name,F_OK) != 0)
            continue;

        if ((fd = open(dev_name, O_RDONLY)) < 0) {
            lerror("open %s err:%s\n", dev_name, strerror(errno));
            continue;
        }

        if (is_mouse_inputdev(fd))
        {
            linfo("find mouse:%s\n", dev_name);
            return fd;
        }

        close(fd);
    }
    
    return -1;
}

/**
 * @brief 关闭据表句柄
 * @param fd 鼠标句柄
 * @return void
 **/
static void close_mouse_inputdev(int fd)
{
    assert(fd > 0);
    close(fd);
}

/**
 * @brief 重置鼠标计数
 * @param left_count 鼠标计数结构体
 * @return void
 **/
void reset_left_count(left_count *left_count)
{
    assert(left_count != NULL);
    left_count->start = 0;
    left_count->timeval_last.tv_sec = 0;
    left_count->left_count = 0;
}

/**
 * @brief 返回路径字符串文件名的头指针
 * @param path 路径字符串
 * @param filename 提取的文件名字
 * @param maxlen_filename 文件名字允许最大长度
 * @return void
 **/
void _basename(const char *path, char *filename_str, int maxlen_filename)
{
    assert(path != NULL);
    assert(maxlen_filename > 0);
    const char *begin = path;
    const char *end = begin;
    for(;;)
    {
        //printf("%c\n", )
        if (*end == '\0' || *begin == '\0')
            break;
        if (*end == '/')
               begin = end + 1;
        ++end;
    }
    snprintf(filename_str, maxlen_filename - 1, "%s", begin);
}

/**
 * @brief 根据按下鼠标左键的数目执行程序
 * @param left_count 统计鼠标左键的数目
 * @return
 **/
static int excute_by_left_count(const int left_count)
{
    assert(left_count >= 0);   
    int index = index_shortcuts_by_leftcount(left_count);
    if (index == -1)
    {
        lerror("no find mlc_handles for left_count %d\n", left_count);
        return -1;
    }
    
    //创建子进程
    if (fork() == 0)
    {
        char filename[MAX_COMMAND_STR_LEN] = {0};
        _basename(g_shortcuts[index].command, filename, MAX_COMMAND_STR_LEN);
        ldebug("execlp %s\n",g_shortcuts[index].command);
        if (execlp("/bin/sh", "sh", "-c", g_shortcuts[index].command, NULL) <0 ){
            lerror("execlp %s err %s\n", g_shortcuts[index].command, strerror(errno));
            exit(EXIT_FAILURE);
        }
    }
}

/**
 * @brief 任何一个快捷键执行的命令都是一个子进程，这里有必要捕获一下任何子进程退出的信息
 * @param void
 * @return  void
 **/
void waitpid_shorcuts_task_noblock(void)
{
	pid_t w;
    int wstatus;
	/*不阻塞等待，但是还是捕获一下任何子进程退出的信息*/
	w = waitpid(-1, &wstatus, WUNTRACED | WCONTINUED | WNOHANG);
	if (w == -1) {
	   lerror("waitpid err %s\n", strerror(errno));
	   return;
	}
	if (WIFEXITED(wstatus)) {
	   lerror("exited(%d), status=%d\n", w, WEXITSTATUS(wstatus));
	} else if (WIFSIGNALED(wstatus)) {
	   lerror("killed(%d) by signal %d\n", w, WTERMSIG(wstatus));
	} else if (WIFSTOPPED(wstatus)) {
	   lerror("stopped(%d) by signal %d\n", w, WSTOPSIG(wstatus));
	} else if (WIFCONTINUED(wstatus)) {
	   linfo("continued(%d)\n",w);
	}
}

/**
 * @brief 处理鼠标事件
 * @param lc 统计鼠标左键的数目
 * @param cur_ie 当前的鼠标事件
 * @return  void
 **/
static void process_mouse_event(left_count *lc,const struct input_event cur_ie)
{
    assert(lc != NULL);

    /*只关注按键*/
    if (cur_ie.type != EV_KEY)
        return;
    /*只关注按下*/
    if (cur_ie.value != 1)
        return;
    /*事件超时直接跳过*/
    if (lc->timeval_last.tv_sec > 0 
        && (cur_ie.time.tv_sec - lc->timeval_last.tv_sec) > 2)
    {
        ldebug("reset_left_count\n");
        reset_left_count(lc);
    }

    /*按下鼠标中间按钮，表示开始或者结束统计鼠标左按键事件*/
    if (cur_ie.code == BTN_MIDDLE)
    {
        if (lc->start == 0)
        {
            //开始
            ldebug("start count lc\n");
            lc->start = 1;            
        }
        else
        {
            //结束
            ldebug("end count left_count.left_count:%d\n",lc->left_count);
            //执行快捷键对应的命令
            excute_by_left_count(lc->left_count);
			waitpid_shorcuts_task_noblock();
            reset_left_count(lc);
        }
        lc->timeval_last.tv_sec = cur_ie.time.tv_sec;
        return;
    }
  
    /*统计鼠标左键次数*/
    if (cur_ie.code == BTN_LEFT && lc->start == 1)
    {
        ++lc->left_count;
        lc->timeval_last.tv_sec = cur_ie.time.tv_sec;
    }
    return;
}

/**
 * @brief 这是我们的主流程，监听鼠标事件，根据事件执行我们的操作
 * @param fd 鼠标input event fd
 * @return  void
 **/
void loop_process_main(const int fd)
{
    assert(fd > 0);
    struct epoll_event ev;
    struct epoll_event event;
    char buf[1024] = {0};
    struct input_event ie;
    left_count lc;
    reset_left_count(&lc);
    //初始化epoll
    int epfd = epoll_create(1);
    if (epfd < 0)
    {
        lerror("epoll_create err :%s", strerror(errno));
        return;
    }    
    ev.data.fd = fd;
    ev.events = EPOLLIN;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) != 0)
    {
        lerror("epoll_ctl err:%s\n", strerror(errno));
        goto _exit;
    }

    //开始监听鼠标事件
    for (;;)
    {
         /*有鼠标事件*/
        if ((epoll_wait(epfd, &event, 1, -1) > 0) && (event.data.fd == fd))
        {
            ldebug("has mouse input dev event\n");
            //读取虚拟机事件
            if (read(fd, &ie, sizeof(struct input_event)) < 0)
            {
                lerror("read mouse event err");
                continue;
            }
            process_mouse_event(&lc, ie);
        }
        /*异常处理*/
        else if (errno == EINTR)
        {
            continue;
        }
        /*其他直接报错退出*/
        else
        {
            lerror("epoll_wait err :%s", strerror(errno));
            goto _exit;            
        }
    }
 
_exit:
    close(epfd);
    return;
}

/*
 * @brief 成为后台服务
 * @param void
 * @return 成功返回0,失败返回-1
 */
int enter_daemon(void)
{
    int  i,fd0, fd1, fd2; 
    pid_t pid = fork();
    if (pid < 0) {
        lerror("fork err:%s\n", strerror(errno));
        return -1;
    }
    /*父进程退出*/
    if (pid > 0) {
        exit(EXIT_SUCCESS);
    }
    /* Create a new SID for the child process */
    if (setsid() < 0) {
        lerror("setsid err:%s\n", strerror(errno));
        return -1;
    }      
    /* Change the current working directory */
    if ((chdir("/")) < 0) {
        lerror("chdir err:%s\n", strerror(errno));
        return -1;
    }
     
    for(i=0;i< NOFILE;close(i++));
    umask(0);
    /* Initialize the log file.	*/
    openlog(NULL, LOG_CONS, LOG_DAEMON);
    return 0;
}

/**
 * @brief 主函数
**/
int main()
{

    /*后台服务*/
    if(enter_daemon() != 0)
    {
        lerror("enter_daemon err");
        return -1;
    }

    /*加载快捷键配置*/
    linfo("load_shortcuts_conf ...\n");
    if(load_shortcuts_conf() < 1)
    {
        lerror("load_shortcuts_conf err");
        return -1;
    }

    /*打开鼠标设备*/
    linfo("open_mouse_inputdev ...\n");
	int fd =  open_mouse_inputdev();
    if (fd < 1)
    {
        lerror("open_mouse_inputdev err");
        return -1;
    }

    /*循环统计鼠标事件和执行快捷键对应的命令*/
    linfo("loop_process_main ...\n");
    loop_process_main(fd);

    /*关闭句柄*/
    linfo("close_mouse_inputdev ...\n");
    close_mouse_inputdev(fd);
	return 0;
}
