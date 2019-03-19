#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <vector>

#include "log.h"
#include "conn.h"
#include "mgr.h"
#include "processpool.h"

using std::vector;

static const char* version = "1.0";

//惯例调试输出函数
static void usage( const char* prog )
{
    log( LOG_INFO, __FILE__, __LINE__,  "usage: %s [-h] [-v] [-f config_file]", prog );
	ption = getopt( argc, argv, "f:xvh" )
}

int main( int argc, char* argv[] )
{
    char cfg_file[1024];
    memset( cfg_file, '\0', 100 );

	//解析命令行选项
    int option;
    while ( ( option = getopt( argc, argv, "f:xvh" ) ) != -1 )
    {
        switch ( option )
        {
            case 'x':	//如果有选项x，设置日志等级为LOG_DEBUG（最高）
            {
                set_loglevel( LOG_DEBUG );
                break;
            }
            case 'v':	//如果有选项v，日志输出本程序名及版本
            {
                log( LOG_INFO, __FILE__, __LINE__, "%s %s", argv[0], version );
                return 0;
            }
            case 'h':	//如果有选项h，日志输出usage
            {
                usage( basename( argv[ 0 ] ) );
                return 0;
            }
            case 'f':	//如果有选项f，将f的参数复制到buff中
            {
                memcpy( cfg_file, optarg, strlen( optarg ) );
                break;
            }
            case '?':	//其他选项，输出错误提示
            {
                log( LOG_ERR, __FILE__, __LINE__, "un-recognized option %c", option );
                usage( basename( argv[ 0 ] ) );
                return 1;
            }
        }
	
	//检测cfg_file中是否有值,有就打开该buff中所存储的文件的名字所代表的文件
    if( cfg_file[0] == '\0' )
    {
        log( LOG_ERR, __FILE__, __LINE__, "%s", "please specifiy the config file" );
        return 1;
    }
    int cfg_fd = open( cfg_file, O_RDONLY );
    if( !cfg_fd )
    {
        log( LOG_ERR, __FILE__, __LINE__, "read config file met error: %s", strerror( errno ) );
        return 1;
    }
	//解析配置文件中的内容
		//读取配置文件中的内容，暂存到临时的buff中
    struct stat ret_stat;
    if( fstat( cfg_fd, &ret_stat ) < 0 )
    {
        log( LOG_ERR, __FILE__, __LINE__, "read config file met error: %s", strerror( errno ) );
        return 1;
    }
    char* buf = new char [ret_stat.st_size + 1];
    memset( buf, '\0', ret_stat.st_size + 1 );
    ssize_t read_sz = read( cfg_fd, buf, ret_stat.st_size );
    if ( read_sz < 0 )
    {
        log( LOG_ERR, __FILE__, __LINE__, "read config file met error: %s", strerror( errno ) );
        return 1;
    }
		//状态机-解析配置文件中指定的服务器主机，包含ip、端口、链接数信息
    vector< host > balance_srv;
    vector< host > logical_srv;
    host tmp_host;
    memset( tmp_host.m_hostname, '\0', 1024 );
    char* tmp_hostname;
    char* tmp_port;
    char* tmp_conncnt;
    bool opentag = false;
    char* tmp = buf;
    char* tmp2 = NULL;
    char* tmp3 = NULL;
    char* tmp4 = NULL;
    while( tmp2 = strpbrk( tmp, "\n" ) )
    {
        *tmp2++ = '\0';
        if( strstr( tmp, "<logical_host>" ) )
        {
            if( opentag )
            {
                log( LOG_ERR, __FILE__, __LINE__, "%s", "parse config file failed" );
                return 1;
            }
            opentag = true;
        }
        else if( strstr( tmp, "</logical_host>" ) )
        {
            if( !opentag )
            {
                log( LOG_ERR, __FILE__, __LINE__, "%s", "parse config file failed" );
                return 1;
            }
            logical_srv.push_back( tmp_host );
            memset( tmp_host.m_hostname, '\0', 1024 );
            opentag = false;
        }
        else if( tmp3 = strstr( tmp, "<name>" ) )
        {
            tmp_hostname = tmp3 + 6;
            tmp4 = strstr( tmp_hostname, "</name>" );
            if( !tmp4 )
            {
                log( LOG_ERR, __FILE__, __LINE__, "%s", "parse config file failed" );
                return 1;
            }
            *tmp4 = '\0';
            memcpy( tmp_host.m_hostname, tmp_hostname, strlen( tmp_hostname ) );
        }
        else if( tmp3 = strstr( tmp, "<port>" ) )
        {
            tmp_port = tmp3 + 6;
            tmp4 = strstr( tmp_port, "</port>" );
            if( !tmp4 )
            {
                log( LOG_ERR, __FILE__, __LINE__, "%s", "parse config file failed" );
                return 1;
            }
            *tmp4 = '\0';
            tmp_host.m_port = atoi( tmp_port );
        }
        else if( tmp3 = strstr( tmp, "<conns>" ) )
        {
            tmp_conncnt = tmp3 + 7;
            tmp4 = strstr( tmp_conncnt, "</conns>" );
            if( !tmp4 )
            {
                log( LOG_ERR, __FILE__, __LINE__, "%s", "parse config file failed" );
                return 1;
            }
            *tmp4 = '\0';
            tmp_host.m_conncnt = atoi( tmp_conncnt );
        }
        else if( tmp3 = strstr( tmp, "Listen" ) )
        {
            tmp_hostname = tmp3 + 6;
            tmp4 = strstr( tmp_hostname, ":" );
            if( !tmp4 )
            {
                log( LOG_ERR, __FILE__, __LINE__, "%s", "parse config file failed" );
                return 1;
            }
            *tmp4++ = '\0';
            tmp_host.m_port = atoi( tmp4 );
            memcpy( tmp_host.m_hostname, tmp3, strlen( tmp3 ) );
            balance_srv.push_back( tmp_host );
            memset( tmp_host.m_hostname, '\0', 1024 );
        }
        tmp = tmp2;
    }

	//如果没有从配置文件中解析到服务主机信息，输出相应日志信息
    if( balance_srv.size() == 0 || logical_srv.size() == 0 )
    {
        log( LOG_ERR, __FILE__, __LINE__, "%s", "parse config file failed" );
        return 1;
    }
	
	//使用均衡服务器主机的ip、port创建套接字并监听之
    const char* ip = balance_srv[0].m_hostname;
    int port = balance_srv[0].m_port;

    int listenfd = socket( PF_INET, SOCK_STREAM, 0 );
    assert( listenfd >= 0 );
 
    int ret = 0;
    struct sockaddr_in address;
    bzero( &address, sizeof( address ) );
    address.sin_family = AF_INET;
    inet_pton( AF_INET, ip, &address.sin_addr );
    address.sin_port = htons( port );

    ret = bind( listenfd, ( struct sockaddr* )&address, sizeof( address ) );
    assert( ret != -1 );

    ret = listen( listenfd, 5 );
    assert( ret != -1 );

    //memset( cfg_host.m_hostname, '\0', 1024 );
    //memcpy( cfg_host.m_hostname, "127.0.0.1", strlen( "127.0.0.1" ) );
    //cfg_host.m_port = 54321;
    //cfg_host.m_conncnt = 5;

	//进程池
    processpool< conn, host, mgr >* pool = processpool< conn, host, mgr >::create( listenfd, logical_srv.size() );
    if( pool )
    {
        pool->run( logical_srv );
        delete pool;
    }

    close( listenfd );
    return 0;
}
