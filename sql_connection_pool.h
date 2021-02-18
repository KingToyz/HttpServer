#ifndef SQL_CONNECTION_POOL_H
#define SQL_CONNECTION_POOL_H

#include <stdio.h>
#include <list>
#include <mysql/mysql.h>
#include <string>
#include <string.h>
#include <iostream>
#include <error.h>
#include "locker.h"
using namespace std;
class connection_pool
{
public:
    //局部静态变量单例模式
    static connection_pool *GetInstance();

    //获取数据库连接
    MYSQL *GetConnection();
    //释放连接
    bool ReleaseConnction(MYSQL* conn);
    //获取连接
    int GetFreeConn();
    //销毁所有连接
    void DestoryPool();

    //初始化连接池
    void init(string url,string user,string password,string databasename,int port,unsigned int maxConn);

    connection_pool();
    ~connection_pool();
private:
    unsigned int MaxConn;//最大连接数
    unsigned int CurConn;//当前已使用的连接数
    unsigned int FreeConn;//当前空闲的连接数

    locker lock;
    list<MYSQL*>connList;//连接池
    sem reserve;

private:
    string url;//主机地址
    string port;//数据库端口号
    string user;//登录数据库用户名
    string password;//登录数据库密码
    string DatabaseName;//数据库名
};

class connectionRAII
{
    public:
        connectionRAII(MYSQL** con,connection_pool *connPool);
        ~connectionRAII();
    private:
        MYSQL* connRAII;
        connection_pool *poolRAII;
};


#endif
