#ifndef BLOCK_QUEUE_H
#define BLOCK_QUEUE_H

#include <iostream>
#include <stdlib.h>
#include <pthread.h>
#include <sys/time.h>
#include "locker.h"

/**
 *  循环数组实现的阻塞队列。m_back=(m_back+1)%m_max_size
 * 线程安全，每次操作前都要互斥锁，操作完后解锁 
 * 
*/

template <class T>
class block_queue
{
public:
    //初始化私有成员
    block_queue(int max_size=1000)
    {
        if(max_size<=0)
        {
            exit(-1);
        }

        //构造函数创建循环数组
        m_max_size = max_size;
        m_array = new T[max_size];
        m_size = 0;
        m_front = -1;
        m_back = -1;

    }

    void clear()
    {
        m_mutex.lock();
        m_size = 0;
        m_front = -1;
        m_back = -1;
        m_mutex.unlock();
    }

    ~block_queue()
    {
        m_mutex.lock();
        if(m_array!=NULL)
        {
            delete [] m_array;
        }
        m_mutex.unlock();
    }

    //判断队列是否满了
    bool full()
    {
        m_mutex.lock();
        if(m_size>=m_max_size)
        {
            m_mutex.unlock();
            return true;
        }
        m_mutex.unlock();
        return false;
    }

    //返回队首元素
    bool front(T &value)
    {
        m_mutex.lock();
        if(m_size==0)
        {
            m_mutex.unlock();
            return false;
        }
        value = m_array[m_front+1];
        m_mutex.unlock();
        return true;

    }
    //返回队尾元素
    bool back(T& value)
    {
        m_mutex.lock();
        if(m_size==0)
        {
            m_mutex.unlock();
            return false;
        }
        value = m_array[m_back];
        m_mutex.unlock();
        return true;
    }

    int size()
    {
        int tmp=0;
        m_mutex.lock();
        tmp=m_size;
        m_mutex.lock();
        return tmp;
    }

    int max_size()
    {
        int tmp = 0;
        m_mutex.lock();
        tmp=max_size;
        m_mutex.unlock();
        return tmp;
    }
    //往队列添加元素，需要将所以使用队列的线程先唤醒
    //当右元素push进队，相当于生产者生产了一个元素
    //若当前没有线程等待条件变量，则唤醒无意义
    bool push(const T &item)
    {
        m_mutex.lock();
        if(m_size>=m_max_size)
        {
            m_cond.boardcast();
            m_mutex.unlock();
            return false;
        }

        //将新增数据放在循环数组的对应位置
        m_back = (m_back+1)%m_max_size;
        m_array[m_back]=item;
        m_size++;

        m_cond.boardcast();
        m_mutex.unlock();

        return true;
    }
    
    //pop时，如果当前队列没有元素，将会等待条件变量
    bool pop(T &item)
    {
        m_mutex.lock();

        //多个消费者的时候，这里使用while。
        //如果使用if无法让取得锁但无法取得资源的线程继续等待
        while(m_size<=0)
        {
            //当重新抢到互斥锁，pthread_cond_wait返回为0
            //pthread_cond_wait中先将线程放入等待队列，解锁，等待是否唤醒。获得资源后再上锁
            if(m_cond.wait(m_mutex.get())!=0)
            {
                m_mutex.unlock();
                return false;
            }
        }

        //取出队首的元素
        m_front = (m_front+1)%m_max_size;
        item = m_array[m_front];
        m_size --;
        m_mutex.unlock();
        return true;
    }

    //在pthread_cond_wait基础上增加了等待的时间，只指定时间内能抢到互斥锁即可。
    //其它逻辑不变
    bool pop(T &item,int ms_timeout)
    {
        struct timespec t = {0,0};
        struct timeval now =  {0,0};
        gettimeofday(&now,NULL);
        m_mutex.lock();
        if(m_size<=0)
        {
            t.tv_sec = now.tv_sec+ms_timeout/1000;
            t.tv_nsec = (ms_timeout%1000)*1000;
            if(m_cond.timewait(m_mutex.get(),t))
            {
                m_mutex.unlock();
                return false;
            }
        }
        if(m_size<=0)
        {
            m_mutex.unlock();
            return false;
        }

        m_front = (m_front+1)%m_max_size;
        item = m_array[m_front];
        m_size--;
        m_mutex.unlock();
        return true;
    }

private:
    locker m_mutex;
    cond m_cond;

    T* m_array;
    int m_size;
    int m_max_size;
    int m_front;//队首的前一个位置
    int m_back;//队末的位置
};







#endif