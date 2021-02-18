#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <stdarg.h>
#include <pthread.h>
#include "block_queue.h"
#include "log.h"
using namespace std;

log::log()
{
    m_count=0;
    m_is_async=false;
}

log::~log()
{
    if(m_fp!=NULL)
    {
        fclose(m_fp);
    }
}

/*
 *init函数实现日志创建、写入方式的判断
 *  初始化生成日志文件，服务器启动按当前时刻创建日志，前缀为时间，后缀为
 * 自定义的log文件名，并记录创建日志的时候day和行数count
 * @param file_name 日志绝对路径名
 * @param log_buf_size 日志缓冲区大小
 * @param split_lines 日志最大行数
 * @param max_queue_size 异步队列长度，异步需要设置阻塞队列的大小，同步不需要
*/
bool log::init(const char* file_name,int log_buf_size,int split_lines,int max_queue_size)
{
    //如果设置了max_queue_size，则设置为异步
    if(max_queue_size>=1)
    {
        m_is_async = true;

        //创建设置阻塞队列的长度
        m_log_queue = new block_queue<string>(max_queue_size);
        pthread_t tid;

        //flush_log_thread为回调函数，这里是表示创建线程异步日志
        pthread_create(&tid,NULL,fluse_log_thread,NULL);
    }

    //输出内容长度
    m_log_buf_size = log_buf_size;
    m_buf = new char[m_log_buf_size];
    memset(m_buf,'\0',sizeof(m_buf));

    //日志最大行数
    m_split_lines = split_lines;

    time_t t = time(NULL);
    struct tm *sys_tm = localtime(&t);
    struct tm my_tm = *sys_tm;

    //从后往前找到第一个/的位置
    const char* p = strrchr(file_name,'/');
    char log_full_name[256]={0};

    //相当于自定义日志名
    //若输入的文件名没有/，则直接将时间+文件名作为日志名
    if(p==NULL)
    {
        snprintf(log_full_name,255,"%d_%02d_%02d_%s",my_tm.tm_year+1900,my_tm.tm_mon+1,my_tm.tm_mday,file_name);
    }
    else
    {
        //将/的位置向后移动一个位置，然后复制到logname中
        //p -file_name +1是文件所在路径文件夹的长度
        strcpy(log_name,p+1);
        strncpy(dir_name,file_name,p-file_name+1);

        //后面的参数和format有关
        snprintf(log_full_name,255,"%s%d_%02d_%02d_%s",dir_name,my_tm.tm_year+1900,my_tm.tm_mon+1,my_tm.tm_mday,log_name);

    }

    m_today = my_tm.tm_mday;
    m_fp = fopen(log_full_name,"a");
    if(m_fp==NULL)
    {
        return false;
    }
    return true;
}

/* 日志分级：
 * 1.DeBug，调试代码时的输出，在系统实际运行的时候一般不适应
 * 2.Warn，这种警告与调试时终端的warning类似，统一是调试代码时使用。
 * 3.Info,报告系统当前的状态，当前执行的流程或接受的信息等
 * 4.Error和fatal。输出系统的错误信息
 * 日志写入前会判断当前day是否为创建日志的实际，行数是否超过最大行限制
 * 1.若为创建日志实际，写入日志，否则按当前实际创建新log,更新创建时间和行数
 * 2.若行数超过最大行限制，则在当前日志的末尾加上count/max_lines为后缀创建新log
 * @param level。日志分级。
 * @param format。输入格式
 * @param ...。输入内容
 */
void log::write_log(int level,const char* format,...)
{
    struct timeval now = {0,0};
    gettimeofday(&now,NULL);
    time_t t = now.tv_sec;
    struct tm *sys_tm =localtime(&t);
    struct tm my_tm = *sys_tm;
    char s[16]={0};

    //日志分级
    switch (level)
    {
        case 0:
            strcpy(s,"[debug]:");
            break;
        case 1:
            strcpy(s,"[info]:");
            break;
        case 2:
            strcpy(s,"[warn]:");
            break;
        case 3:
            strcpy(s,"[error]:");
            break;
        default:
            strcpy(s,"[info]:");
            break;
    }

    m_mutex.lock();
    
    //更新现有行数
    m_count++;

    //日志不是今天或者写入的日志行数是最大行的倍数
    //m_split_lines为最大行数
    if(m_today!=my_tm.tm_mday||m_count%m_split_lines==0)
    {
        char new_log[256]={0};
        fflush(m_fp);
        fclose(m_fp);
        char tail[16] = {0};

        //格式化日志名中的时间部分
        snprintf(tail,16,"%d_%02d_%02d_",my_tm.tm_year+1900,my_tm.tm_mon+1,my_tm.tm_mday);

        //如果时间不是今天，则创建今天的日志，更新m_today和m_count
        if(m_today!=my_tm.tm_mday)
        {
            snprintf(new_log,255,"%s%s%s",dir_name,tail,log_name);
            m_today = my_tm.tm_mday;
            m_count=0;
        }
        else
        {
            //超过了最大行，在之前的日志名之前加上后缀，m_count/m_split_lines
            snprintf(new_log,255,"%s%s%s.%lld",dir_name,tail,log_name,m_count/m_split_lines);
        }
        m_fp = fopen(new_log,"a");
    }
    m_mutex.unlock();

    va_list valst;
    //将传入的format参数赋值给valst,便于格式化输出
    va_start(valst,format);

    string log_str;
    m_mutex.lock();

    //写入内容格式化：时间+内容
    //时间格式化，snprintf成功返回写字符的总数，其中不包括结尾的null字符
    int n = snprintf(m_buf,48,"%d-%02d-%02d %02d:%02d:%02d.%06ld %s ",
    my_tm.tm_year+1900,my_tm.tm_mon+1,my_tm.tm_mday,my_tm.tm_hour,my_tm.tm_min,my_tm.tm_sec,now.tv_usec,s);

    //内容格式化，用于向字符串中打印数据、数据格式用户自定义，返回写入到字符数组str中的字符个数(不包含终止符)
    int m = vsnprintf(m_buf+n,m_log_buf_size-1,format,valst);
    m_buf[n+m] = '\n';
    m_buf[n+m+1] = '\0';

    log_str = m_buf;
    m_mutex.unlock();

    //若m_is_async为true表示为异步，否则同步
    //若异步，则将日志信息写入阻塞队列，同步则加锁向文件写
    if(m_is_async&&!m_log_queue->full())
    {
        m_log_queue->push(log_str);
    }
    else
    {
        m_mutex.lock();
        fputs(log_str.c_str(),m_fp);
        m_mutex.unlock();
    }
    va_end(valst);
}

void log::flush(void)
{
    m_mutex.lock();
    //强制刷新写入流缓冲区
    fflush(m_fp);
    m_mutex.unlock();
}