#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <limits.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <fcntl.h>

#define MAX_STR 256
#define MAX_TOK 16

char* tokenize(char*);

int main(int argc, char** argv){

  char str[MAX_STR] = {0};

  while(1){
    // terminal prompt
  loop:
    memset(str, 0, sizeof(str)); // zero init cwd str
    getcwd(str, MAX_STR);
    write(1, str, sizeof(str));
    write(1, "$ ", 2);

    // read input
    int i = 0;
    memset(str, 0, sizeof(str)); // zero init cmd str
    while(1){
      read(1, &str[i++], 1);
      if(i == MAX_STR && str[i-1] != '\n'){ // detect overflow
        write(1, "error: command length exceeds buffer size\n", 42);
        while(getchar() != '\n'){} // flush stdin
        goto loop;
      }
      if(str[i-1] == '\n'){
        if(i == 1) goto loop; // if only 'enter', restart loop
        str[i-1] = '\0'; // replace newline with null char
        break;
      }
    }

    // tokenize input
    i = 0;
    int pipe_count = 0;
    int redirect_flag = 0;
    char*** cmd_list = (char***)malloc(MAX_TOK * sizeof(char**));
    cmd_list[pipe_count] = (char**)malloc(MAX_TOK * sizeof(char*));
    char* redirect_file = (char*)malloc(MAX_TOK * sizeof(char));
    char* token = tokenize(str);
    while(token != NULL){
      if(strcmp(token, "|") == 0){
        cmd_list[++pipe_count] = (char**)malloc(MAX_TOK * sizeof(char*));
        i = 0;
        token = tokenize(NULL);
        continue;
      }else if(strcmp(token, ">") == 0){
        redirect_flag = 1;
        i = 0;
        token = tokenize(NULL);
        continue;
      }
      if(redirect_flag == 0){
        cmd_list[pipe_count][i++] = token;
      }else{
        strcpy(redirect_file, token);
      }
      token = tokenize(NULL);
    }

    // execute tokenized command(s)
    if(strcmp(cmd_list[0][0], "cd") == 0){
      chdir(cmd_list[0][1]);
    }else if(strcmp(cmd_list[0][0], "exit") == 0){
      exit(0); // system will clean up malloc'd mem
    }else{
      int prior_fd;
      int pipe_fd[2];
      int orig_stdout = dup(1); // save stdout for printing errors
      for(int i = pipe_count; i >= 0; --i){
        pipe(pipe_fd); // creates pipes
        if(fork() == 0){

          // logic for connecting pipes
          if(pipe_count != 0){
            if(i == pipe_count){
	      dup2(pipe_fd[0], 0);
            }else if(i == 0){
	      dup2(prior_fd, 1);
	      close(pipe_fd[0]);
            }else{
	      dup2(pipe_fd[0], 0);
	      dup2(prior_fd, 1);
            }
	    close(pipe_fd[1]);
          }

          // file redirection logic
          if(redirect_flag == 1 && i == pipe_count){
            int fd = open(redirect_file, O_WRONLY|O_CREAT|O_TRUNC, 0644);
	    dup2(fd, 1);
	    close(fd);
          }

          // execute next command
	  execvp(cmd_list[i][0], cmd_list[i]);
          // exec calls do not return upon success, print error otherwise
	  dup2(orig_stdout, 1); // reset stdout to original fd
	  write(1, "error: ", 7);
	  write(1, cmd_list[i][0], strlen(cmd_list[i][0]));
	  write(1, ": command not found\n", 20);
	  exit(0);
        }

        // manage pipes
        if(i != pipe_count){
	  close(prior_fd);
        }
        prior_fd = pipe_fd[1];
	close(pipe_fd[0]);
      }

      // wait on all children
      for(int i = 0; i <= pipe_count; ++i){
	wait(NULL);
      }
    }

    // manage heap memory
    for(int i = 0; i <= pipe_count; ++i){
      free(cmd_list[i]);
    }
    free(cmd_list);
    free(redirect_file);
  }
}

char* tokenize(char* str){
  char* token;
  static char* s;
  
  if(str == NULL && s == NULL){
    return NULL;
  }else if(str != NULL){
    s = str;
  }

  while(*s == ' '){
    ++s;
  }
  
  token = s;

  while(1){
    char c = *(s++);
    if(c == ' '){
      *(s-1) = 0x00;
      break;
    }else if(c == 0x00){
      s = NULL;
      break;
    }
  }
  return *token == 0x00 ? NULL : token;
}
