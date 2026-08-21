#通用Makefile
out:=server#可执行文件名称需要根据需要更改
SRCS:=$(wildcard *.c)#拿到当前目录下所有的.c文件
OBJS:=$(patsubst %.c,%.o,$(SRCS))#把.c文件替换为.o文件
#预定义变量
CC:=gcc
MYSQL_CFLAGS:=$(shell mysql_config --cflags)
MYSQL_LIBS:=$(shell mysql_config --libs)
CPPFLAGS:=-I../include $(MYSQL_CFLAGS)
CFLAGS:=-g
LDFLAGS:=-L../l8w8jwt/bin/release
$(out):$(OBJS)
	$(CC) $^ -o $@ $(LDFLAGS) -pthread -lcrypt $(MYSQL_LIBS) -ll8w8jwt -lmbedcrypto -lmbedx509 -g
%.o:%.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $^ -o $@

#.PHONY:clean rebuild
rebuild:clean $(out)
clean:
	rm -rf $(OBJS) $(out)
