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
#define MAX_HIS 10

char* tokenize(char*);

int main(int argc, char** argv){

  char str[MAX_STR] = {0};
  char* history[MAX_HIS];
  int hist_cnt = 0;
 
  while(1){
    // terminal prompt
  loop:
    memset(str, 0, sizeof(str)); // zero init prompt str
    strcpy(str, getlogin()); // NOTE: possible returned NULL ptr
    str[strlen(str)] = '@';
    gethostname(&str[strlen(str)], 16);
    str[strlen(str)] = ':';
    getcwd(&str[strlen(str)], MAX_STR);
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

    // dont save history cmds to history buff
    if(!strstr(str, "history")){
      // shift cmd history when at max
      if(hist_cnt >= MAX_HIS){
        free(history[0]);
        memcpy(&history[0], &history[1], sizeof(char*) * (--hist_cnt));
        history[9] = NULL;
      }
      // cpy cmd to history buff
      history[hist_cnt] = (char*)calloc(strlen(str) + 1, sizeof(char));
      strcpy(history[hist_cnt++], str);
    }
   
  parse:
    // tokenize input
    i = 0;
    int pipe_count = 0;
    int stdin_redir = 0;
    int stdout_redir = 0;
    int stderr_redir = 0;
    char*** cmd_list = (char***)calloc(MAX_TOK * sizeof(char**), sizeof(char**));
    cmd_list[pipe_count] = (char**)calloc(MAX_TOK * sizeof(char*), sizeof(char*));
    char* stdin_redir_file = (char*)calloc(MAX_STR * sizeof(char), sizeof(char));
    char* stdout_redir_file = (char*)calloc(MAX_STR * sizeof(char), sizeof(char));
    char* stderr_redir_file = (char*)calloc(MAX_STR * sizeof(char), sizeof(char));
    char* token = tokenize(str);
    while(token != NULL){
      if(strcmp(token, "|") == 0){
        cmd_list[++pipe_count] = (char**)calloc(MAX_TOK * sizeof(char*), sizeof(char*));
        i = 0;
        token = tokenize(NULL);
        continue;
      }else if(strcmp(token, ">") == 0){
        stdout_redir = 1;
        i = 0;
        token = tokenize(NULL);
        continue;
      }else if(strcmp(token, "<") == 0){
        stdin_redir = 1;
        i = 0;
        token = tokenize(NULL);
        continue;
      }else if(strcmp(token, "2>") == 0){
        stderr_redir = 1;
        i = 0;
        token = tokenize(NULL);
      }

      if(stdout_redir == 1 && !*stdout_redir_file){
        strcpy(stdout_redir_file, token);
      }else if(stdin_redir == 1 && !*stdin_redir_file){
        strcpy(stdin_redir_file, token);
      }else if(stderr_redir == 1 && !*stderr_redir_file){
        strcpy(stderr_redir_file, token);
      }else{
        cmd_list[pipe_count][i++] = token;
      }
     
      token = tokenize(NULL);
    }

    // bypass execution w/ empty cmd list
    if(!*cmd_list[0][0]){
      goto cleanup;
    }
   
    // execute tokenized command(s)
    if(strcmp(cmd_list[0][0], "cd") == 0){
      chdir(cmd_list[0][1]);
    }else if(strcmp(cmd_list[0][0], "history") == 0){
      if(i == 2 && strcmp(cmd_list[0][1], "clean") == 0){
        // clean history buffer
        for(int i = 0; i < hist_cnt; ++i){
          free(history[i]);
        }
        hist_cnt = 0;
      }else if(i == 3 && strcmp(cmd_list[0][1], "exec") == 0){
        // execute old cmd
        int cmd_index = atoi(cmd_list[0][2]);
        if(cmd_index < MAX_HIS && cmd_index <= hist_cnt){
          strcpy(str, history[cmd_index]);
          goto parse;
        }else{
          write(1, "error: invalid cmd history execution\n", 37);
        }
      }else{
        char index[16] = { 0 };
        // print history buffer
        for(int i = 0; i < hist_cnt; ++i){
          sprintf(index, "%d", i);
          write(1, index, strlen(index));
          write(1, ". ", 2);
          write(1, history[i], strlen(history[i]));
          write(1, "\n", 1);
        }
      }
    }else if(strcmp(cmd_list[0][0], "exit") == 0){

      // manage heap memory & exit
      for(int i = 0; i <= pipe_count; ++i){
        free(cmd_list[i]);
      }
      free(cmd_list);
      free(stdout_redir_file);
      free(stdin_redir_file);
      for(int i = 0; i < hist_cnt; ++i){
        free(history[i]);
      }
      exit(0);
     
    }else{
      int prior_fd;
      int pipe_fd[2];
      int orig_stdout = dup(1); // save stdout for printing errors
      for(int i = pipe_count; i >= 0; --i){ // connect pipes in reverse order
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
          if(stdout_redir == 1 && i == pipe_count){
            int fd = open(stdout_redir_file, O_WRONLY|O_CREAT|O_TRUNC, 0644);
            if(fd == -1){
              write(1, "error: ", 7);
              write(1, "failed to open ", 15);
              write(1, stdout_redir_file, strlen(stdout_redir_file));
              write(1, "\n", 1);
              exit(0);
            }
            dup2(fd, 1);
            close(fd);
          }

          if(stderr_redir == 1 && i == pipe_count){
            int fd = open(stderr_redir_file, O_WRONLY|O_CREAT|O_TRUNC, 0644);
            if(fd == -1){
              write(1, "error: ", 7);
              write(1, "failed to open ", 15);
              write(1, stderr_redir_file, strlen(stderr_redir_file));
              write(1, "\n", 1);
              exit(0);
            }
            dup2(fd, 2);
            close(fd);
          }

          if(stdin_redir == 1 && i == 0){
            int fd = open(stdin_redir_file, O_RDONLY, 0644);
            if(fd == -1){
              write(1, "error: ", 7);
              write(1, "failed to open ", 15);
              write(1, stdin_redir_file, strlen(stdin_redir_file));
              write(1, "\n", 1);
              exit(0);
            }
            dup2(fd, 0);
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

  cleanup:
    // manage heap memory
    for(int i = 0; i <= pipe_count; ++i){
      free(cmd_list[i]);
    }
    free(cmd_list);
    free(stdout_redir_file);
    free(stderr_redir_file);
    free(stdin_redir_file);
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
