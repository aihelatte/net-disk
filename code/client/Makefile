#通用Makefile
out:=client#可执行文件名称需要根据需要更改
SRCS:=$(wildcard *.cpp)
OBJS:=$(patsubst %.cpp,%.cpp.o,$(SRCS))
#预定义变量
CXX:=g++
CPPFLAGS:=-I../include
CXXFLAGS:=-std=c++17 -pthread -MMD -MP -g -Wall -Wextra -Wpedantic -Werror=return-type
LDFLAGS:=-L../l8w8jwt/bin/release
$(out):$(OBJS)
	$(CXX) $^ -o $@ $(LDFLAGS) -pthread -lcrypt -ll8w8jwt -lmbedcrypto -lmbedx509 -g

%.cpp.o:%.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

.PHONY:clean rebuild
rebuild: clean
	$(MAKE) $(out)
clean:
	rm -f $(OBJS) $(OBJS:.o=.d) $(out)

-include $(OBJS:.o=.d)
