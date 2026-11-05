#include "threadPool.h"
#include "worker.h"
#include "md5.h"

// 调用方持有文件及锁，从同一个文件描述符计算完整 MD5
int Compute_fd_md5(int fd, char *md5_str)
{
	int i;
	int ret;
    unsigned char data[READ_DATA_SIZE];
	unsigned char md5_value[MD5_SIZE];
	MD5_CTX md5;

	if (lseek(fd, 0, SEEK_SET) == -1)
	{
		perror("lseek");
		return -1;
	}
 
	// init md5
	MD5Init(&md5);
 
	while (1)
	{
		ret = read(fd, data, READ_DATA_SIZE);
		if (-1 == ret)
		{
			if(errno == EINTR)
			{
				continue;
			}
			perror("read");
			return -1;
		}
		if (0 == ret)
		{
			break;
		}
		MD5Update(&md5, data, ret);
	}
	if(lseek(fd, 0, SEEK_SET) == -1)
	{
		perror("lseek");
		return -1;
	}
 
	MD5Final(&md5, md5_value);
 
	for(i = 0; i < MD5_SIZE; i++)
	{
		snprintf(md5_str + i*2, 2+1, "%02x", md5_value[i]);
	}
	md5_str[MD5_STR_LEN] = '\0'; // add end
 
	return 0;
}

int Compute_file_md5(char *file_path, char *md5_str)
{
	int fd = open_in_root("upload", file_path, O_RDONLY, 0);
	if(fd == -1)
	{
		perror("open");
		return -1;
	}
	int ret = Compute_fd_md5(fd, md5_str);
	close(fd);
	return ret;
}
