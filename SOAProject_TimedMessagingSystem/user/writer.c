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

#define MAX_MSG_SIZE 512

int main(int argc, char *argv[]){

	int fd_write, ret;
	char msg[MAX_MSG_SIZE];
	unsigned long timeout;

	if(argc != 3){
		fprintf(stderr, "Wrong Usage! Usage: sudo %s, <filename>, <timeout>\n", argv[0]);
		exit (EXIT_FAILURE);
	}
	
	fd_write = open(argv[1], O_RDWR);
	if(fd_write == -1){
		fprintf(stderr, "Error in opening file\n");
		exit(EXIT_FAILURE);
	}
	
	
	errno = 0;
	timeout = strtoul(argv[2], NULL, 0);
	ret = ioctl(fd_write, SET_SEND_TIMEOUT, timeout);
	if (ret == -1 || errno != 0){
		fprintf(stderr, "Error in ioctl() \n");
		exit(EXIT_FAILURE);
	}
	
	fputs("Write REVOKE_DELAYED_MESSAGES to revoke message with expired timeout \n", stdout);
	fputs("Write CLOSE to close file descriptor \n", stdout);
	while (1) {
		fflush(stdout);
		if (!fgets(msg, MAX_MSG_SIZE, stdin)) {
			fprintf(stderr, "Error in fgets() \n");
			return(EXIT_FAILURE);
		}
		msg[strlen(msg)-1] = '\0';
		if (strcmp(msg, "REVOKE_DELAYED_MESSAGES") == 0) {
			errno = 0;
			ret = ioctl(fd_write, REVOKE_DELAYED_MESSAGES);
			if (ret == -1) {
				fprintf(stderr, "Error in revoke delayed messages: %d\n", errno);
			} else {
				printf("Delayed messages revoked\n");
			}
			continue;
		}
		if (strcmp(msg, "CLOSE") == 0) {
			close(fd_write);
			printf("File descriptor closed\n");
			return(EXIT_SUCCESS);
		}
		// Write
		errno = 0;
		ret = write(fd_write, msg, strlen(msg) + 1);
		if (ret == -1) {
			fprintf(stderr, "Error in write(): %d\n", errno);
		} else {
			printf("Message writed \n");
		}
	}
}
	

