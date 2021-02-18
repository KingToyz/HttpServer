#ifndef LST_TIMER_H
#define LST_TIMER_H

#include <time.h>
#include "log.h"

//定时器设计
//项目中将连接资源、定时事件和超时时间封装为定时器类，具体的
//1.连接资源包括客户端套接字地址、文件描述符和定时器
//2.定时事件为回调函数，将其封装起来由用户自定义，这里是删除非活动socket上的注册事件，并关闭。
//3.定时器超时时间=浏览器和服务器连接时刻+TIMESLOT,可以看出，定时器使用绝对时间作为超时值，这里alarm设置为5s,连接超时为15s

class util_timer;
//前置声明

//连接资源
struct client_data
{
    //客户端socket地址
    sockaddr_in address;

    //socket文件描述符
    int sockfd;

    //定时器
    util_timer* timer;
};

//定时器类
class util_timer
{
    public:
        util_timer():prev(NULL),next(NULL){}
    
    public:
        //超时时间
        time_t expire;
        //回调函数
        void (*cb_func)(client_data*);
        //连接资源
        client_data* user_data;
        //前向定时器
        util_timer* prev;
        //后继定时器
        util_timer* next;
};

//定时容器类
//定时器容器为带头尾节点的升序双向链表，具体的为每一个连接创建一个定时器，将其添加到
//链表中，并按照超时时间升序排列。执行定时任务时，将到期的定时器从链表中删除
//升序双向链表主要逻辑如下，具体的
//1.创建头尾节点，但是没有意义
//2.add_timer函数，将目标定时器添加到链表中，添加时按照升序添加
//  1.若链表中只有头尾节点，直接插入
//  2.否则将定时器按升序插入
//3.adjust_timer函数，当定时任务发送变化，调整对应定时器在链表中的位置
//  1.客户端在设定时间内有数据收发，则当前时刻对该定时器重新设定时间，这里只是往后延长超
//时时间
//  2.被调整的目标定时器在尾部，或定时器新的超时值仍然小于下一个定时器的超时时间，不用调整
//  3.否则先将定时器从链表中取出，重新插入链表
//4.del_timer函数将超时的定时值从链表中删除
//  1.常规双向链表删除节点
class sort_timer_lst
{
    public:
        sort_timer_lst():head(NULL),tail(NULL){}
        //常规销毁链表
        ~sort_timer_lst()
        {
            util_timer* tmp = head;
            while(tmp)
            {
                head = tmp->next;
                delete tmp;
                tmp=head;
            }
        }

        //添加定时器，内部调用私有成员add_timer
        void add_timer(util_timer* timer)
        {
            if(!timer)
            {
                return;
            }
            if(!head)
            {
                head = tail = timer;
                return;
            }

            //如果新的定时器超时时间小于当前头部节点
            //直接将当前定时器结点作为头部节点
            if(timer->expire<=head->expire)
            {
                timer->next = head;
                head->prev=timer;
                head=timer;
                return;
            }

            //否则调用私有成员，调整内部节点
            add_timer(timer,head);
        }

        //调整定时器，任务发生变化时，调整定时器在链表中的位置
        void adjust_timer(util_timer* timer)
        {
            if(!timer)
            {
                return;
            }
            util_timer* tmp = timer->next;

            //被调整的定时器在链表尾部
            //定时器的超时值仍然小于笑一个定时器的超时值，不调整
            if(!tmp||(timer->expire<=tmp->expire))
            {
                return;
            }

            //被调整定时器是链表头节点，将定时器取出，重新插入
            if(timer==head)
            {
                head=head->next;
                head->prev=NULL;
                timer->next=NULL;
                add_timer(timer,head);
            }

            //被调整定时器在内部，将定时器取出，重新插入
            else
            {
                timer->prev->next=timer->next;
                timer->next->prev=timer->prev;
                add_timer(timer,timer->next);
            }
            
        }

        //删除定时器
        void del_timer(util_timer* timer)
        {
            if(!timer)
            {
                return;
            }

            //链表中只有一个定时器，需要删除该定时器
            if((timer==head)&&(timer==tail))
            {
                delete timer;
                head=NULL;
                tail=NULL;
                return;
            }

            //被删除的定时器为头节点
            if(timer==head)
            {
                head->next->prev=NULL;
                head=head->next;
                delete timer;
                return;
            }

            //被删除的定时器为尾节点
            if(timer==tail)
            {
                tail=tail->prev;
                tail->next=NULL;
                delete timer;
                return;
            }

            //被删除的定时器在链表内部，常规链表节点删除
            timer->prev->next=timer->next;
            timer->next->prev=timer->prev;
            delete timer;
        }
        //定时任务处理函数
        //使用统一事件源，SIGALRM信号每次被触发，主循环中调用一次定时任务处理函数
        //处理链表容器中到期的定时器
        //具体逻辑
        //1.遍历定时器升序链表容器，从头节点开始处理每一个定时器，直到遇到尚未到期的定时器
        //2.若当前时间小于定时器超时时间，即找到未到期的定时器
        //3.若大于定时器超时时间，即是找到了到期的定时器，执行回调函数，然后将它从链表中删除
        void tick()
        {
            if(!head)
            {
                return;
            }

            //获取当前时间
            time_t cur = time(NULL);
            util_timer* tmp = head;

            //遍历定时链表
            while(tmp)
            {
                //链表容器为升序排列
                //当前时间小于定时器的超时时间，后面的定时器没有到期
                if(cur<tmp->expire)
                {
                    break;
                }

                //当前定时器到期，则调用回调函数
                tmp->cb_func(tmp->user_data);

                //将处理后的定时器从链表容器中删除，并重置头节点
                head=tmp->next;
                if(head)
                {
                    head->prev=NULL;
                }
                delete tmp;
                tmp=head;
            }
        }

    private:
        //私有成员，被公有成员add_timer和adjust_timer调用
        //主要用于调整链表内部节点
        void add_timer(util_timer* timer,util_timer* last_head)
        {
            util_timer* prev =last_head;
            util_timer* tmp = prev->next;
            
            //遍历当前节点之后的节点，按照超时时间找到目标定时器对应的位置再进行插入
            while(tmp)
            {
                if(timer->expire<=tmp->expire)
                {
                    prev->next=timer;
                    timer->next=tmp;
                    tmp->prev=timer;
                    timer->prev=prev;
                    return;
                }
                prev=tmp;
                tmp=tmp->next;
            }

            //遍历完发现要插入结尾处
            if(!tmp)
            {
                prev->next=timer;
                timer->next=NULL;
                timer->prev=prev;
                tail=timer;
            }
        }

    private:
        //头尾节点
        util_timer* head;
        util_timer* tail;
};















#endif