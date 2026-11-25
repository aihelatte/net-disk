#通用Makefile
out:=server#可执行文件名称需要根据需要更改
SRCS:=$(wildcard *.cpp)
OBJS:=$(patsubst %.cpp,%.cpp.o,$(SRCS))
#预定义变量
CXX:=g++
MYSQL_CFLAGS:=$(shell mysql_config --cflags)
MYSQL_LIBS:=$(shell mysql_config --libs)
CPPFLAGS:=-I../include $(MYSQL_CFLAGS)
CXXFLAGS:=-std=c++17 -MMD -MP -g -Wall -Wextra -Wpedantic -Werror=return-type
LDFLAGS:=-L../l8w8jwt/bin/release
$(out):$(OBJS)
	$(CXX) $^ -o $@ $(LDFLAGS) -pthread -lcrypt $(MYSQL_LIBS) -ll8w8jwt -lmbedcrypto -lmbedx509 -g
%.cpp.o:%.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

.PHONY:clean rebuild
rebuild: clean
	$(MAKE) $(out)
clean:
	rm -f $(OBJS) $(OBJS:.o=.d) $(out)

-include $(OBJS:.o=.d)
