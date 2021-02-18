#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <list>
#include <cstdio>
#include <exception>
#include <pthread.h>
#include "locker.h"
#include "sql_connection_pool.h"

template <class T>
class threadpool
{
    public:
    /*
    * 构造函数
    * @param actor_model 模型
    * @param connPool 数据库
    * @param thread_number 线程数
    * @param max_request 最大请求数
    */
    threadpool(connection_pool *connPool, int thread_number = 8, int max_request = 10000):m_thread_number(thread_number),m_max_requests(m_max_requests),m_stop(false),m_threads(NULL),m_connPool(connPool)
    {
        //参数异常
        if(thread_number <=0 || max_request <=0)
        {
            throw std::exception();
        }
        //创建线程
        m_threads = new pthread_t[m_thread_number];
        if(!m_threads)
        {
            throw std::exception();
        }
        for(int i=0;i<thread_number;++i)
        {
            /*
            * 创建线程的系统函数
            * @param m_threads+i 线程ID的指针
            * @param NULL 指向线程属性的指针，通常为NULL
            * @param worker 执行的函数指针
            * @param this worker的参数，让static函数拥有函数指针，上面的默认参数类型为(void* ),不能调用隐式传递this指针的参数。
            */
            if(pthread_create(m_threads+i,NULL,worker,this)!=0)
            {
                delete[] m_threads;
                throw std::exception(); 
            }
            //让子线程无需等待即可退出，将线程分离后，不用单独对工作线程进行回收。
            if(pthread_detach(m_threads[i])!=0)
            {
                delete[] m_threads;
                throw std::exception();
            }
        }
    }
    ~threadpool()
    {
        delete[] m_threads;
        m_stop=true;
    }
    /*往线程池的请求队列中添加任务
    * @param request 请求的任务
    * @para state 任务的类型
    */
    bool append(T *request,int state)
    {
        m_queuelocker.lock();
        if(m_workqueue.size()>=m_max_requests)
        {
            //超过上限则不加
            m_queuelocker.unlock();
            return false;
        }
        request->m_state = state;
        m_workqueue.push_back(request);
        m_queuelocker.unlock();
        //唤醒工作线程
        m_queuestat.post();
        return true;
    }
    /*
    * 将任务放入任务队列，不用指定类型
    * 其它同上
    */
    bool append_p(T* request)
    {
        m_queuelocker.unlock();
        if(m_workqueue.size()>=m_max_requests)
        {
            m_queuelocker.unlock();
            return false;
        }
        m_workqueue.push_back(request);
        m_queuelocker.unlock();
        m_queuestat.post();
        return true;
    }
    private:
        //线程池中的线程数
        int m_thread_number;
        //请求队列中允许的最大请求数量
        int m_max_requests;
        //描述线程池的数组，其大小为m_thread_number
        pthread_t *m_threads;
        //请求队列
        std::list<T*>m_workqueue;
        //保护请求队列的互斥锁
        locker m_queuelocker;
        //是否有任务处理
        sem m_queuestat;
        //数据库
        connection_pool *m_connPool;
        //是否结束线程
        bool m_stop;
        //工作模式
        //int m_actor_model;
        /*工作线程运行的函数，不断从工作队列中取出任务并执行
        * @param arg 函数指针
        */
        static void *worker(void* arg)
        {
            threadpool *pool = (threadpool *)arg;
            pool->run();
            return pool;
        }
        void run()
        {
            while(!m_stop)
            {
                //等待任务处理
                m_queuestat.wait();
                //上锁处理工作队列
                m_queuelocker.lock();
                //工作队列为空
                if(m_workqueue.empty())
                {
                    m_queuelocker.unlock();
                    continue;
                }
                //取出工作线程
                T* request = m_workqueue.front();
                m_workqueue.pop_front();
                m_queuelocker.unlock();
                if(!request)
                {
                    continue;
                }
                // if (1 == m_actor_model)
                // {
                //     if (0 == request->m_state)
                //     {
                //         if (request->read_once())
                //         {
                //             request->improv = 1;
                //             connectionRAII mysqlcon(&request->mysql, m_connPool);
                //             request->process();
                //         }
                //         else
                //         {
                //             request->improv = 1;
                //             request->timer_flag = 1;
                //         }
                //     }       
                //     else
                //     {
                //         if (request->write())
                //         {
                //             request->improv = 1;
                //         }
                //         else
                //         {
                //             request->improv = 1;
                //             request->timer_flag = 1;
                //         }
                //     }
                // }
                // else
                // {
                    connectionRAII mysqlcon(&request->mysql, m_connPool);
                    request->process();
                //}
            }
        }
};

#endif