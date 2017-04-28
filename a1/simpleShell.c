#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h> //this was automatic with gcc-6... get with it, SOCS
#include <fcntl.h>
#include <sys/wait.h>

pid_t backgroundChildPids[32];
int childPidNum = 0;
pid_t shellpid;



//there's some memory leak in getcmd that I wasn't able to pick out
//I've already fixed two, and spent hours searching for the last one, please have mercy
int getcmd(char *prompt, char *args[], int *background) {
  int length, i = 0;
  char *token, *loc;
  char *line = NULL;
  size_t linecap = 0;
  printf("%s", prompt);
  length = getline(&line, &linecap, stdin);
  if (length <= 0) {
    exit(-1);
  }

  char *line2 = line; //prevents memory leak somehow
  // Check if background is specified..
  if ((loc = index(line2, '&')) != NULL) {
    *background = 1;
    *loc = ' ';
  } else *background = 0;

  while ((token = strsep(&line2, " \t\n")) != NULL) {
    for (int j = 0; j < strlen(token) + 1; j++) {
      if (token[j] <= 32) {
        token[j] = '\0';
      }
    }
    if (strlen(token) > 0) {
        args[i++] = token;
    }
  }
  return i;
}

static void kill_process() {
  if (getpid() != shellpid) {
    exit(EXIT_FAILURE);
  }
}

int piping(char* args[], int cnt, int split) {
  char* first[split+1];
  char* second[cnt-split];

  //splits args into jobs using '|' as delimiter
  for (int i = 0; i < split; i++) {
    first[i] = malloc((strlen(args[i])+1) * sizeof(char));
    first[i] = strcpy(first[i], args[i]);
  }
  first[split] = NULL;
  for (int i = split + 1, j = 0; i < cnt; i++, j++) {
    second[j] = malloc((strlen(args[i])+1) * sizeof(char));
    second[j] = strcpy(second[j], args[i]);
  }
  second[cnt-split-1] = NULL;

  // printf("%s %s : %s %s\n", first[0], first[1], second[0], second[1]);

  pid_t firstpid = fork();
  if (firstpid == 0) {
    int pipefd[2];
    if (pipe(pipefd) < 0) {
      return -1;
    }
    pid_t pipeWaitPid = getpid();
    pid_t secondpid = fork();

    if (secondpid == 0) { //grandchild of shell
      close(0);
      close(pipefd[1]);
      dup(pipefd[0]);
      waitpid(pipeWaitPid, NULL, 0);
      if (execvp(second[0], second) < 0) { //if execvp fails, process exits
        perror("execvp error");
        exit(EXIT_FAILURE);
      }
    }

    else { //child of shell
      close(1);
      close(pipefd[0]);
      dup(pipefd[1]);
      //printf("%s test\n", first[0]);
      if (execvp(first[0], first) < 0) { //if execvp fails, process exits
        perror("execvp error");
        exit(EXIT_FAILURE);
      }
    }
  }

  else { //parent
    for (int i = 0; i < split; i++) {
      free(first[i]);
    }
    for (int i = 0; i < cnt - split - 1; i++) {
      free(second[i]);
    }
    printf("\n");
    //was trying to find a way to wait on grandchildren, but that's a pain
  }

  return 1;
}

int execBuiltin(char* args[], int bg, int cnt) {

  if (args[0] == NULL) {
    return 1;
  }

  if (strcmp(args[0],"exit") == 0) { //exit input
    exit(EXIT_SUCCESS);
    return 1; //yeah I know
  }

  if (strcmp(args[0],"pwd") == 0) { //pwd input
    printf("%s\n", getcwd(NULL, 0));
    return 1;
  }

  if (strcmp(args[0],"cd") == 0) { //cd input
    if (chdir(args[1]) < 0) {
      printf("Invalid path. \n");
    }
    return 1;
  }

  if (strcmp(args[0], "jobs") == 0) { //jobs input
    printf("Background jobs:\n----------------\n");
    for (int i = 0; i < childPidNum; i++) {
      if (backgroundChildPids[i] == 0) {
        printf("%d : finished\n", i);
      } else {
        printf("%d : %d\n", i, backgroundChildPids[i]);
      }
    }
    return 1;
  }

  if (strcmp(args[0], "fg") == 0) { //fg input
    int jobIndex = atoi(args[1]); //who thought it was a good idea to make atoi return 0 on failure? wtf?
    if (jobIndex < childPidNum) { //because of that, if fg can't find a number it'll fg process 0

      printf("Foreground job %d : PID %d\n", jobIndex, backgroundChildPids[jobIndex]);
      waitpid(backgroundChildPids[jobIndex], NULL, 0);
      //can't switch to allow interrupt, and Prof told us not to worry about stopping inputs to bg processes
    } else {
      printf("Invalid job index. \n");
    }
    return 1;
  }
  for (int i = 0; i < cnt; i++) {
    if (strcmp(args[i], "|") == 0) {
      if (piping(args, cnt, i) < 0) {
        printf("Piping error. \n");
      }
      return 1;
    }
  }
  return 0;
}

int setOutputRedirect(char* cArgs[], int cnt) {
  if (strcmp(cArgs[cnt-2], ">") == 0) {
    close(1);
    fopen(cArgs[cnt-1], "w");

    cArgs[cnt-1] = NULL;
    cArgs[cnt-2] = NULL;
    return (cnt-2);
  }
  return cnt;
}

static void handle_child_termination() {
  pid_t deadChildFinder = waitpid(-1, NULL, WNOHANG);
  if ( deadChildFinder > 0) {
    for (int i = 0; i < childPidNum; i++) {
      if (deadChildFinder == backgroundChildPids[i]) {
        backgroundChildPids[i] = 0;
      }
    }
  }
}

int main(void) {
  char *args[20];
  int bg;
  shellpid = getpid();
  signal(SIGINT, kill_process); //if ctrl-C and pid indicates presently in child, exit with failure
  signal(SIGTSTP, SIG_IGN); //ignore SIGSTP signal from ctrl-D
  signal(SIGCHLD, handle_child_termination);

  while(1) {
    bg = 0;
    int status;
    int cnt = getcmd("\n>> ", args, &bg);

    while (execBuiltin(args, bg, cnt) == 1) { //takes care of builtins
      for (int i = 0; i < cnt; i++) { //resets input array
        memset(args[i], 0, strlen(args[i]));
      }
      cnt = getcmd("\n>> ", args, &bg);
    }

    pid_t childpid = fork();
    if (childpid < 0) {
      perror("fork error");
      exit(EXIT_FAILURE);
    }

    if (childpid == 0) { //child

      if (bg == 1) {
        signal(SIGINT, SIG_IGN); //thanks, Professor- this stops sigint from killing all bg processes
      }
      char* cArgs[cnt];
      for (int i = 0; i < cnt + 1; i++) {
        if (i == cnt) { //must null-terminate the array... many hours were wasted before finding this
          cArgs[i] = '\0';
          break;
        }
        cArgs[i] = malloc((strlen(args[i])+1) * sizeof(char));
        strcpy(cArgs[i], args[i]);
      }
      cnt = setOutputRedirect(cArgs, cnt);
      if (execvp(cArgs[0], cArgs) < 0) { //if execvp fails, process exits
        perror("execvp error");
        exit(EXIT_FAILURE);
      }
    }

    else { //parent
      if (bg == 0) { //if foreground, will wait until completion or interrupt
        waitpid(childpid, &status, 0);
      } else {
        backgroundChildPids[childPidNum++] = childpid; //maintains jobs list
        printf("background job '%s' : PID %d\n", args[0], childpid);
      }
      for (int i = 0; i < cnt - 3; i++) { //frees args
        free(args[i]);
      }
    }
  }
  return 1;
}
