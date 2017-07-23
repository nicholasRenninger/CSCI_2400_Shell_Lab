/* 
 * tsh - A tiny shell program with job control
 * 
 * Nicholas Renninger - nire1188
 * SID: 105492876
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <ctype.h>

/* Misc manifest constants */
#define MAXLINE     1024   			/* max line size */
#define MAXARGS     128   			/* max args on a command line */
#define MAXJOBS     16   			/* max jobs at any point in time */
#define MAXJID    	1 << MAXJOBS    /* max job ID */

/* Job states */
#define UNDEF 	0 	 /* undefined */
#define FG 		1    /* running in foreground */
#define BG 		2    /* running in background */
#define ST 		3    /* stopped */

/* 
 * Jobs states: FG (foreground), BG (background), ST (stopped)
 * Job state transitions and enabling actions:
 *     FG -> ST  : ctrl-z
 *     ST -> FG  : fg command
 *     ST -> BG  : bg command
 *     BG -> FG  : fg command
 * At most 1 job can be in the FG state.
 */

/* Global variables */
extern char **environ;      /* defined in libc */
char prompt[] = "tsh> ";    /* command line prompt (DO NOT CHANGE) */
int verbose = 0;            /* if true, print additional output */
int nextjid = 1;            /* next job ID to allocate */
char sbuf[MAXLINE];         /* for composing sprintf messages */

struct job_t {              /* The job struct */
	pid_t pid;              /* job PID */
	int jid;                /* job ID [1, 2, ...] */
	int state;              /* UNDEF, BG, FG, or ST */
	char cmdline[MAXLINE];  /* command line */
};

struct job_t jobs[MAXJOBS]; /* The job list */
/* End global variables */


/* Function prototypes */

/* Here are the functions that you will implement */
void eval(char *cmdline);
int builtin_cmd(char **argv);
void do_bgfg(char **argv, int isBG);
void waitfg(pid_t pid);

void sigchld_handler(int sig);
void sigtstp_handler(int sig);
void sigint_handler(int sig);

/* Here are helper routines that we've provided for you */
int parseline(const char *cmdline, char **argv); 
void sigquit_handler(int sig);

void clearjob(struct job_t *job);
void initjobs(struct job_t *jobs);
int maxjid(struct job_t *jobs); 
int addjob(struct job_t *jobs, pid_t pid, int state, char *cmdline);
int changeJobState(struct job_t *jobs, pid_t pid, int state);
int deletejob(struct job_t *jobs, pid_t pid); 
pid_t fgpid(struct job_t *jobs);
struct job_t *getjobpid(struct job_t *jobs, pid_t pid);
struct job_t *getjobjid(struct job_t *jobs, int jid); 
int pid2jid(pid_t pid); 
int jid2pid(struct job_t *job);
void listjobs(struct job_t *jobs);
void printJob(struct job_t *jobs, pid_t pid);

void usage(void);
void unix_error(char *msg);
void app_error(char *msg);
typedef void handler_t(int);
handler_t *Signal(int signum, handler_t *handler);

/*
 * main - The shell's main routine 
 */
int main(int argc, char **argv){

	char c;
	char cmdline[MAXLINE];
	int emit_prompt = 1; /* emit prompt (default) */

	/* Redirect stderr to stdout (so that driver will get all output
	 * on the pipe connected to stdout) */
	dup2(1, 2);

	/* Parse the command line */
	while ((c = getopt(argc, argv, "hvp")) != EOF){
		switch (c) {
			case 'h':             /* print help message */  
				usage();
				break;
			case 'v':             /* emit additional diagnostic info */
				verbose = 1;
				break;
			case 'p':             /* don't print a prompt */
				emit_prompt = 0;  /* handy for automatic testing */
				break;
			default:
				usage();
	   }
	}

	/* Install the signal handlers */

	/* These are the ones you will need to implement */
	Signal(SIGINT,  sigint_handler);   /* ctrl-c */
	Signal(SIGTSTP, sigtstp_handler);  /* ctrl-z */
	Signal(SIGCHLD, sigchld_handler);  /* Terminated or stopped child */

	/* This one provides a clean way to kill the shell */
	Signal(SIGQUIT, sigquit_handler); 

	/* Initialize the job list */
	initjobs(jobs);

	/* Execute the shell's read/eval loop */
	while (1) {

		/* Read command line */
		if (emit_prompt){
			printf("%s", prompt);
			fflush(stdout);
		}

		/* read buffer from stdin into cmdline */
		if ((fgets(cmdline, MAXLINE, stdin) == NULL) && ferror(stdin))
			app_error("fgets error");
		
		/* End of file (ctrl-d) */
		if (feof(stdin)) { 
			fflush(stdout);
			exit(0);
		}

		/* Evaluate the command line */
		eval(cmdline);
		fflush(stdout);
		fflush(stdout);
	} 

	exit(0); /* control never reaches here */
}
  
/* 
 * eval - Evaluate the command line that the user has just typed in
 * 
 * If the user has requested a built-in command (quit, jobs, bg or fg)
 * then execute it immediately. Otherwise, fork a child process and
 * run the job in the context of the child. If the job is running in
 * the foreground, wait for it to terminate and then return.  Note:
 * each child process must have a unique process group ID so that our
 * background children don't receive SIGINT (SIGTSTP) from the kernel
 * when we type ctrl-c (ctrl-z) at the keyboard.  
*/
void eval(char *cmdline){

	char* argv[MAXARGS]; 	// arguments sent to the execve() system call
	char buf[MAXLINE]; 		// buffer storing entered command line
	pid_t pid; 				// process ID of current process
	int isBG;				// determines whether the job should be run in the bg or fg
	sigset_t mask;			// mask used to block child process mask

	strcpy(buf, cmdline); 	// read the contents of stdin into buf from the cmdline
	
	/* Determine if process should be run in the bg/fg, as well as copy args
	 * from stdin into argv.
	 */
	isBG = parseline(buf, argv); 

	/* Do nothing on an empty line. */
	if (argv[0] == NULL)
		return;


	/* Check if the comman is built in. if not, spawn child process to handle the 
	 * execve() system call.
	 */
	if (!builtin_cmd(argv)){

		// initialize a mask to block SIGCHILD
		sigemptyset(&mask); 
		
		// block SIGCHILD
		sigaddset(&mask, SIGCHLD);

		// apply masks		
		sigprocmask(SIG_BLOCK, &mask, NULL);


		/* Use a child process to run the inputed job. */
		if (( (pid = fork()) == 0 )){

			// unblock SIGCHILD, SIGINT, SIGTSTP
			sigprocmask(SIG_UNBLOCK, &mask, NULL);

			/* Need to reset process group of child so that it runs in its own pgrp, 
			 * not from that of the calling shell. 
			 */
			setpgid(0, 0);

			/* Call the system call to execute the command given on command line. */
			if (execve(argv[0], argv, environ) < 0){

				// if the system call does not function, inform user of error and quit.
				printf("%s: Command not found\n", argv[0]);
				exit(0);
			}


		}

		
		/* The process is the Parent. Wait for fg job to terminate. */
		if (!isBG){

			// need to add job to the BG here
			if(!addjob(jobs, pid, FG, cmdline))
				unix_error("added empty job\n.");

			// unblock SIGCHILD
			sigprocmask(SIG_UNBLOCK, &mask, NULL);

			waitfg(pid);
			return;

		} else {

			if(!addjob(jobs, pid, BG, cmdline))
				unix_error("added empty job\n.");

			// unblock SIGCHILD
			sigprocmask(SIG_UNBLOCK, &mask, NULL);

			printJob(jobs, pid);
			return;
		}

	}

}

/* 
 * parseline - Parse the command line and build the argv array.
 * 
 * Characters enclosed in single quotes are treated as a single
 * argument.  Return true if the user has requested a BG job, false if
 * the user has requested a FG job.  
 */
int parseline(const char *cmdline, char **argv){

	static char array[MAXLINE]; /* holds local copy of command line */
	char *buf = array;          /* ptr that traverses command line */
	char *delim;                /* points to first space delimiter */
	int argc;                   /* number of args */
	int bg;                     /* background job? */

	strcpy(buf, cmdline);
	buf[strlen(buf)-1] = ' ';  /* replace trailing '\n' with space */

	while (*buf && (*buf == ' ')) /* ignore leading spaces */
	   buf++;

	/* Build the argv list */
	argc = 0;
	if (*buf == '\'') {
	   
	   buf++;
	   delim = strchr(buf, '\'');

	} else {

	   delim = strchr(buf, ' ');
	}

	while (delim) {
		
		argv[argc++] = buf;
		*delim = '\0';
		buf = delim + 1;

		while (*buf && (*buf == ' ')) /* ignore spaces */
		   buf++;

		if (*buf == '\'') {
			
			buf++;
			delim = strchr(buf, '\'');

		} else {

			delim = strchr(buf, ' ');
		}
	}

	argv[argc] = NULL;
	
	if (argc == 0)  /* ignore blank line */
		return 1;

	/* should the job run in the background? */
	if ((bg = (*argv[argc-1] == '&')) != 0)
	   argv[--argc] = NULL;

	return bg;
}

/* 
 * builtin_cmd - If the user has typed a built-in command then execute
 *    it immediately.  
 */
int builtin_cmd(char **argv) {

	// QUIT
	if (!strcmp(argv[0], "quit")) {

		exit(0);

	// JOBS
	} else if (!strcmp(argv[0], "jobs")){
		
		listjobs(jobs);
		return 1;

	// FG handling
	} else if (!strcmp(argv[0], "fg")) {

		int isBG = 0;
		do_bgfg(argv, isBG);
		return 1;
	
	// BG handling
	} else if (!strcmp(argv[0], "bg")) {

		int isBG = 1;
		do_bgfg(argv, isBG);
		return 1;
	
	// Ignore the Single "&" in a command line
	} else if (!strcmp(argv[0], "&")) {

		return 1;
	}

	return 0;     /* not a builtin command */
}

/* 
 * do_bgfg - Execute the builtin bg and fg commands
 */
void do_bgfg(char **argv, int isBG) {

	struct job_t *currentJob;
	pid_t pid;
	int jid;
	int jobState;
	char bg_fg_JID_char;
	char *cmdStr = "fg";

	if (isBG)
		cmdStr = "bg";

	/***** get jid to execute on based on user input *****/

	/* Need to get the jid from the input command */

	/* Check that the user actually entered argument to the fg/bg command */
	if (argv[1] == NULL) {

		printf("%s command requires PID or %%jobid argument\n", cmdStr);
		return;

	} 

	/* Check if the argument to bg/fg is valid */
	char *bg_fg_arg = argv[1];

	if (bg_fg_arg[0] == '%') { // user entered a job fg/bg command

		if (!isdigit(bg_fg_arg[1])) {

			printf("%s: argument must be a PID or %%jobid\n", cmdStr);
			return;

		} else { // used entered a valid jid

			bg_fg_JID_char = bg_fg_arg[1];
			
			// convert char jid to int jid
			jid = bg_fg_JID_char - '0'; 

			// get job from jobs list and error checks
			if ( (currentJob = getjobjid(jobs, jid)) == NULL){
				printf("%s: No such job\n", argv[1]);
				return;
			}

			// retrieve the pid from the jid and error check
			if ( (pid = jid2pid(currentJob)) < 1){

				printf("(%d): No such process\n", pid);
				return;
			}
		}

	} else { // user entered a pid

		if ( !isdigit(bg_fg_arg[0]) ) { // used entered a non-numeric pid

			printf("%s: argument must be a PID or %%jobid\n", cmdStr);
			return;

		} else { // user entered a valid pid

			// cast the entered pid into an int
			pid = atoi(argv[1]);


			// check if the entered pid is in jobs list
			if ( (jid = pid2jid(pid)) == 0){

				printf("(%d): No such process\n", pid);
				return;
			}
		}

	}
	

	/***** execute bg/fg commands based on user input *****/
	
	// Do conditional work based on if
	if (isBG) {

		jobState = BG;
		
		/* Common Code between BG and FG jobs */
		kill(-pid, SIGCONT); // continue all jobs in current pgrp
		changeJobState(jobs, pid, jobState);

		// print the BG job
		printJob(jobs, pid);

	} else {

		jobState = FG;

		/* Common Code between BG and FG jobs */
		kill(-pid, SIGCONT); // continue all jobs in current pgrp 
		changeJobState(jobs, pid, jobState);

		// need to block all other processes until FG job is done
		waitfg(pid);
	}

}

/* 
 * waitfg - Block until process pid is no longer the foreground process
 */
void waitfg(pid_t pid) {
 
	while(pid == fgpid(jobs)) {
		// simply spin this proccess until the job isn't in FG
		sleep(0.1);
	}

	return;

}


/*****************
 * Signal handlers
 *****************/

/* 
 * sigchld_handler - The kernel sends a SIGCHLD to the shell whenever
 *     a child job terminates (becomes a zombie), or stops because it
 *     received a SIGSTOP or SIGTSTP signal. The handler reaps all
 *     available zombie children, but doesn't wait for any other
 *     currently running children to terminate.  
 */
void sigchld_handler(int sig) {

	pid_t pid;
	int child_status;

	/* read child processes when they terminate, and delete them from jobs list. */
	while ((pid = waitpid(-1, &child_status, WNOHANG | WUNTRACED)) > 0){

		// If the child was stopped, updated its status in jobs list
		if(WIFSTOPPED(child_status)){

			if(changeJobState(jobs, pid, ST)){

				printf("Job [%d] (%d) stopped by signal %d\n",
						pid2jid(pid), pid, WSTOPSIG(child_status));

			} else {

				unix_error("invalid job status change target");
			}

		} else { // otherwise if the child was killed, deleted it from jobs list

			if (WIFSIGNALED(child_status)) {

				printf("Job [%d] (%d) terminated by signal %d\n",
						pid2jid(pid), pid, WTERMSIG(child_status));
			}

			deletejob(jobs, pid);
		}

	}

	return;
}

/* 
 * sigint_handler - The kernel sends a SIGINT to the shell whenver the
 *    user types ctrl-c at the keyboard.  Catch it and send it along
 *    to the foreground job.  
 */
void sigint_handler(int sig) {

	// get the current FG process to send the signal to.	
	pid_t currentFGPID = fgpid(jobs);
	int killStatus;

	if (currentFGPID > 0) {

		// send SIGINT to every process in the current pgrp
		if( (killStatus = kill(-currentFGPID, sig)) < 0)
			unix_error("Kill function did not return properly");

	}

	return;
}

/*
 * sigtstp_handler - The kernel sends a SIGTSTP to the shell whenever
 *     the user types ctrl-z at the keyboard. Catch it and suspend the
 *     foreground job by sending it a SIGTSTP.  
 */
void sigtstp_handler(int sig) {

	// get the current FG process to send the signal to.
	pid_t currentFGPID = fgpid(jobs);
	int killStatus;

	if (currentFGPID > 0) {

		// send SIGTSTP to every process in the current pgrp
		if( (killStatus = kill(-currentFGPID, sig)) < 0)
			unix_error("Kill function did not return properly");

	} else {

		unix_error("Error retreving foreground job");
	}

	return;
}

/*********************
 * End signal handlers
 *********************/

/***********************************************
 * Helper routines that manipulate the job list
 **********************************************/

/* clearjob - Clear the entries in a job struct */
void clearjob(struct job_t *job) {
	job->pid = 0;
	job->jid = 0;
	job->state = UNDEF;
	job->cmdline[0] = '\0';
}

/* initjobs - Initialize the job list */
void initjobs(struct job_t *jobs) {
	int i;

	for (i = 0; i < MAXJOBS; i++)
	clearjob(&jobs[i]);
}

/* maxjid - Returns largest allocated job ID */
int maxjid(struct job_t *jobs) {
	
	int i, max=0;

	for (i = 0; i < MAXJOBS; i++)
	if (jobs[i].jid > max)
		max = jobs[i].jid;

	return max;
}

/* addjob - Add a job to the job list */
int addjob(struct job_t *jobs, pid_t pid, int state, char *cmdline) {

	int i;
	
	if (pid < 1)
		// unix_error("adding job failed.");
		return 0;

	for (i = 0; i < MAXJOBS; i++) {
		if (jobs[i].pid == 0) {
			
			jobs[i].pid = pid;
			jobs[i].state = state;
			jobs[i].jid = nextjid++;

			if (nextjid > MAXJOBS)
				nextjid = 1;
			strcpy(jobs[i].cmdline, cmdline);

			if(verbose)
				printf("Added job [%d] %d %s\n", jobs[i].jid, 
					   jobs[i].pid, jobs[i].cmdline);
			
			return 1;
		}
	}

	printf("Tried to create too many jobs\n");
	return 0;
}

/* changeJobState - find job, and change its state to the new state */
int changeJobState(struct job_t *jobs, pid_t pid, int state){

	
	struct job_t *currentJob;

	if( (currentJob = getjobpid(jobs, pid)) ){
		currentJob->state = state;
		return 1;
	}

	return 0;
}

/* deletejob - Delete a job whose PID = pid from the job list */
int deletejob(struct job_t *jobs, pid_t pid) {

	int i;

	if (pid < 1)
		return 0;

	for (i = 0; i < MAXJOBS; i++) {
		if (jobs[i].pid == pid) {
			
			clearjob(&jobs[i]);
			nextjid = maxjid(jobs)+1;
			return 1;
		}
	}

	return 0;
}

/* fgpid - Return PID of current foreground job, 0 if no such job */
pid_t fgpid(struct job_t *jobs) {
	
	int i;

	for (i = 0; i < MAXJOBS; i++)
		if (jobs[i].state == FG)
			return jobs[i].pid;

	return 0;
}

/* getjobpid  - Find a job (by PID) on the job list */
struct job_t *getjobpid(struct job_t *jobs, pid_t pid) {
	
	int i;

	if (pid < 1)
		return NULL;

	for (i = 0; i < MAXJOBS; i++)
		if (jobs[i].pid == pid)
			return &jobs[i];

	return NULL;
}

/* getjobjid  - Find a job (by JID) on the job list */
struct job_t *getjobjid(struct job_t *jobs, int jid) {

	int i;

	if (jid < 1)
		return NULL;

	for (i = 0; i < MAXJOBS; i++)
		if (jobs[i].jid == jid)
			return &jobs[i];

	return NULL;
}

/* pid2jid - Map process ID to job ID. return 0 if pid is not in jobs list. */
int pid2jid(pid_t pid) {

	int i;

	if (pid < 1)
		return 0;

	for (i = 0; i < MAXJOBS; i++)
		if (jobs[i].pid == pid)
			return jobs[i].jid;

	return 0;
}

/* jid2pid - Map job ID to process ID. return 0 if job is not in job list. */
int jid2pid(struct job_t *job) {

	int i;
	int isValid_JID = 0;
	int jid = job->jid;

	/* Check to make sure that the given jobs is */
	if (jid < 1)
		return isValid_JID;
	
	for (i = 0; i < MAXJOBS; i++)
		if (jobs[i].jid == jid)
			isValid_JID = 1;

	if (isValid_JID)
		return job->pid;

	return isValid_JID;
}

/* listjobs - Print the job list */
void listjobs(struct job_t *jobs) {

	int i;
	
	for (i = 0; i < MAXJOBS; i++) {
		if (jobs[i].pid != 0) {

			printf("[%d] (%d) ", jobs[i].jid, jobs[i].pid);

				switch (jobs[i].state) {
					case BG: 
						printf("Running ");
						break;
					case FG: 
						printf("Foreground ");
						break;
					case ST: 
						printf("Stopped ");
						break;
					default:
						printf("listjobs: Internal error: job[%d].state=%d ", 
							   i, jobs[i].state);
				}

			printf("%s", jobs[i].cmdline);
		}
	}
}

/* printJob - Print the given job */
void printJob(struct job_t *jobs, pid_t pid) {
	
	struct job_t *currentJob = getjobpid(jobs, pid);

	if (currentJob->pid != 0) {

		printf("[%d] (%d) ", currentJob->jid, currentJob->pid);
		printf("%s", currentJob->cmdline);
	}
}

/******************************
 * end job list helper routines
 ******************************/


/***********************
 * Other helper routines
 ***********************/

/*
 * usage - print a help message
 */
void usage(void) {

	printf("Usage: shell [-hvp]\n");
	printf("   -h   print this message\n");
	printf("   -v   print additional diagnostic information\n");
	printf("   -p   do not emit a command prompt\n");
	exit(1);
}

/*
 * unix_error - unix-style error routine
 */
void unix_error(char *msg) {

	fprintf(stdout, "%s: %s\n", msg, strerror(errno));
	exit(1);
}

/*
 * app_error - application-style error routine
 */
void app_error(char *msg) {

	fprintf(stdout, "%s\n", msg);
	exit(1);
}

/*
 * Signal - wrapper for the sigaction function
 */
handler_t *Signal(int signum, handler_t *handler) {

	struct sigaction action, old_action;

	action.sa_handler = handler;  
	sigemptyset(&action.sa_mask); /* block sigs of type being handled */
	action.sa_flags = SA_RESTART; /* restart syscalls if possible */

	if (sigaction(signum, &action, &old_action) < 0)
		unix_error("Signal error");

	return (old_action.sa_handler);
}

/*
 * sigquit_handler - The driver program can gracefully terminate the
 *    child shell by sending it a SIGQUIT signal.
 */
void sigquit_handler(int sig) {

	printf("Terminating after receipt of SIGQUIT signal\n");
	exit(1);
}



