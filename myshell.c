#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>


// Sets the new handler as the SIGNAME handler, for the caller proccess. 
// Returns: 0 on success. -1 otherwise;
int set_handler(int SIGNAME, void (*new_handler)()){
	struct sigaction new_action;
	memset(&new_action, 0, sizeof(new_action));
  	new_action.sa_handler = new_handler;
  	new_action.sa_flags = SA_SIGINFO | SA_RESTART;
  	if(sigaction(SIGNAME, &new_action, NULL) != 0)
  	{
    	fprintf(stderr, "Signal handler registration failed: %s\n", strerror(errno));
    	return -1;
  	}
	
	return 0;
}

// will be the SIGCHLD handler for the shell process;
void SIGCHLD_handler(int signnum){
	int saved_errno = errno; // waitpids will change it, but we want it to be as it was before the handler
	// for concurrency when parent executes;
	int status;
	// Will check if any zombie process exists. 
	// if yes, one of them is deleted; if not, 0 is returned (and process holds only active processes);
	// WNOHANG flag makes wait to return immediatly if there are no zombie children.
	// That prevents shell from stopping running / stopped children.
	while (waitpid(-1, &status, WNOHANG) > 0){	

	}
	errno = saved_errno;
}

int prepare(void){

	#ifdef WriteErrorsToFile

	if (error_file_init == 0){
		int fd = open("errors.txt", O_RDWR | O_CREAT, 0777);
		dup2(fd, fileno(stderr));
	}

	#endif

	// Makes shell remove zombie processes every time SIGCHLD arrives (which means that some child has
	// terminated / stopped / continued);
	if (set_handler(SIGCHLD, &SIGCHLD_handler) != 0){
		return -1;
	}

	// Making shell ignore SIGINT signal (childs will inherit this (because the new handler is SIG_IGN));
	if (set_handler(SIGINT, SIG_IGN) != 0){
		return -1;
	}

	return 0;
}

int finalize(void){
	return 0;
}

//	Searches for the character sign at the strings list.
//	RETURNS- if found: the corresponding index. otherwise: -1;
int find_sign(char** list, const char *sign, int count);


// arglist - a list of char* arguments (words) provided by the user
// it contains count+1 items, where the last item (arglist[count]) and *only* the last is NULL
// RETURNS - 0 on success, -1 otherwise;
int process_arglist(int count, char** arglist){

    int pipe_index, arrow_index;
	int status;

	if (find_sign(arglist, "&", count) != -1){
		// run arglist[:count-1] command at background;
		// NOTICE: we did not change the signal handler for SIGINT (which is the shell's handler for the signal)
		// because we want to ignore it for a background process;
		// SIGCHLD handler does not have to be reseted to SIG_DFL because it automaticlly does at execvp 
		// (because SIGCHLD's handler was not set to SIG_IGN);

		int pid = fork();
		
        if (pid < 0){
            fprintf(stderr, "Error in fork: %s\n", strerror(errno));
			return 0;
        }

		arglist[count-1] = NULL;

		if (pid == 0){
			// no need to change child's (which is parent's) SIGINT handler (because we want to ignore that signal);
			if (execvp(arglist[0], arglist) < 0){
				fprintf(stderr, "Error in execvp: %s\n", strerror(errno));
				exit(1); // only child exists;
			}
		}
		// Else: we wont get here (execvp does not return if not fails);
		// we do not wait to the child proccess to finish - it runs at the background;
	}

	else if ((pipe_index = find_sign(arglist, "|", count)) != -1){
		// run and collect arglist[:pipe_index]'s output, and input it to arglist[pipe_index+1:count] as running it;
		// using pipes to make stdout of the child proccess the stdin for the second proccess;

		arglist[pipe_index] = NULL;

		int fds[2];
		if (pipe(fds) < 0){
			fprintf(stderr, "Error at pipe: %s\n", strerror(errno));
			return 0;
		}

		int pid = fork();
		if (pid < 0){
			fprintf(stderr, "Error in fork: %s\n", strerror(errno));
			close(fds[0]);
			close(fds[1]);
			return 0;
		}

		if (pid == 0){
			// The writing proccess;

			// reseting the foreground processs' handler for SIGINT to default 
			// (want process to terminate when recieving that signal, and it currently has the SIGINT
			// handler of the parent (shell) which ignores that signal);
			if (set_handler(SIGINT, SIG_DFL) != 0){
				exit(1);
			} 

			close(fds[0]); // closing the reading end of the pipe;

			// replace the stdout of the proccess with the writing end of the pipe;
			if (dup2(fds[1], fileno(stdout)) == -1){
				fprintf(stderr, "Error at duplicate2: %s\n", strerror(errno));
				exit(1);
			} 

			if (execvp(arglist[0], arglist) < 0){
				fprintf(stderr, "Error in execvp: %s\n", strerror(errno));
				exit(1);
			}
		}

		close(fds[1]); // closing unused write end for the parent and its next child
		// (otherwise process will not stop);

		// Child Never gets here because execvp doesnt return if not fails (parent's only code):
		int new_pid = fork();
		if (new_pid < 0){
			fprintf(stderr, "Error in fork: %s\n", strerror(errno));
			return 0;
		}

		if (new_pid == 0){
			// The reading proccess;
			if (set_handler(SIGINT, SIG_DFL) != 0){
				exit(1);
			}
			close(fds[1]); // closing the writing end of the pipe;
			
			// replace the stdin of the proccess with the reading end of the pipe;
			if (dup2(fds[0], fileno(stdin)) == -1){
				fprintf(stderr, "Error at duplicate2: %s\n", strerror(errno));
				exit(1);
			}

			// closing the reading end of the pipe;
			close(fds[0]);
			if (execvp(arglist[pipe_index+1], arglist + pipe_index + 1) < 0){
				fprintf(stderr, "Error in execvp: %s\n", strerror(errno));
				exit(1);
			}
		}

		close(fds[0]); // closing unused read end for the parent;
		
		// Second child never gets here (parent's only code):

		if (waitpid(pid, &status, 0) == -1){
			// SA_RESTART was set. no EINTR;
			if (errno != ECHILD){
				fprintf(stderr, "Error in wait: %s\n", strerror(errno));
				return 0;
			}
		}

		if (waitpid(new_pid, &status, 0) == -1){
			if (errno != ECHILD){
				fprintf(stderr, "Error in wait: %s\n", strerror(errno));
				return 0;
			}
		}

		// closing the fds (we cant do it from the child proccesses as they never back to our control
		// if not failed);
    }

	else if ((arrow_index = find_sign(arglist, ">", count)) != -1){
		// run arglist[:arrow_index] and put its output at the file arglist[arrow_index:count]
		// while creating that file if not allready created;

		arglist[arrow_index] = NULL;

		int fd = open(arglist[arrow_index + 1], O_RDWR | O_CREAT, 0777); // create a w/o file if does not exist;
		if (fd == -1){
			fprintf(stderr, "Error with opening file for writing: %s\n", strerror(errno));
			return 0;
		}

		int pid = fork();
		if (pid < 0){
			fprintf(stderr, "Error at fork: %s\n", strerror(errno));
			return 0;
		}

		//else:
		if (pid == 0){
			// need proc to stop when SIGINT (currently has the handler of the parent which does not terminate upon SIGINT);
			if (set_handler(SIGINT, SIG_DFL) != 0){
				exit(1);
			}
			dup2(fd, fileno(stdout)); // redirecting the stdout of the proccess to the file;
			if (execvp(arglist[0], arglist) < 0){
				fprintf(stderr, "Error in execvp: %s\n", strerror(errno));
				exit(1); 
			}
		}

		// Child never gets here (execvp never returns);
		// This is PARENT's only code:
		if (waitpid(pid, &status, 0) == -1){
			if (errno != ECHILD){
				fprintf(stderr, "Error in wait: %s\n", strerror(errno));
				return 0;
			}
		}

		close(fd);
	}

	else{
		// run arglist[:count];

        int pid = fork();
		
		#ifdef DEBUG
		printf("Child's PID: %d\n", pid);
		#endif

        if (pid < 0){
            fprintf(stderr, "Error in fork: %s\n", strerror(errno));
			return 0;
        }

		if (pid == 0){
			if (set_handler(SIGINT, SIG_DFL) != 0){
				exit(1);
			}
			if (execvp(arglist[0], arglist) < 0){
				fprintf(stderr, "Error in execvp: %s\n", strerror(errno));
				exit(1);
			}
		}

		// PARENT process' code only (execvp not return if not failed);
		if (waitpid(pid, &status, 0) == -1){
			if (errno != ECHILD){
				fprintf(stderr, "Error in wait: %s\n", strerror(errno));
				return 0;
			}
		}
	}

	// if not already failed (i.e exited):
	return 1;
}

//	Searches for the character sign at the strings list.
//	RETURNS: if found - the corresponding index. otherwise- (-1);

int find_sign(char** list, const char *sign, int list_size){
	for(int i=0; i < list_size; i++){
		if (strcmp(list[i], sign) == 0){
			return i;
		}
	}

	return -1;
}