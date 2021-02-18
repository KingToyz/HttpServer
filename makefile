server: main.cpp ./threadpool.h ./http_conn.cpp ./http_conn.h ./locker.h ./log.cpp ./log.h ./block_queue.h ./sql_connection_pool.cpp ./sql_connection_pool.h
	g++ -g -o server main.cpp ./threadpool.h ./http_conn.cpp ./http_conn.h ./locker.h ./log.cpp ./log.h ./sql_connection_pool.cpp ./sql_connection_pool.h -lpthread -lmysqlclient


clean:
	rm  -r server