#include <mysql/mysql.h>
#include <stdio.h>
#include <string>
#include <string.h>
#include <stdlib.h>
#include <list>
#include <pthread.h>
#include <iostream>
#include "sql_connection_pool.h"

using namespace std;

//单例模式
connection_pool *connection_pool::GetInstance()
{
    static connection_pool connPool;
    return &connPool;
}
connection_pool::connection_pool()
{
    this->CurConn=0;
    this->FreeConn=0;
}

//RALL机制销毁连接池
connection_pool::~connection_pool()
{
    DestoryPool();
}

//构造初始化
void connection_pool::init(string url,string user,string password,string databasename,int port,unsigned int maxConn)
{
    //初始化数据库信息
    this->url=url;
    this->user=user;
    this->password=password;
    this->DatabaseName=databasename;
    this->port=port;

    //创建MaxConn条数据库连接
    for(int i=0;i<maxConn;++i)
    {
        MYSQL* con=NULL;
        con=mysql_init(con);

        if(con==NULL)
        {
            cout<<"Error:"<<mysql_error(con);
            exit(1);
        }
        con=mysql_real_connect(con,url.c_str(),user.c_str(),password.c_str(),databasename.c_str(),port,NULL,0);

        if(con==NULL)
        {
            cout<<"Error:"<<mysql_error(con);
            exit(1);            
        }

        //更新连接池和空闲连接数
        connList.push_back(con);
        ++FreeConn;
    }

    //将信号量初始化为最大连接数
    reserve = sem(FreeConn);
    this->MaxConn = FreeConn;
}

//当有请求时候，从数据库连接池中返回一个可用的连接，更新使用和空闲连接数
MYSQL* connection_pool::GetConnection()
{
    MYSQL* con=NULL;
    if(connList.size()==0)
    {
        return NULL;
    }

    //取出连接，申请信号量
    reserve.wait();

    lock.lock();

    con = connList.front();
    connList.pop_front();

    --FreeConn;
    ++CurConn;

    lock.unlock();
    return con;
}

//释放当前使用的连接
bool connection_pool::ReleaseConnction(MYSQL *conn)
{
    if(conn==NULL)
        return false;
    lock.lock();

    connList.push_back(conn);
    ++FreeConn;
    --CurConn;

    lock.unlock();
    reserve.post();

    return true;
}

//销毁数据库连接池
void connection_pool::DestoryPool()
{
    lock.lock();
    if(connList.size()>0)
    {
        //遍历关闭数据库连接
        for(auto it=connList.begin();it!=connList.end();++it)
        {
            MYSQL *con=*it;
            mysql_close(con);
        }
        CurConn=0;
        FreeConn=0;

        connList.clear();

        lock.unlock();
        return;
    }
    lock.unlock();
}

//双指针对指针本身进行修改（而非指向的）
connectionRAII::connectionRAII(MYSQL** con,connection_pool *connPool)
{
    *con = connPool->GetConnection();
    connRAII  = *con;
    poolRAII = connPool;
}

connectionRAII::~connectionRAII()
{
    poolRAII->ReleaseConnction(connRAII);
}