// SPDX-License-Identifier: BSD-3-Clause

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <fcntl.h>
#include <unistd.h>

#include "cmd.h"
#include "utils.h"

#include <string.h> //for string manipulation functions
#include <stdio.h> //for printf
#include <stdlib.h> //for setenv/getenv
#include <errno.h> //for errno

#define READ		0
#define WRITE		1

static int shell_pwd(void)
{
	char current_directory[500];

	char *r = getcwd(current_directory, 500); //returns current directory

	if (r == NULL)
		return 1;

	printf("%s\n", current_directory);
	return 0;
}

/**
 * Internal change-directory command.
 */

static bool shell_cd(word_t *dir)
{
	if (dir == NULL) //no args
		return true;

	if (dir->next_word != NULL) //many args
		return true;

	const char *path;

	if (dir->expand) { //cd uses environment variable for path
		path = getenv(dir->string);
	} else {
		path = dir->string;
	}

	int r = chdir(path); //works for both absolute and relative paths

	if (r != 0) {
		perror("");
		return false;
	}

	return true;
}

/**
 * Internal exit/quit command.
 */
static int shell_exit(void)
{
	return SHELL_EXIT;
}

void compute_word(word_t *flag, char *auxvalue) //takes a word and computes it
{
	bool first = true;

	while (flag != NULL) { //take all parts of input file
		if (flag->expand) { //environment part
			char *var = getenv(flag->string);

			if (var == NULL) { //environment is not declared
				flag = flag->next_part;
				continue;
			}
			if (first) {
				strcpy(auxvalue, var);
				first = false;
			} else {
				strcat(auxvalue, var);
			}
		} else {
			if (first) {
				strcpy(auxvalue, flag->string);
				first = false;
			} else {
				strcat(auxvalue, flag->string);
			}
		}

		flag = flag->next_part;
	}
}

void redirection_files(simple_command_t *s, bool *changed_in, bool *changed_out, bool *changed_err) //redirects in/out/err into file
{
	if (s->in != NULL) { //input redirection
		char auxvalue[100]; //input file name

		compute_word(s->in, auxvalue);

		int fd = open(auxvalue, O_RDONLY); //open input file

		dup2(fd, STDIN_FILENO); //make 0 file descriptor for input file
		close(fd);
		if (changed_in != NULL)
			*changed_in = true;
	}
	if (s->out != NULL) {//output redirection
		char auxvalue[100]; //output file name

		compute_word(s->out, auxvalue);

		int fd;

		if (s->io_flags == IO_REGULAR) {
			if (s->err != NULL) { //redirect &> so we need to delete stuff from file + open as append
				fd = open(auxvalue, O_WRONLY | O_CREAT | O_TRUNC, 0777); //delete stuff from file
				close(fd);
				fd = open(auxvalue, O_WRONLY | O_CREAT | O_APPEND, 0777); //open file as append
			} else { //simple redirection
				fd = open(auxvalue, O_WRONLY | O_CREAT | O_TRUNC, 0777);
			}
		} else { //append to file
			fd = open(auxvalue, O_WRONLY | O_CREAT | O_APPEND, 0777);
		}
		dup2(fd, STDOUT_FILENO); //make 1 file descriptor for output file
		close(fd);

		if (changed_out != NULL)
			*changed_out = true;
	}
	if (s->err != NULL) { //error redirection
		char auxvalue[100]; //error file name

		compute_word(s->err, auxvalue);

		int fd;

		if (s->io_flags == IO_REGULAR) {
			if (s->out != NULL) { //redirect &> so delete stuff + open as append
				fd = open(auxvalue, O_WRONLY | O_CREAT | O_TRUNC, 0777);
				close(fd);
				fd = open(auxvalue, O_WRONLY | O_CREAT | O_APPEND, 0777);
			} else { //simple redirection
				fd = open(auxvalue, O_WRONLY | O_CREAT | O_TRUNC, 0777);
			}
		} else { //append to file
			fd = open(auxvalue, O_WRONLY | O_CREAT | O_APPEND, 0777);
		}

		dup2(fd, STDERR_FILENO); //make 2 file descriptor for the error file
		close(fd);
		if (changed_err != NULL)
			*changed_err = true;
	}
}

/**
 * Parse a simple command (internal, environment variable assignment,
 * external command).
 */
static int parse_simple(simple_command_t *s, int level, command_t *father)
{
	if (s == NULL) { //bad command
		return 0;
	}

	fflush(stdout);

	if (strcmp(s->verb->string, "cd") == 0 || strcmp(s->verb->string, "pwd") == 0) {//buildin commands (cd and pwd)
		//save file descriptors for stdin, stdout, stderr in case we need to redirect
		int saved_in = dup(STDIN_FILENO);
		int saved_out = dup(STDOUT_FILENO);
		int saved_err = dup(STDERR_FILENO);
		bool changed_in = false;
		bool changed_out = false;
		bool changed_err = false;

		redirection_files(s, &changed_in, &changed_out, &changed_err);

		if (strcmp(s->verb->string, "cd") == 0) { //cd command
			bool r = shell_cd(s->params); //change directory

			fflush(stdout);

			//in case of redirection we need to restore the stdout/stdin/stderr
			if (changed_in) {
				dup2(saved_in, STDIN_FILENO);
				close(saved_in);
			}
			if (changed_out) {
				dup2(saved_out, STDOUT_FILENO);
				close(saved_out);
			}
			if (changed_err) {
				dup2(saved_err, STDERR_FILENO);
				close(saved_err);
			}

			if (r)
				return 0;
			return 1;

		} else if (strcmp(s->verb->string, "pwd") == 0) { //pwd command

			int r = shell_pwd(); //update current_directory

			fflush(stdout);

			//in case of redirection restore stdin/stdout/stderr
			if (changed_in) {
				dup2(saved_in, STDIN_FILENO);
				close(saved_in);
			}
			if (changed_out) {
				dup2(saved_out, STDOUT_FILENO);
				close(saved_out);
			}
			if (changed_err) {
				dup2(saved_err, STDERR_FILENO);
				close(saved_err);
			}

			return r;
		}
	}

	if (strcmp(s->verb->string, "exit") == 0 || strcmp(s->verb->string, "quit") == 0) { //exit/quit command
		int r = shell_exit(); //returns SHELL_EXIT
		return r;
	}

	//environment variables

	if (s->verb->next_part != NULL) {
		if (s->verb->next_part->string[0] == '=') { //variable assignment
			int r;
			char auxvalue[100]; //value for environment variable

			compute_word(s->verb->next_part->next_part, auxvalue);
			r = setenv(s->verb->string, auxvalue, 1); //set variable with value
			return r;
		}
	}

	//external commands

	fflush(stdout); //flush stdout because redirections mess with stdout synch

	int pid = fork();

	if (pid == 0) {//child process

		char *args[50];
		int aux = 0;

		args[aux] = s->verb->string; //pointer to command
		aux++;

		word_t *flag = s->params;

		while (flag != NULL) { //take all parameters
			if (flag->next_part != NULL) { //parameter with multiple parts
				char auxvalue[100]; //computed variable

				compute_word(flag, auxvalue);

				args[aux] = auxvalue; //put computed parameter in args array
				aux++;
				flag = flag->next_word;
				continue;
			}

			if (flag->expand) { //the param is an environment one
				args[aux] = getenv(flag->string);

				if (args[aux] == NULL) {//no match
					args[aux] = "";
				}
			} else { //normal parameter
				args[aux] = flag->string;
			}
			aux++;
			flag = flag->next_word;
		}

		args[aux] = NULL; //put the last NULL in args

		//redirections (no need for restoring since this is the child process)

		redirection_files(s, NULL, NULL, NULL);

		execvp(s->verb->string, args); //executes command and dies if everything goes normal

		printf("Execution failed for '%s'\n", s->verb->string); //in case of execvp failure
		fflush(stdout);
		exit(errno);
	} else { //parent process
		int status;

		waitpid(pid, &status, 0); //wait for child

		if (WIFEXITED(status))
			return WEXITSTATUS(status);
	}

	return 0;
}

/**
 * Process two commands in parallel, by creating two children.
 */
static bool run_in_parallel(command_t *cmd1, command_t *cmd2, int level,
		command_t *father)
{
	/* TODO: Execute cmd1 and cmd2 simultaneously. */

	int pid1 = fork();

	if (pid1 == 0) { //first kiddo
		int pid2 = fork();

		if (pid2 == 0) { //second kiddo
			parse_command(cmd2, level+1, father); //execute second command
			exit(0); //end process

		} else { //first kiddo

			parse_command(cmd1, level+1, father); //execute first command

			int status;

			waitpid(pid2, &status, 0); //wait for second kiddo

			if (WIFEXITED(status)) {
				int r = WEXITSTATUS(status);

				//end the process

				if (r == 0)
					exit(0);
				exit(1);
			}

			exit(1);
		}

	} else { //parent
		int status;

		waitpid(pid1, &status, 0); //wait for first kiddo

		if (WIFEXITED(status)) {
			int r = WEXITSTATUS(status);

			if (r == 0)
				return true;
			return false;
		}
	}

	return true;
}

/**
 * Run commands by creating an anonymous pipe (cmd1 | cmd2).
 */
static bool run_on_pipe(command_t *cmd1, command_t *cmd2, int level,
		command_t *father)
{
	/* TODO: Redirect the output of cmd1 to the input of cmd2. */

	int pipefd[2]; // pipefd[0] read end aka cmd2 ; pipefd[1] write end aka cmd1

	pipe(pipefd); //create the pipe

	int pid1 = fork();

	if (pid1 == 0) { //child
		dup2(pipefd[1], STDOUT_FILENO);
		close(pipefd[1]);

		int r = parse_command(cmd1, level+1, father);

		exit(r);
	}

	//parent
	close(pipefd[1]);

	int pid2 = fork();

	if (pid2 == 0) { //child again
		dup2(pipefd[0], STDIN_FILENO);
		close(pipefd[0]);

		int r = parse_command(cmd2, level+1, father);

		exit(r);
	}

	//parent again

	waitpid(pid1, NULL, 0);
	int status;

	waitpid(pid2, &status, 0);
	close(pipefd[0]);

	if (WIFEXITED(status)) {
		int r = WEXITSTATUS(status);

		if (r == 0)
			return true;
		return false;
	}

	return true; /* TODO: Replace with actual exit status. */
}

/**
 * Parse and execute a command.
 */
int parse_command(command_t *c, int level, command_t *father) //parsing the command from main
{
	/* TODO: sanity checks */

	fflush(stdout);

	if (c == NULL) { //command is blank so root/c is NULL
		return 0;
	}

	if (c->op == OP_NONE) {
		//we have only one command to execute, stored in c->scmd
		int r = parse_simple(c->scmd, 0, c);
		return r;
	}

	int r = 0; //return value for commands
	bool ret;

	switch (c->op) {
	case OP_SEQUENTIAL:

		r = parse_command(c->cmd1, level+1, c);
		r = parse_command(c->cmd2, level+1, c);

		break;

	case OP_PARALLEL:

		ret = run_in_parallel(c->cmd1, c->cmd2, level+1, c);

		if (ret)
			r = 0;
		else
			r = 1;

		break;

	case OP_CONDITIONAL_NZERO:
		/* TODO: Execute the second command only if the first one
		 * returns non zero.
		 */

		r = parse_command(c->cmd1, level+1, c);

		if (r != 0) { //first one returns nonzero
			r = parse_command(c->cmd2, level+1, c);
		}

		break;

	case OP_CONDITIONAL_ZERO:
		/* TODO: Execute the second command only if the first one
		 * returns zero.
		 */

		r = parse_command(c->cmd1, level+1, c);

		if (r == 0) { //first one returns 0
			r = parse_command(c->cmd2, level+1, c);
		}

		break;

	case OP_PIPE:
		/* TODO: Redirect the output of the first command to the
		 * input of the second.
		 */
		ret = run_on_pipe(c->cmd1, c->cmd2, level+1, c);

		if (ret)
			r = 0;
		else
			r = 1;

		break;

	default:
		return SHELL_EXIT;
	}

	return r;
}
