#include "csapp.h"

void sigchild_handler(int s);
void sigstop_handler(int s);
void eval(char *cmdline);
void fork_pipe(int n, command *cmd, int bg);
int spawn_proc(int in, int out, command *cmd, int bg);
int pipeCount(char *buf);
int parseline(char *buf, command *cmd, int *cmdNum);
int builtin_command(char **argv);
void jobs_add(pid_t pid, pid_t gid, int bg, int status, char *cmdLine);
void cmdcat(char ** argv, int argc, char * buf);
void jobs_update(pid_t pid, pid_t gid, int dest_state);
int get_jid(pid_t pid, pid_t gid);
void jobs_print();
void jobs_delete(int jid);
void myHistory_add(char *cmdLine);
void myHistory_print(void);
void myHistory_nCmd(int n);

#define CURR_PATH getenv("PWD")

volatile sig_atomic_t pid;
volatile sig_atomic_t curr_child_pid;
job jobList[MAX_JOBS];
int jobNum = 1;
int jobSize = 0;

int main(){
    sigset_t mask, prev;
    Signal(SIGCHLD, sigchild_handler);
    Signal(SIGTSTP, sigstop_handler);
    sigemptyset(&mask);
    Sigaddset(&mask, SIGCHLD);

    char buf[MAX_LINE];
    char cmdLine[MAX_LINE];
    char prevLine[MAX_LINE];

    while(1){
        /* print command prompt */
        printf("CSE4100-MP-P1> ");
        fgets(cmdLine, MAX_LINE, stdin); /* read */
        if (strcmp(prevLine, cmdLine) != 0){ /* If current command isn't duplicated in history, */
            myHistory_add(cmdLine);         /* Add to the history */
        }
        if (feof(stdin)){
            exit(0);
        }
        eval(cmdLine); /* evaluate */
        strcpy(prevLine, cmdLine);  /* For checking the duplication */
    }
    return 0;
}

/* For foreground commands, executed in parent process */
void sigchild_handler(int s){
    int olderrno = errno;
    int status;
    pid = Waitpid(-1, &status, WNOHANG); // 2. Handler 동작 : Reaping

    int jid = get_jid(pid, -1);
    if (jobList[jid].bg == 1){
        char buf[50];
        snprintf(buf, sizeof(buf), "\n[%d]  %d done\n", jid, pid);
        jobs_update(pid, jid, DONE);
        write(STDIN_FILENO, buf, strlen(buf));
        snprintf(buf, sizeof(buf), "CSE4100-MP-P1> ");
        write(STDOUT_FILENO, buf, strlen(buf));
    }
    if (jobList[jid].status != STOPPED){
        jobs_update(pid, jid, DONE);
    }

    errno = olderrno;
} 

void sigstop_handler(int s){
    int olderrno = errno;

    pid_t cpid = curr_child_pid;
    int jid = get_jid(cpid, -1);
    if (!jobList[jid].bg){
        jobs_update(cpid, -1, STOPPED);
        if (kill(cpid, SIGTSTP) < 0){  // SIGCHLD generated -> sigchild_handler 호출
            printf("kill error\n");
        }
        jobList[jid].bg = 1; // SIGINT change process to background job
        printf("\n[%d] %d suspended %s\n", jid, cpid, jobList[jid].cmd);
        pid = -1; // Make pid != 0 so parent process does not wait
    }
    errno = olderrno;
}

void eval(char *cmdline){
    int fd[2];            /* File descriptor array */
    pipe(fd);
    pid_t pid;            /* Process id */
    int bg;
    int pipeCnt;
    int ret, status;
    char buf[MAX_BUF];    /* Modified command line */

    strcpy(buf, cmdline);
    /* Counting pipeline command and create command list */
    pipeCnt = pipeCount(buf)+1;
    command* cmd = malloc(sizeof(command) * pipeCnt);
    pipeCnt = 0;
    bg = parseline(buf, cmd, &pipeCnt);
    if (bg == -1){
        return;  /* Ignore empty lines */
    }
    pipeCnt++; /* Consequently point out the number of command */
               /* No-pipe : 1 */ /* A | B pipe command : 2*/

    /* Only for non-pipeline command */
    if (pipeCnt == 1){
        if (!builtin_command(cmd->argv)){
            fork_pipe(pipeCnt, cmd, bg);
        }
    }
    else{
        fork_pipe(pipeCnt, cmd, bg);
    }
    return;
}

void fork_pipe(int n, command *cmd, int bg){
    sigset_t mask, prev;
    Sigemptyset(&mask);
    Sigaddset(&mask, SIGCHLD);

    char buf[MAX_BUF] = "";
    int ret, status;
    int fd[2];
    int i, j;
    int jid;

    int in = STDIN_FILENO;
    for (i = 0; i < n-1; i++){
        /* Save each command in pipeline */
        cmdcat(cmd[i].argv, cmd[i].argc, buf);
        strcat(buf, "| ");

        /* Redirection */
        pipe(fd);
        spawn_proc(in, fd[1], cmd+i, bg);
        close(fd[1]);
        in = fd[0];
    }

    /* Save last command */
    cmdcat(cmd[i].argv, cmd[i].argc, buf);

    /* Last command of the pipeline */
    sigprocmask(SIG_BLOCK, &mask, &prev);
    if ((pid = fork()) == 0){
        if (in != 0){
            dup2(in, STDIN_FILENO); /* Set STDIN */
            close(in);
        }
        if (execvp(cmd[i].argv[0], (char * const *)cmd[i].argv) == -1){
            printf("%s: Command  not found.\n", buf);
            exit(0);
        }
    }
    /* Add job to the job list */
    if (bg) {
        curr_child_pid = pid;
        jobs_add(pid, getpgid(pid), 1, RUNNING, buf);
        int jid = get_jid(pid, -1); // pid = child process's PID
        printf("[%d]  %d\n", jid, pid); /* Print the child process's PID */
    } 
    else {
        jobs_add(pid, getpgid(pid), 0, RUNNING, buf);
        curr_child_pid = pid;
        /* Wait only for foreground job to terminate */
        pid = 0;
        while (!bg && !pid){
            Sigsuspend(&prev);
        }
        /* When the job changes to DONE state in the SIGCHLD handler, delete it*/
        /* But when the fg job stopped due to ^Z, no delete */
        jobs_delete(get_jid(curr_child_pid, -1)); 
    }
    sigprocmask(SIG_SETMASK, &prev, NULL);
    return;
}

int spawn_proc(int in, int out, command *cmd, int bg){
    sigset_t mask, prev;
    sigemptyset(&mask);
    Sigaddset(&mask, SIGCHLD);

    int ret, status;
    Sigprocmask(SIG_BLOCK, &mask, &prev);
    if ((pid = fork()) == 0){
        if (in != 0){
            dup2(in, STDIN_FILENO);
            close(in);
        }
        if (out != 1){
            dup2(out, STDOUT_FILENO);
            close(out);
        }
        if (execvp(cmd->argv[0], (char * const *)cmd->argv) < 0){
            unix_error("Command not found");
            exit(0);
        }
    }
    pid = 0;
    while (!bg && !pid){
        Sigsuspend(&prev); // 1. 여기서 SIGCHLD 기다린다
    }        
    Sigprocmask(SIG_SETMASK, &prev, NULL);
    return pid;
}

int pipeCount(char *buf){
    int res = 0;
    char *pipeCmd;
    for (int i = 0; i < strlen(buf); i++){
        if (buf[i] == '|'){
            res++;
        }
    }
    return res;
}

int parseline(char *buf, command *cmd, int *cmdNum){
    char *argv[MAX_ARGS];
    char *delim;
    char *quote;
    char *pipeCmd;
    int argc;
    int bg = 0;

    /* Find pipe command */
    while((pipeCmd = strchr(buf, '|'))){
        *pipeCmd = '\0';
        parseline(buf, cmd, cmdNum);
        (*cmdNum)++;     /* First commmand will be executed */
        buf = pipeCmd + 1; /* buf points the next of pipeCmd */
        argc = 0;          /* Initialize arguments */
    }

    /* Replace trailing '\n'*/
    if (buf[strlen(buf)-1] == '\n'){
        buf[strlen(buf)-1] = ' ';
    }

    /* Change ' or " to space */
    while((quote = strchr(buf, '\'')) || (quote = strchr(buf, '\"'))){
        *quote = ' ';
    }

    /* Ignore leading ' ' */
    while(*buf && (*buf == ' ')){
        buf++;
    }

    // ls asdf 있으면... argv[0] = ls. 
    argc = 0;
    while((delim = strchr(buf, ' '))){ /* Find the first space between commands */
        *delim = '\0'; /* Change space into EOF */
        if (!strcmp(buf, "&")){
            bg = 1;
            break;
        }
        cmd[*cmdNum].argv[argc++] = buf;
        buf = delim + 1;
        while(*buf && (*buf == ' ')){ /* Ignore trailing spaces after delim */
            buf++;
        }
    }
    cmd[*cmdNum].argv[argc] = NULL;
    cmd[*cmdNum].argc = argc;

    /* Ignore blank line */
    if (argc == 0){
        return -1;
    }

    /* Check & command */
    int len;
    char last[MAX_BUF];
    strcpy(last, cmd[*cmdNum].argv[argc-1]);
    len = strlen(last);
    if (last[len-1] == '&'){
        cmd[*cmdNum].argv[argc-1][len-1] = '\0';
        bg = 1; /* If there exists & command, return 1 */
    }

    return bg;
}

int builtin_command(char **argv){
    if (!strcmp(argv[0], "quit") && argv[1] == NULL){
        exit(0);
    }
    if (!strcmp(argv[0], "&")){
        return 1;
    }
    if (!strcmp(argv[0], "exit")){
        exit(0);
    }
    /* cd command implementation */
    if (!strcmp(argv[0], "cd")){
        if (argv[1] == NULL || !strcmp(argv[1], "~")){
            /* Move to the home directory */
            if (chdir(getenv("HOME")) < 0){
                unix_error("chdir error");
            }
            return 1;
        }
        /* Move to the current directory */
        if (!strcmp(argv[1], ".")){
            if (chdir(".") < 0){
                unix_error("chdir error");
            }
            return 1;
        }
        /* Move to the parent directory */
        if (!strcmp(argv[1], "..")){
            if (chdir("..") < 0){
                unix_error("chdir error");
            }
            return 1;
        }

        /* Move to the target directory */
        int res = chdir(argv[1]);
        if (res < 0){
            unix_error("chdir error");
        }
        else return 1;
    }

    /* history command implementation */
    if (!strcmp(argv[0], "history")){
        myHistory_print();
        return 1;
    }
    /* !! command implementation */
    if (!strcmp(argv[0], "!!")){
        myHistory_nCmd(-1);
        return 1;
    }
    /* !# command implementation */
    if (argv[0][0] == '!'){
        int num = atoi(argv[0]+1);
        myHistory_nCmd(num);
        return 1;
    }
    /* Jobs command implementation */
    if (!strcmp(argv[0], "jobs")){
        jobs_print();
        return 1;
    }
    /* Stopped or running background -> running foreground */
    if (!strcmp(argv[0], "fg")){
        // Find command with given job number
        int jid;
        if (argv[1] == NULL){
            jid = jobNum;
        }
        else jid = atoi(argv[1]+1);
        if (jid <= 0 || jid > jobNum){
            printf("No such job\n");
            return 1;
        }
        // Run
        char * buf = malloc(sizeof(char) * strlen(jobList[jid].cmd) + 1);
        strcpy(buf, jobList[jid].cmd);
        strcat(buf, "\n");

        /* If the job stopped  */
        if (jobList[jid].status == RUNNING){
            /* First, stop the job */ 
            pid_t j_pid = jobList[jid].pid;
            kill(j_pid, SIGTSTP);
            jobs_update(-1, jid, STOPPED);
        }
        /* And Delete */
        if (jobList[jid].status == STOPPED){
            jobs_update(-1, jid, DONE);
            jobs_delete(jid);
        }
        printf("%s",buf);
        eval(buf); /* Then restart in the foreground */
        free(buf);
        return 1;   
    }
    /* Stopped backgound -> running background*/ 
    if (!strcmp(argv[0], "bg")){
        // Find command with given job number
        int jid;
        if (argv[1] == NULL){
            jid = jobNum;
        }
        else jid = atoi(argv[1]+1);
        if (jid <= 0 || jid > jobNum){
            printf("No such job\n");
            return 1;
        }
        // Update jobs
        jobList[jid].status = DONE;

        // Run
        char * buf = malloc(sizeof(char) * strlen(jobList[jid].cmd) + 2);
        strcpy(buf, jobList[jid].cmd);
        printf("[%d]  %s\n", jid, buf);
        strcat(buf, "&\n");
        eval(buf);
        free(buf);
        return 1;
    }
    /* Terminate process */
    if (!strcmp(argv[0], "kill")){
        // Find command with given job number
        int jid;
        if (argv[1] == NULL){
            jid = jobNum;
        }
        else jid = atoi(argv[1]+1);
        if (jid <= 0 || jid > jobNum){
            printf("No such job\n");
            return 1;
        }
        pid_t j_pid = jobList[jid].pid;
        kill(j_pid, SIGCHLD);
        jobs_update(j_pid, jid, DONE);
        jobs_delete(jid);
        return 1;
    }

    return 0;
}

/* Add certain job with its pid, gid, bg, status, command information */
void jobs_add(pid_t pid, pid_t gid, int bg, int status, char *cmdLine){
    jobList[jobNum].gid = gid;
    jobList[jobNum].pid = pid;
    jobList[jobNum].jid = jobNum;
    jobList[jobNum].bg = bg; /* For background job, bg = 1 */
    strcpy(jobList[jobNum].cmd, cmdLine); /* Command should be already parsed */ 
    jobList[jobNum++].status = status;
    jobSize++;
}

void cmdcat(char ** argv, int argc, char* buf){
    for (int i = 0; i < argc; i++){
        strcat(buf, argv[i]);
        strcat(buf, " ");
    }
}

void jobs_update(pid_t pid, int jid, int dest_state){
    int i = 1;
    while(jobList[i].pid != pid && jobList[i].jid != jid){
        i++;
    }
    if (i > jobNum){
        // printf("job index out of bound; jobNum = %d, i = %d\n", jobNum, i);
        return;
    }
    else jobList[i].status = dest_state;
}

int get_jid(pid_t pid, pid_t gid){
    int i = 1;
    while(jobList[i].pid != pid && jobList[i].gid != gid){
        i++;
    }
    if (i > jobNum){
        return -1;
    }
    return jobList[i].jid;
}


void jobs_print(){
    int j = 1;
    for (int i = 0; i < jobNum; i++){
        if (jobList[i].status == RUNNING){
            printf("[%d] Running %s\n", jobList[i].jid, jobList[i].cmd);
        }
        else if (jobList[i].status == STOPPED){
            printf("[%d] Suspended %s\n", jobList[i].jid, jobList[i].cmd);
        }
        else if (jobList[i].status == DONE){
            printf("[%d] DONE %s\n", jobList[i].jid, jobList[i].cmd);
            jobs_delete(jobList[i].jid);
        }
        else if (jobList[i].status == DELETED){
            continue;
        }
    }
}

void jobs_delete(int jid){
    if (jobList[jid].status == STOPPED){
        return;
    }

    jobList[jid].status = DELETED;
    if (jid == jobNum-1){
        jobNum--;
    }
    jobSize--;
    if (jobSize == 0){
        jobNum = 1;
    }
}

/* Add "cmdLine" to the history.txt */
void myHistory_add(char *cmdLine){
    /* Case 1: if the command starts with !, then ignore */
    if (cmdLine[0] == '!'){
        return;
    }
    FILE *fp;
    char buf[MAX_BUF];
    char prevLine[MAX_BUF];
    char pathName[MAX_BUF];

    /* Variable 'path' stores the path where the history file exists */
    strcpy(pathName, CURR_PATH);
    strcat(pathName, "/history.txt");

    fp = fopen(pathName, "r"); /* Open history file with read-mode */
    if (!fp){
        printf("Error : Cannot open history.txt\n");
        exit(0);
    }
    while(fgets(buf, MAX_LINE, fp) != NULL){
        strcpy(prevLine, buf);
    }
    /* Case 2: if the command is a redundant, then ignore */
    if (!strcmp(prevLine, cmdLine)){
        return;
    }
    fclose(fp);

    /* Case 3: Else, recored current cmdLine */
    fp = fopen(pathName, "a+");
    fprintf(fp, "%s", cmdLine);
    fclose(fp);
}

/* Prints all the "cmdLine" in the history.txt */
void myHistory_print(){
    int i = 1;
    char buf[MAX_BUF];
    char pathName[MAX_BUF];

    /* Variable 'path' stores the path where the history file exists */
    strcpy(pathName, CURR_PATH);
    strcat(pathName, "/history.txt"); 

    FILE * fp = fopen(pathName, "r");
    if (!fp){
        printf("Error : Cannot open history.txt\n");
        return;
    }
    while(fgets(buf, MAX_LINE, fp) != NULL){
        printf("[%d] : %s", i++, buf); /* Prints all the command in history */
    }
    fclose(fp);
}

/* Return n-th "cmdLine" in the history*/
void myHistory_nCmd(int n){
    int lineNum = 1;
    char buf[MAX_BUF];
    char prevLine[MAX_LINE];
    char pathName[MAX_LINE];

    /* Variable 'path' stores the path where the history file exists */
    strcpy(pathName, CURR_PATH);
    strcat(pathName, "/history.txt");

    FILE * fp = fopen(pathName, "r");
    if (!fp){
        printf("Error : Cannot open history.txt\n");
    }
    /* Step 1: Go to the n-th command in history */
    while(fgets(buf, MAX_LINE, fp) != NULL){
        if (lineNum == n){
            printf("%s", buf);  /* Step 2: Print the n-th command */
            myHistory_add(buf); /* Step 3: Add the n-th command in hisotry */
            eval(buf);          /* Step 4: Execute the n-th command */
            return;
        }
        lineNum++;
        strcpy(prevLine, buf);
    }
    if (n >= lineNum){ /* If n is larger then ....*/
        return;
    }
    else{ /* This is for the !! command */
        printf("%s", prevLine);
        eval(prevLine); /* Execute the latest executed command */
    }
}