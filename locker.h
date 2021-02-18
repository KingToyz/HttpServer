#ifndef LOCKER_H
#define LOCKER_H


#include <pthread.h>
#include <semaphore.h>
#include <exception>

class sem
{
    public:
        sem()
        {
            //信号量初始化
            //互斥
            if(sem_init(&m_sem,0,0)!=0)
            {
                throw std::exception();
            }
        }
        sem(int num)
        {
            //信号量初始化
            //指定初始信号量
            if(sem_init(&m_sem,0,num)!=0)
            {
                throw std::exception();
            }
        }
        ~sem()
        {
            //信号量销毁
            sem_destroy(&m_sem);
        }
        bool wait()
        {
            //P操作
            return sem_wait(&m_sem)==0;
        }
        bool post()
        {
            //V操作
            return sem_post(&m_sem)==0;
        }
    private:
        sem_t m_sem;
};

class locker
{
    //线程互斥量
    public:
        locker()
        {
            //初始化
            if(pthread_mutex_init(&m_mutex,NULL)!=0)
            {
                throw std::exception();
            }
        }
        ~locker()
        {
            //销毁
            pthread_mutex_destroy(&m_mutex);
        }
        bool lock()
        {
            //上锁
            return pthread_mutex_lock(&m_mutex)==0;
        }
        bool unlock()
        {
            //解锁
            return pthread_mutex_unlock(&m_mutex)==0;
        }
        pthread_mutex_t *get()
        {
            //获得锁情况
            return &m_mutex;
        }
    private:
        pthread_mutex_t m_mutex;
};

class cond
{
    //线程用条件变量
    public:
        cond()
        {
            //初始化
            if(pthread_cond_init(&m_cond,NULL)!=0)
            {
                throw std::exception();
            }
        }
        ~cond()
        {
            //销毁
            pthread_cond_destroy(&m_cond);
        }
        bool wait(pthread_mutex_t *m_mutex)
        {
            //P操作,上锁再操作条件量，条件变量检测前不被修改，虚假唤醒
            //pthread_cond_wait执行后内部操作分为：
            //1.将线程放在条件变量的请求队列，内部解锁（为了其它线程能够获得锁），放入队列后上锁是为了避免虚假唤醒
            //2.线程等待被pthread_cond_broadcast信号唤醒或者pthread_signal唤醒，唤醒后去竞争锁
            //3.竞争到锁后，内部再次加锁
            pthread_mutex_lock(m_mutex);
            int ret = pthread_cond_wait(&m_cond,m_mutex);
            pthread_mutex_unlock(m_mutex);
            return ret == 0;
        }
        bool timewait(pthread_mutex_t *m_mutex,struct timespec t)
        {
            //限时等待，信号唤醒或者超时，线程唤醒
            int ret = pthread_cond_timedwait(&m_cond,m_mutex,&t);
            return ret == 0;
        }
        bool signal()
        {
            //同样需要锁保护
            return pthread_cond_signal(&m_cond) == 0;
        }
        bool boardcast()
        {
            //广播唤醒所有等待目标条件的变量
            return pthread_cond_broadcast(&m_cond) == 0;
        }
    private:
     //static pthread_mutex_t m_mutex;
     pthread_cond_t m_cond;
};


#endif