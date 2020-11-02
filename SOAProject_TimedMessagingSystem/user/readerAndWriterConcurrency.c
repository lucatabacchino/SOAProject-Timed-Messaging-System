#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/sysmacros.h>
#include <pthread.h>
#include <string.h>
#include <errno.h>
#include <sys/ioctl.h>
#include "../timed_messaging_system.h"

#define MINOR 0
#define WRITERS 20
#define READERS 5
#define MAX_MSG_SIZE 256

int fd;

void *writer(void *arg)
{
	pthread_t id;
	char msg[MAX_MSG_SIZE];
	int ret;

	id = pthread_self();
	sprintf(msg, "Hello I am thread with id: %lu\n", id);
	
	ret = write(fd, msg, strlen(msg)+1);
	if (ret == -1) {
		fprintf(stderr, "Error in write\n");
		exit(EXIT_FAILURE);
	}
	
	return NULL;
	
}

void *reader(void *arg)
{
	char msg[MAX_MSG_SIZE];
	int ret;
	pthread_t id = pthread_self();
	
	// Read
	while (1) {
		ret = read(fd, msg, MAX_MSG_SIZE);
		if (ret == -1 && errno != ENOMSG) {
			fprintf(stderr, "Error in read\n");
			exit(EXIT_FAILURE);
		}
		if (ret > 0) {
			printf("Hello I am thread with id %lu!Message read: %s\n",id, msg);
		}	
	}
	
	return NULL;
}

int main(int argc, char *argv[])
{
	int ret, i;
	unsigned int major;
	unsigned long write_timeout, read_timeout, temp;
	pthread_t reader_id[READERS];
	pthread_t writer_id[WRITERS];

	if (argc != 5) {
		fprintf(stderr, "Usage: %s <pathname> <major> <write_timeout> <read_timeout>\n", argv[0]);
		return(EXIT_FAILURE);
	}
	
	major = strtoul(argv[2], NULL, 0);
	write_timeout = strtoul(argv[3], NULL, 0);
	read_timeout = strtoul(argv[4], NULL, 0);
	
	if(write_timeout > read_timeout){
		fprintf(stderr, "read_timeout must be greater than write_timeout!\nValues is switched \n");
		temp = read_timeout;
		read_timeout = write_timeout;
		write_timeout = temp;
		fprintf(stderr, "read_timeout: %lu \nwrite_timeout: %lu \n", read_timeout, write_timeout);
	}
	
	// Create a char device file
	ret = mknod(argv[1], S_IFCHR, makedev(major, MINOR));
	if (ret == -1) {
		fprintf(stderr, "Error in mknod()\n");
		return(EXIT_FAILURE);
	}
	
	// Open file
	fd = open(argv[1], O_RDWR);
	if (fd == -1) {
		fprintf(stderr, "Error in open()\n");
		return(EXIT_FAILURE);
	}
	
	if (write_timeout != 0){
		ret = ioctl(fd, SET_SEND_TIMEOUT, write_timeout);
		printf("write_timeout=%lu\n", write_timeout);
		if (ret == -1) {
			fprintf(stderr, "Error in ioctl()\n");
			return(EXIT_FAILURE);
		}
	}
	
	if (read_timeout != 0){
		ret = ioctl(fd, SET_RECV_TIMEOUT, read_timeout);
		printf("read_timeout=%lu\n", read_timeout);
		if (ret == -1) {
			fprintf(stderr, "Error in ioctl()\n");
			return(EXIT_FAILURE);
		}
	}
	
	for (i = 0; i < WRITERS; i++) {
		if (pthread_create(&writer_id[i], NULL, writer, NULL)) {
			fprintf(stderr, "Error in pthread_create\n");
			return(EXIT_FAILURE);
		}
	}
	
	for (i = 0; i < READERS; i++) {
		if (pthread_create(&reader_id[i], NULL, reader, NULL)) {
			fprintf(stderr, "Error in pthread_create\n");
			return(EXIT_FAILURE);
		}
	}
	
	while(1);
	
	return(EXIT_SUCCESS); 
}
