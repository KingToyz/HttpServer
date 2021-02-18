#include "http_conn.h"
#include "log.h"
#include <unordered_map>
#include <fstream>
#include <fcntl.h>

#define ET
//定义一些http响应的一些状态信息
const char* ok_200_title = "OK";
const char* error_400_title =  "Bad Request";
const char* error_400_form = "Your request has bad synatx or is iherently impossible to satisfy.\n";
const char* error_403_title = "Forbidden";
const char* error_403_form = "You do not have permission to get file from this server.\n";
const char* error_404_title = "Not Found";
const char* error_404_form = "The requested file was not found on this server.\n";
const char* error_500_title = "Internal Error";
const char* error_500_form = "There was an unusual problem serving the request file.\n";

//载入数据库表
//将数据库中的用户名和密码载入服务器的map中，map的key为用户名，value为密码
unordered_map<string,string>users;
locker m_lock;

void http_conn::initmysql_result(connection_pool *connPool)
{
    //先从连接池中取一个连接
    MYSQL* mysql=NULL;
    connectionRAII mysqlcon(&mysql,connPool);
    
    //在user表中检索username，passwd数据，浏览器输入
    if(mysql_query(mysql,"SELECT username,passwd FROM user"));
    {
        LOG_ERROR("SELECT error:%s\n",mysql_error(mysql));
    }

    //从表中检索完整的结果集
    MYSQL_RES *result = mysql_store_result(mysql);

    //返回结果集中的列数
    int num_fields = mysql_num_fields(result);

    //返回所有字段结构的数组
    MYSQL_FIELD *fields = mysql_fetch_field(result);

    //从结果集中获取下一行，将对应的用户名和密码存入map中
    while(MYSQL_ROW row = mysql_fetch_row(result))
    {
        string temp1(row[0]);
        string temp2(row[1]);
        users[temp1]=temp2;
    }
}


//循环读取客户数据，直到无数据可读或对方关闭连接
bool http_conn::read_once()
{
    //最后的数据超出范围
    if(m_read_idx>=READ_BUFFER_SIZE)
    {
        return false;
    }
    int bytes_read = 0;
    while(1)
    {
        //从套接字收取数据，存储在m_read_buf缓冲区
        bytes_read = recv(m_sockfd,m_read_buf+m_read_idx,READ_BUFFER_SIZE-m_read_idx,0);
        if(bytes_read==-1)
        {
            //非阻塞ET模式下，需要一次性将数据读完直到出现EAGAIN，使用非阻塞IO，应用马上处理事件。
            if(errno==EAGAIN||errno==EWOULDBLOCK)
                break;
            return false;
        }
        //没有读到数据
        else if(bytes_read==0)
        {
            return false;
        }
        //修改m_read_idx的读取字节数
        m_read_idx+=bytes_read;
    }
    return true;   
}

//epoll相关代码
//对文件描述符设置非阻塞
int setnonblocking(int fd)
{
    //获取文件的标志状态
    int old_option = fcntl(fd,F_GETFL);
    //设置新的文件状态
    int new_option = old_option|O_NONBLOCK;
    fcntl(fd,F_SETFL,new_option);
    return old_option;
}

//内核事件表注册新事件，开启EPOLLSHOT,针对客户端连接的描述符，listenfd不用开启
//EPOLLSHOT代表该事件只希望被一个线程处理，处理完后需要重置
void addfd(int epollfd,int fd,bool one_shot)
{
    epoll_event event;
    event.data.fd = fd;

#ifdef ET
    //设置文件描述符可读，EI边缘触发模式，设置对端断开连接时触发EPOLLRDHUP
    event.events = EPOLLIN|EPOLLET|EPOLLRDHUP;
#endif 

#ifdef LT
    //默认LT水平触发模式
    event.events = EPOLLIN | EPOLLRDHUP;
#endif
    //开启EPOLLSHOT;
    if(one_shot)
        event.events |= EPOLLONESHOT;
    //向内核事件表注册事件
    epoll_ctl(epollfd,EPOLL_CTL_ADD,fd,&event);
    //设置为非阻塞
    setnonblocking(fd);
}

//内核事件表删除事件
void removefd(int epollfd,int fd)
{
    epoll_ctl(epollfd,EPOLL_CTL_DEL,fd,0);
    close(fd);
}

//重置EPOLLNONESHOT事件
void modfd(int epollfd,int fd,int ev)
{
    epoll_event event;
    event.data.fd = fd;
#ifdef ET
    event.events = ev|EPOLLET|EPOLLONESHOT|EPOLLRDHUP;
#endif

#ifdef LT
    event.events = ev|EPOLLONESHOT|EPOLLRDHUP;
#endif
    //修改事件表中的文件描述符的事件状态
    epoll_ctl(epollfd,EPOLL_CTL_MOD,fd,&event);
}

int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;

//关闭连接，客户总量减一
void http_conn::close_conn(bool real_close)
{
    if(real_close&&(m_sockfd!=-1))
    {
        removefd(m_epollfd,m_sockfd);
        m_sockfd=-1;
        m_user_count--;
    }
}
//初始化连接，外部调用初始化套接字地址
void http_conn::init(int sockfd,const sockaddr_in &addr)
{
    m_sockfd = sockfd;
    m_address = addr;
    addfd(m_epollfd,sockfd,true);
    m_user_count++;
    init();
}

//初始化新接受的连接
//check_state默认为分析请求行状态
void http_conn::init()
{
    mysql = NULL;
    bytes_to_send = 0;
    bytes_have_send = 0;
    m_check_state = CHECK_STATE_REQUESTLINE;
    m_linger = false;
    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    cgi = 0;
    memset(m_read_buf, '\0', READ_BUFFER_SIZE);
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
    memset(m_real_file, '\0', FILENAME_LEN);
}
//各个子线程通过process函数对任务进行处理，调用process_read函数和
//process_write函数分完成报文的解析和报文响应
void http_conn::process()
{
    HTTP_CODE read_ret = process_read();

    //NO_REQUEST 表示请求不完整，需要继续接受请求数据
    if(read_ret ==NO_REQUEST)
    {
        //注册并监听读事件
        modfd(m_epollfd,m_sockfd,EPOLLIN);
        return;
    }

    //调用process_write完成报文响应
    bool write_ret = process_write(read_ret);
    if(!write_ret)
    {
        close_conn();
    }

    //注册并监听事件
    modfd(m_epollfd,m_sockfd,EPOLLOUT);
}

//process_read通过while循环，将主从装条件进行封装，对报文的每一行进行循环处理。

http_conn::HTTP_CODE http_conn::process_read()
{
    //初始化从状态机、HTTP请求解析结果
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char* text = 0;

    //parse_line为从状态机的具体实现
    while((m_check_state==CHECK_STATE_CONTENT&&line_status==LINE_OK)||((line_status=parse_line())==LINE_OK))
    {
        //这里这样写的原因是：在GET请求报文中，每一行都是\r\n作为结束，所以对报文进行拆解时，仅用从状态机的状态line_status=parse_line())==LINE_OK语句即可。
        //但在POST请求报文中，消息体的末尾没有任何字符，所以不能使用从状态机的状态，这里转而使用主状态机的状态作为循环入口条件。
        //解析完消息体后，报文的完整解析就完成了，但此时主状态机的状态还是CHECK_STATE_CONTENT，也就是说，符合循环入口条件，还会再次进入循环，这并不是我们所希望的。
        //为此，增加了该语句，并在完成消息体解析后，将line_status变量更改为LINE_OPEN，此时可以跳出循环，完成报文解析任务。
        text = get_line();

        //m_start_line是每一个数据行在m_read_buf中的起始位置
        //m_checked_idx表示从状态机在m_read_buf中读取的位置
        m_start_line = m_checked_idx;

        //主状态机的三种状态转移逻辑
        switch(m_check_state)
        {
            case CHECK_STATE_REQUESTLINE:
            {
                //请求解析行
                ret = parse_request_line(text);
                if(ret==BAD_REQUEST)
                {
                    return BAD_REQUEST;
                }
                break;
            }
            case CHECK_STATE_HEADER:
            {
                //请求解析头部
                ret = parse_headers(text);
                if(ret == BAD_REQUEST)
                {
                    return BAD_REQUEST;
                }
                else if(ret==GET_REQUEST)
                {
                   //完整解析GET请求后，跳转到报文响应函数
                   return do_request();   
                }
                break;
            }
            case CHECK_STATE_CONTENT:
            {
                //解析消息体
                ret=parse_content(text);
                //完整解析POST请求后，跳转到报文响应函数
                if(ret==GET_REQUEST)
                {
                    return do_request();
                }
                //解析完消息体后即完成报文解析，避免再次进入循环
                line_status = LINE_OPEN;
                break;
            }
            default:
                return INTERNAL_ERROR;
        }
    }
    return NO_REQUEST;
}

//从状态机，用于分析出一行内容
//返回值为行的读取状态，有LINE_OK,LINE_BAD,LINE_OPEN

//m_read_idx指向缓冲区m_read_buf的数据末尾的下一个字节
//m_checked_idx指向从状态机正在分析的字节
http_conn::LINE_STATUS http_conn::parse_line()
{
    char temp;
    for(;m_checked_idx<m_read_idx;++m_checked_idx)
    {
        //temp为将要分析的字节
        temp = m_read_buf[m_checked_idx];

        //如果当前是\r字符，则有可能会读取完整的行
        if(temp == '\r')
        {
            //下一个字符达到了buffer结尾，则接收不完整，需要继续接收
            if((m_checked_idx+1)==m_read_idx)
            {
                return LINE_OPEN;
            }
            //如果下一字符为\n，将\r\n改为\0\0
            else if(m_read_buf[m_checked_idx+1]=='\n')
            {   
                m_read_buf[m_checked_idx++]='\0';
                m_read_buf[m_checked_idx++]='\0';
                return LINE_OK;
            }
            //如果都不符合，则返回语法错误
            return LINE_BAD;
        }

        //如果当前字符是\n,也有可能读取到完整行
        //一般是上次读取到\r就到buffer末尾了，没有接收完整，再次接收的时候会出现这种情况
        else if(temp=='\n')
        {
            //前一个字符是\r，则接收完整
            if(m_checked_idx>1&&m_read_buf[m_checked_idx-1]=='\r')
            {
                m_read_buf[m_checked_idx-1]='\0';
                m_read_buf[m_checked_idx++]='\0';
            }
            return LINE_BAD;
        }
    }

    //并没有找到\r\n，需要继续接收
    return LINE_OPEN;
}

//主状态机初始状态为CHECK_STATE_REQUESTLINE，通过调用从状态机来驱动主状态机，在主状态机
//进行解析前，从状态机已经将每一行的末尾\r\n符号改为\0\0，以便于主状态机直接取出对应
//字符串进行处理
//CHECK_STATE_REQUESTLINE
//主状态机的初始状态，调用parse_request_line函数解析请求行
//解析函数从m_read_buf中解析http请求行，获得请求方法、目标URL以及HTTP版本号
//解析完成后主状态机的状态变为CHECK_STATE_HEADER

http_conn::HTTP_CODE http_conn::parse_request_line(char *text)
{
    //在HTTP报文中，请求行用来说明请求类型，要访问的资源以及所使用的HTTP版本，其中各个部分用\t分隔
    //请求行中最先含有空格或者\t任一字符的位置并返回
    //C 库函数 char *strpbrk(const char *str1, const char *str2) 检索字符串 str1 中第一个匹配字符串 str2 中字符的字符，不包含空结束字符。
    //也就是说，依次检验字符串 str1 中的字符，当被检验字符在字符串 str2 中也包含时，则停止检验，并返回该字符位置。
    m_url = strpbrk(text," \t");
    
    //如果没有找到'\t'和' '，报文错误，直接返回
    if(!m_url)
    {
        return BAD_REQUEST;
    }

    //将该位置修改为\0，用于将前面数据取出
    *m_url++='\0';

    //取出数据，并通过与GET和POST比较，以确定请求方式
    char* method = text;
    if(strcasecmp(method,"GET")==0)
    {
        m_method = GET;
    }
    else if(strcasecmp(method,"POST")==0)
    {
        m_method = POST;
        cgi = 1;
    }
    else
    {
        return BAD_REQUEST;
    }

    //m_url此时跳过了第一个空格或\t字符，但是不知道之后是否还有
    //将m_url向后偏移，通过查找，继续跳过空格和\t字符，指向请求资源的第一个字符
    m_url+=strspn(m_url," \t");

    //使用与判断请求方式相同逻辑，判断HTTP版本号
    m_version = strpbrk(m_url," \t");
    if(!m_version)
    {
        return BAD_REQUEST;
    }
    //提取前面的数据
    *m_version++='\0';
    //继续跳过空格和\t
    m_version+=strspn(m_version," \t");

    //仅支持HTTP/1.1
    if(strcasecmp(m_version,"HTTP/1.1")!=0)
    {
        return BAD_REQUEST;
    }

    //对请求资源前七个字符进行判断
    //这里主要是有些报文的请求资源中会带有http://，这里需要对这种情况单独处理
    if(strncasecmp(m_url,"http://",7)==0)
    {
        m_url+=7;
        //找到下面出现的第一个/的位置
        m_url=strchr(m_url,'/');
    }
    
    //同样增加https的情况
        if(strncasecmp(m_url,"https://",8)==0)
    {
        m_url+=8;
        //找到下面出现的第一个/的位置
        m_url=strchr(m_url,'/');
    }

    //一般的不好带有上述两种符合，直接是单独的/或/后面带有访问资源
    if(!m_url||m_url[0]!='/')
        return BAD_REQUEST;
    
    //当url为/时，显示欢迎界面
    if(strlen(m_url)==1)
    {
        strcat(m_url,"judge.html");
    }

    //请求行处理完毕，将主状态机转移处理请求头
    m_check_state = CHECK_STATE_HEADER;
    return NO_REQUEST;
}

//解析完请求行后，主状态机继续分析请求头。在报文中，请求行和空行的处理使用的是同一个函数。
//这里通过判断当前的text首位是不是\0字符，若是，则表示当前处理的是空行，若不是，则表示当前处理的是请求头。
//CHECK_STATE_HEADER
//1.调用parse_headers函数解析请求头部信息
//2.判断是空行还是请求头，如果是空行，进而判断content-length是否为0。
//如果不是0，表明是POST请求，则转移到CHECK_STATE_CONTENT,否则说明是GET请求，则报文解析结束。
//3.若解析的是请求头部字段，则主要分析connection字段，content-length字段，其它字段可以直接跳过，或者按需求继续分析
//4.content-length字段，这里用于读取post请求的消息体长度
//解析HTTP请求的一个头部信息
http_conn::HTTP_CODE http_conn::parse_headers(char *text)
{
    //判断是空头还是请求头
    if(text[0]=='\0')
    {
        //判断是GET还是POST请求
        if(m_content_length!=0)
        {
            //POST需要跳转到消息体处理状态
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        return GET_REQUEST;
    }
    //解析请求头部连接字段
    else if(strncasecmp(text,"Connection:",11)==0)
    {
        text+=11;

        //跳过空格和\t字符
        text+=strspn(text," \t");
        if(strcasecmp(text,"keep-alive")==0)
        {
            //如果是长连接，则将linger标志设置为true
            m_linger = true;
        }
    }
    //解析请求头部内容长度字段
    else if(strncasecmp(text,"Content-length:",15)==0)
    {
        text+=15;
        text+=strspn(text," \t");
        m_content_length=atol(text);
    }
    //解析请求头部HOST字段
    else if(strncasecmp(text,"Host:",5)==0)
    {
        text+=5;
        text+=strspn(text," \t");
        m_host=text;
    }
    else
    {
        LOG_INFO("oop!unknow header: %s", text);
        log::get_instance()->flush();
    }
    return NO_REQUEST;
}

//GET和POST请求报文的区别之一是有无消息体部分，GET请求没有消息体，当解析完空行之后，便完成了报文的解析。
//但后续的登录和注册功能，为了避免将用户名和密码直接暴露在URL中，我们在项目中改用了POST请求，将用户名和密码添加在报文中作为消息体进行了封装。

//CHECK_STATE_CONTENT
//仅用于解析POST请求，调用parse_content函数解析消息体
//用于保存post请求消息体，为后面的登录和注册做准备

//判断HTTP请求是否被完整读入
http_conn::HTTP_CODE http_conn::parse_content(char *text)
{
    //判断buffer中是否读取了消息体
    if(m_read_idx>=(m_content_length+m_checked_idx))
    {
        text[m_content_length]='\0';

        //POST请求中最后为输入的用户名和密码
        m_string = text;
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

//网站根目录，文件夹内存放的是请求的资源和跳转的HTML文件
const char* doc_root = "/home/cs2103b/Linux/TinyHttpServer/root";

http_conn::HTTP_CODE http_conn::do_request()
{
    //将初始化的m_real_file赋值为网站的根目录
    strcpy(m_real_file,doc_root);
    int len = strlen(doc_root);

    //找到m_url中的/的位置
    const char *p = strrchr(m_url,'/');
    //实现登录和注册校验
    if(cgi==1&&(*(p+1)=='2')||*(p+1)=='3')
    {
        //根据标志判断是登录检测还是注册检测
        char flag = m_url[1];

        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/");
        strcat(m_url_real, m_url + 2);
        strncpy(m_real_file + len, m_url_real, FILENAME_LEN - len - 1);
        free(m_url_real);

        //将用户名和密码提取出来
        //user=123&password=123
        char name[100], password[100];
        int i;
        for (i = 5; m_string[i] != '&'; ++i)
            name[i - 5] = m_string[i];
        name[i - 5] = '\0';

        int j = 0;
        for (i = i + 10; m_string[i] != '\0'; ++i, ++j)
            password[j] = m_string[i];
        password[j] = '\0';
        //同步线程登录校验
        if (*(p + 1) == '3')
        {
            //如果是注册，先检测数据库中是否有重名的
            //没有重名的，进行增加数据
            char *sql_insert = (char *)malloc(sizeof(char) * 200);
            strcpy(sql_insert, "INSERT INTO user(username, passwd) VALUES(");
            strcat(sql_insert, "'");
            strcat(sql_insert, name);
            strcat(sql_insert, "', '");
            strcat(sql_insert, password);
            strcat(sql_insert, "')");

            if (users.find(name) == users.end())
            {

                m_lock.lock();
                int res = mysql_query(mysql, sql_insert);
                users.insert(pair<string, string>(name, password));
                m_lock.unlock();

                if (!res)
                    strcpy(m_url, "/log.html");
                else
                    strcpy(m_url, "/registerError.html");
            }
            else
                strcpy(m_url, "/registerError.html");
        }
        //如果是登录，直接判断
        //若浏览器端输入的用户名和密码在表中可以查找到，返回1，否则返回0
        else if (*(p + 1) == '2')
        {
            if (users.find(name) != users.end() && users[name] == password)
                strcpy(m_url, "/welcome.html");
            else
                strcpy(m_url, "/logError.html");
        }
    }

    //如果请求资源为/0，表示跳转注册界面
    if(*(p+1)=='0')
    {
        char *m_url_real = (char* )malloc(sizeof(char)*200);
        strcpy(m_url_real,"/register.html");

        //将网站目录和/register.html进行拼接，更新到m_real_file中
        strncpy(m_real_file+len,m_url_real,strlen(m_url_real));

        free(m_url_real);
    }
    //如果请求资源为/1，表示跳转登录界面
    else if(*(p+1)=='1')
    {
        char *m_url_real = (char*)malloc(sizeof(char)*200);
        strcpy(m_url_real,"/log.html");

        //将网站目录和/log.html进行拼接，更新到m_real_file中
        strncpy(m_real_file+len,m_url_real,strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '5')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/picture.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '6')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/video.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '7')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/fans.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else
    {
        //如果意思均不符合，即不是登录和注册，直接将url与网站目录拼接
        //这里的情况是welcome界面，请求服务器上的一个图片
        strncpy(m_real_file+len,m_url,FILENAME_LEN-len-1);
    }

    //通过stat获取请求资源文件信息，成功则将信息更新到m_file_stat结构体
    //失败返回NO_RESOURCE状态，表示资源不存在
    if(stat(m_real_file,&m_file_stat)<0)
        return NO_RESOURCE;
    
    //判断文件的权限，是否可读，不可读则返回FORBIDDEN_REQUEST状态
    if(!(m_file_stat.st_mode&S_IROTH))
        return FORBIDDEN_REQUEST;
    //判断文件类型，如果是目录，则返回BAD_REQUEST,表示请求报文有误
    if(S_ISDIR(m_file_stat.st_mode))
        return BAD_REQUEST;

    //以只读方式获取文件描述符，通过mmap将该文件映射到内存中
    int fd = open(m_real_file,O_RDONLY);
    m_file_address = (char*)mmap(0,m_file_stat.st_size,PROT_READ,MAP_PRIVATE,fd,0);

    close(fd);

    return FILE_REQUEST;
    
}

//process_write
//根据do_request的返回状态，服务器子线程调用process_write向m_write_buf中写入响应报文
//1.add_status_line函数，添加状态行：http/1.1状态消息
//2.add_headers函数添加消息报头，内部调用add_content_length和add_linger函数
//  1.content-length记录响应报文长度，用于浏览器端判断服务器是否发送完数据
//  2.connection记录连接状态，用于告诉浏览器端保持长连接
//3.add_blank_line添加空行
//上述涉及的5个函数，均是内部调用add_response函数更新m_write_idx指针和缓冲区m_write_buf中的内容
bool http_conn::add_response(const char* format,...)
{
    //如果写入内容超过m_write_buf则报错
    if(m_write_idx>=WRITE_BUFFER_SIZE)
    {
        return false;
    }

    //定义可变参数列表
    va_list arg_list;

    //将变量arg_list初始化传入参数
    //初始化并且绑定的意思？
    va_start(arg_list,format);

    //将数据format从可变参数列表写入缓冲区，返回写入数据的长度
    //int _vsnprintf(char* str, size_t size, const char* format, va_list ap);
    //str为字符串的存放位置，size为str可接受的最大字符数，format指定输出格式的字符串，决定了需要提供的可变参数的类型、个数和顺序
    //ap为可变参数
    int len = vsnprintf(m_write_buf+m_write_idx,WRITE_BUFFER_SIZE-1-m_write_idx,format,arg_list);

    //如果写入的长度超过缓冲区剩余空间，则报错
    if(len>=(WRITE_BUFFER_SIZE-1-m_write_idx))
    {
        va_end(arg_list);
        return false;
    }

    //更新m_write_idx位置
    m_write_idx+=len;
    //清空可变参数列表
    va_end(arg_list);
    return true;
}

//添加状态行
bool http_conn::add_status_line(int status,const char* title)
{
    return add_response("%s %d %r\r\n","HTTP/1.1",status,title);
}

//添加消息报头，具体的添加文本长度、连接状态和空行
bool http_conn::add_headers(int content_len)
{
    add_content_length(content_len);
    add_linger();
    add_blank_line();
}

//添加content-length，表示响应报文的长度
bool http_conn::add_content_length(int content_len)
{
    return add_response("Content-Length:%d\r\n",content_len);
}

//添加文本类型，这里是html
bool http_conn::add_content_type()
{
    return add_response("Content-Type:%s\r\n","text/html");
}

//添加连接状态，通知浏览器端是保活还是关闭
bool http_conn::add_linger()
{
    return add_response("Connection:%s\r\n",(m_linger==true)?"keep-alive":"close");
}

//添加空行
bool http_conn::add_blank_line()
{
    return add_response("%s","\r\n");
}

//添加文本content
bool http_conn::add_content(const char* content)
{
    return add_response("%s",content);
}

//响应报文分为两种，一种是请求文件的存在，通过io向量机制iovec,声明两个iovec,第一个指向
//m_write_buf,第二个指向mmap的地址m_file_address；一种是 请求出错，这时候只申请一个
//iovec,指向m_write_buf.
//1.iovec是一个结构体，里面有两个元素，指针成员iov_base指向一个缓冲区，这个缓冲区存放的是
//writev将要发送的数据
//2.成员iov_len表示实际写入的长度
bool http_conn::process_write(HTTP_CODE ret)
{
    switch(ret)
    {
        //内部错误,500
        case INTERNAL_ERROR:
        {
            //状态行
            add_status_line(500,error_500_title);
            //消息报头
            add_headers(strlen(error_500_form));
            if(!add_content(error_500_form))
                return false;
            break;
        }
        //报文语法错误，404
        case BAD_REQUEST:
        {
            add_status_line(404,error_404_title);
            add_headers(strlen(error_404_form));
            if(!add_content(error_404_form))
                return false;
            break;
        }
        //资源没有访问权限，403
        case FORBIDDEN_REQUEST:
        {
            add_status_line(403,error_403_title);
            add_headers(strlen(error_403_form));
            if(!add_content(error_403_form))
                return false;
            break;
        }
        //文件存在，200
        case FILE_REQUEST:
        {
            add_status_line(200,ok_200_title);
            //如果请求的资源存在
            if(m_file_stat.st_size!=0)
            {
                add_headers(m_file_stat.st_size);
                //第一个iovec指针指向响应报文缓冲区，长度指向m_write_idx
                m_iv[0].iov_base=m_write_buf;
                m_iv[0].iov_len=m_write_idx;
                //第二个iovec指针指向mmap返回的文件指针，长度指向文件的大小
                m_iv[1].iov_base=m_file_address;
                m_iv[1].iov_len=m_file_stat.st_size;
                m_iv_count=2;
                //发送的全部数据为响应报文头部信息和文件大小
                bytes_to_send=m_write_idx+m_file_stat.st_size;
                return true;
            }
            else
            {
                //如果请求的资源大小为0，则返回空白的html文件
                const char* ok_string = "<html><body></body></html>";
                add_headers(strlen(ok_string));
                if(!add_content(ok_string))
                {
                    return false;
                }
            }
        }
        default:
            return false;
    }
    //除FILE_REQUEST状态外，其余状态只申请一个iovec,指向响应报文缓冲区
    m_iv[0].iov_base=m_write_buf;
    m_iv[0].iov_len=m_write_idx;
    m_iv_count=1;
    return true;
}

//http_conn::write
//服务器子线程调用process_write完成响应报文，随后注册epollout事件。服务器主线程检测写事件
//并调用http_conn::write函数将响应报文发送给浏览器端
//该函数具体逻辑如下：
//在生成响应报文时初始化byte_to_send，包括头部信息和文件数据大小。通过writev函数循环发送响应
//报文数据，根据返回值更新byte_have_send和iovec结构体的指针和长度，并判断响应报文整体是否发送
//成功
//1.若writev单次发送成功，更新byte_to_send和byte_have_send的大小，若响应报文整体发送成功
//则取消mmap映射，并判断是否为长连接
//  1.长连接重置http类实例，注册事件，不关闭连接
//  2.短链接直接关闭连接
//2.若单次writev发送不成功，判断是否是缓冲区写满了
//  1.若不是因为缓冲区满了而失败，取消mmap映射，关闭连接
//  2.若eagain则满了，更新iovec结构体的指针和长度，并注册写事件，等待下一次写事件触发
//  （当写缓冲区从不可写变为可写，触发epollout）,因此在此期间无法立即接收到同一用户的下一请求。
//     但可以保证连接的完整性。
bool http_conn::write()
{
    int temp=0;
    int newadd=0;

    //若要发送的数据长度为0
    //表示响应报文为空，一般不会出现
    if(bytes_to_send==0)
    {
        modfd(m_epollfd,m_sockfd,EPOLLIN);
        init();
        return true;
    }

    while(1)
    {
        //将响应报文的状态行、消息头、空行和响应正文发送给浏览器端
        temp=writev(m_sockfd,m_iv,m_iv_count);

        //正常发送,temp为发送的字节数
        if(temp>0)
        {
            //更新已发送的字节
            bytes_to_send+=temp;
            //偏移文件iovec的指针
            newadd = bytes_to_send-m_write_idx;
        }
        if(temp<0)
        {
            //判断缓冲区是否满了
            if(errno==EAGAIN)
            {
                //第一个iovec头部消息的数据已发送完，发送第二个iovec数据
                if(bytes_have_send>=m_iv[0].iov_len)
                {
                    //不再发送头部数据
                    m_iv[0].iov_len=0;
                    m_iv[1].iov_base=m_file_address+newadd;
                    m_iv[1].iov_len=bytes_to_send;
                }
                //继续发送第一个iovec头部信息的数据
                else
                {
                    m_iv[0].iov_base=m_write_buf+bytes_to_send;
                    m_iv[0].iov_len=m_iv[0].iov_len-bytes_have_send;
                }
                //重新注册写事件
                modfd(m_epollfd,m_sockfd,EPOLLIN);
                return true;
            }
            //如果发送失败，但不是缓冲区问题，取消映射
            unmap();
            return false;
        }
        //更新已发送字节数
        bytes_to_send-=temp;

        //判断条件，数据已全部发送完
        if(bytes_to_send<=0)
        {
            unmap();
            
            //在epoll树上重置EPOLLONESHOT事件
            modfd(m_epollfd,m_sockfd,EPOLLIN);

            //浏览器请求为长连接
            if(m_linger)
            {
                //重新初始化http对象
                init();
                return true;
            }
            else
            {
                return false;
            }            
        }
    }
}

void http_conn::unmap()
{
    if(m_file_address)
    {
        munmap(m_file_address,m_file_stat.st_size);
        m_file_address=0;
    }
}















