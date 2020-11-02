#include<stdio.h>
#include<unistd.h>
#include<stdlib.h>
#include<errno.h>
#include<string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include "../timed_messaging_system.h"

#define MAX_MESSAGE_SIZE 512

int main(int argc, char *argv[]){
	
	int fd_read, ret;
	char message[MAX_MESSAGE_SIZE];
	unsigned long timeout;
	
	if(argc != 3){
		fprintf(stderr, "Wrong Usage! Usage: sudo %s, <filename>, <read_timeout>\n", argv[0]);
		exit (EXIT_FAILURE);
	}
	
	errno = 0;
	fd_read = open(argv[1], O_RDWR);
	if(fd_read == -1){
		fprintf(stderr, "Error in opening file errno = %d \n", errno);
		exit(EXIT_FAILURE);
	}
	
	
	errno = 0;
	timeout = strtoul(argv[2], NULL, 0);
	ret = ioctl(fd_read, SET_RECV_TIMEOUT, timeout);
	if (ret == -1 || errno != 0){
		fprintf(stderr, "Error in ioctl() \n");
		exit(EXIT_FAILURE);
	}
	
	while(1){
		ret = read(fd_read, message, MAX_MESSAGE_SIZE);
		if (ret == -1){
			fprintf(stderr, "All messages have been read \n");
		}else{
			printf("Message: %s \n", message);
		}
	}
}
