#ifndef LOG_H
#define LOG_H

#include <stdio.h>
#include <iostream>
#include <string>
#include <stdarg.h>
#include <pthread.h>
#include "block_queue.h"
using namespace std;
class log
{
public:
    //C++11后，懒汉模式不需要加锁
    static log* get_instance()
    {
        static log instance;
        return &instance;
    }

    //可选择的参数有日志文件、日志缓冲区大小、最大行数以及最长日志队列
    bool init(const char* file_name,int log_buf_size = 8192,int split_lines = 5000000, int max_queue_size = 0);

    //异步写日志公有方法，调用私有方法async_write_log
    static void *fluse_log_thread(void *args)
    {
        log::get_instance()->async_write_log();
    }

    //将输出内容按照标准格式整理
    void write_log(int level,const char*format,...);

    //强制刷新缓冲区
    void flush(void);
private:
    log();
    ~log();

    //异步写日志方法
    void *async_write_log()
    {
        string single_log;

        //从阻塞队列中取出一条日志内容，写入文件
        while(m_log_queue->pop(single_log))
        {
            m_mutex.lock();
            //将字符串写入文件指针指向的FILE对象，FILE对象标识了要被写入字符串的流
            fputs(single_log.c_str(),m_fp);
            m_mutex.unlock();
        }
    }
    char dir_name[128]; //路径名
    char log_name[128]; //log文件名
    int m_split_lines;  //日志最大行数
    int m_log_buf_size; //日志缓冲区大小
    long long m_count;  //日志行数记录
    int m_today;        //按天分文件，记录当前时间是哪一天
    FILE *m_fp;         //打开的log的文件指针
    char *m_buf;        //要输出的内容
    block_queue<string> *m_log_queue;   //阻塞队列
    bool m_is_async;    //是否异步
    locker m_mutex;     //同步类 
};

//可变参数宏__VA_ARGS_
//加上##是为了防止没有参数的情况出现
#define LOG_DEBUG(format,...) log::get_instance()->write_log(0,format,##__VA_ARGS__);
#define LOG_INFO(format,...) log::get_instance()->write_log(1,format,##__VA_ARGS__);
#define LOG_WARN(format,...) log::get_instance()->write_log(2,format,##__VA_ARGS__);
#define LOG_ERROR(format,...) log::get_instance()->write_log(3,format,##__VA_ARGS__);


#endif