#通用Makefile
out:=client#可执行文件名称需要根据需要更改
SRCS:=$(wildcard *.c)#拿到当前目录下所有的.c文件
OBJS:=$(patsubst %.c,%.o,$(SRCS))#把.c文件替换为.o文件
#预定义变量
CC:=gcc
CPPFLAGS:=-I../include
CFLAGS:=-g
LDFLAGS:=-L../l8w8jwt/bin/release
$(out):$(OBJS)
	$(CC) $^ -o $@ $(LDFLAGS) -pthread -lcrypt -ll8w8jwt -lmbedcrypto -lmbedx509 -g

#.PHONY:clean rebuild
rebuild:clean $(out)
clean:
	rm -rf $(OBJS) $(out)
