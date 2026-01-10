#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_STR 256
#define MAX_TOK 16
#define MAX_HIS 10

char *tokenize(char *);

int main(int argc, char **argv) {
  int fd = 1;
  char str[MAX_STR] = {0};
  char *history[MAX_HIS];
  unsigned int hist_cnt = 0;

  char *user_name = getlogin();

  if (argc != 1 && argc != 2) {
    write(1, "error: invalid cli arguments\n", 29);
    exit(-1);
  }

  // open script file for executing multiple cmds
  if (argc == 2) {
    fd = open(argv[1], O_RDONLY);
    if (fd == -1) {
      write(1, "error: invalid script file ", 27);
      write(1, argv[1], strlen(argv[1]));
      write(1, "\n", 1);
      exit(-1);
    }
  }

  while (1) {
    // terminal prompt
  loop:
    // init prompt string
    memset(str, 0, sizeof(str));
    if (user_name == NULL) {
      strncpy(str, "unknown", MAX_STR);
    } else {
      strncpy(str, user_name, MAX_STR);
      str[MAX_STR] = '\0';
    }
    str[strlen(str)] = '@';
    gethostname(&str[strlen(str)], 16);
    str[strlen(str)] = ':';
    getcwd(&str[strlen(str)], MAX_STR);
    // if not processing commands from a file
    // write terminal prompt line
    if (fd == 1) {
      write(1, str, sizeof(str));
      write(1, "$ ", 2);
    }

    // read input
    int i = 0;
    memset(str, 0, sizeof(str));  // zero init cmd str
    while (1) {
      int bytes_read = read(fd, &str[i++], 1);
      if (bytes_read == 0 || bytes_read == -1) {
        if (fd != 1) {
          close(fd);
          exit(0);
        } else {
          write(1, "error: input read failure\n", 26);
          exit(-1);
        }
      }
      if (i == MAX_STR && str[i - 1] != '\n') {  // detect overflow
        write(1, "error: command length exceeds buffer size\n", 42);
        while (getchar() != '\n') {
        }  // flush stdin
        goto loop;
      }
      if (str[i - 1] == '\n') {
        if (i == 1) goto loop;  // if only 'enter', restart loop
        str[i - 1] = '\0';      // replace newline with null char
        break;
      }
    }

    // dont save history/help cmds to history buff
    if (!strstr(str, "history") && !strstr(str, "help") && !strstr(str, "?")) {
      // shift cmd history when at max
      if (hist_cnt >= MAX_HIS) {
        free(history[0]);
        memcpy(&history[0], &history[1], sizeof(char *) * (--hist_cnt));
        history[MAX_HIS - 1] = NULL;
      }
      // cpy cmd to history buff
      history[hist_cnt] = (char *)calloc(strlen(str) + 1, sizeof(char));
      strcpy(history[hist_cnt++], str);
    }

    // flag when processing a multi command line (cmd ; cmd ; cmd)
    int multi_command = 0;

  parse:
    // tokenize input
    i = 0;
    int pipe_count = 0;
    int stdin_redir = 0;
    int stdout_redir = 0;
    int stderr_redir = 0;
    char ***cmd_list =
        (char ***)calloc(MAX_TOK * sizeof(char **), sizeof(char **));
    cmd_list[pipe_count] =
        (char **)calloc(MAX_TOK * sizeof(char *), sizeof(char *));
    char *stdin_redir_file =
        (char *)calloc(MAX_STR * sizeof(char), sizeof(char));
    char *stdout_redir_file =
        (char *)calloc(MAX_STR * sizeof(char), sizeof(char));
    char *stderr_redir_file =
        (char *)calloc(MAX_STR * sizeof(char), sizeof(char));

    char *token = NULL;
    if (multi_command == 0) {
      token = tokenize(str);
    } else {
      token = tokenize(NULL);
      multi_command = 0;
    }

    while (token != NULL) {
      if (strcmp(token, ";") == 0) {
        multi_command = 1;
        break;
      } else if (strcmp(token, "|") == 0) {
        cmd_list[++pipe_count] =
            (char **)calloc(MAX_TOK * sizeof(char *), sizeof(char *));
        i = 0;
        token = tokenize(NULL);
        continue;
      } else if (strcmp(token, ">") == 0) {
        stdout_redir = 1;
        i = 0;
        token = tokenize(NULL);
        continue;
      } else if (strcmp(token, "<") == 0) {
        stdin_redir = 1;
        i = 0;
        token = tokenize(NULL);
        continue;
      } else if (strcmp(token, "2>") == 0) {
        stderr_redir = 1;
        i = 0;
        token = tokenize(NULL);
      }

      if (stdout_redir == 1 && !*stdout_redir_file) {
        strcpy(stdout_redir_file, token);
      } else if (stdin_redir == 1 && !*stdin_redir_file) {
        strcpy(stdin_redir_file, token);
      } else if (stderr_redir == 1 && !*stderr_redir_file) {
        strcpy(stderr_redir_file, token);
      } else {
        cmd_list[pipe_count][i++] = token;
      }

      token = tokenize(NULL);
    }

    // bypass execution w/ empty cmd list
    if (!*cmd_list[0] || !*cmd_list[0][0]) {
      goto cleanup;
    }

    // execute tokenized command(s)
    if (strcmp(cmd_list[0][0], "help") == 0 || strcmp(cmd_list[0][0], "?") == 0) {
      write(1, "0. cd <path> - change directory\n", 33);
      write(1, "1. history - list input command history\n", 41);
      write(1, "2. history clean - remove all commands from history\n", 53);
      write(1, "3. history exec <n> - execute the nth command from this list\n", 62);
      write(1, "4. exit - terminate sesh instance\n", 35);
    } else if (strcmp(cmd_list[0][0], "cd") == 0) {
      chdir(cmd_list[0][1]);
    } else if (strcmp(cmd_list[0][0], "history") == 0) {
      if (i == 2 && strcmp(cmd_list[0][1], "clean") == 0) {
        // clean history buffer
        for (int i = 0; i < hist_cnt; ++i) {
          free(history[i]);
        }
        hist_cnt = 0;
      } else if (i == 3 && strcmp(cmd_list[0][1], "exec") == 0) {
        // execute old cmd
        unsigned int cmd_index = atoi(cmd_list[0][2]);
        if (cmd_index < MAX_HIS && cmd_index <= hist_cnt) {
          strcpy(str, history[cmd_index]);
          goto parse;
        } else {
          write(1, "error: invalid cmd history execution\n", 37);
        }
      } else {
        char index[16] = {0};
        // print history buffer
        for (int i = 0; i < hist_cnt; ++i) {
          sprintf(index, "%d", i);
          write(1, index, strlen(index));
          write(1, ". ", 2);
          write(1, history[i], strlen(history[i]));
          write(1, "\n", 1);
        }
      }
    } else if (strcmp(cmd_list[0][0], "exit") == 0) {
      // manage heap memory & exit
      for (int i = 0; i <= pipe_count; ++i) {
        free(cmd_list[i]);
      }
      free(cmd_list);
      free(stdout_redir_file);
      free(stdin_redir_file);
      for (int i = 0; i < hist_cnt; ++i) {
        free(history[i]);
      }
      exit(0);

    } else if (fd != 1 && cmd_list[0][0][0] == '#' &&
               cmd_list[0][0][1] == '!') {
      // if processing script file, skip shebang line
      goto cleanup;

    } else {
      int prior_fd;
      int pipe_fd[2];
      int orig_stdout = dup(1);  // save stdout for printing errors
      // connect pipes in reverse order
      for (int i = pipe_count; i >= 0; --i) {
        // creates pipes
        pipe(pipe_fd);
        if (fork() == 0) {
          // logic for connecting pipes
          if (pipe_count != 0) {
            if (i == pipe_count) {
              dup2(pipe_fd[0], 0);
            } else if (i == 0) {
              dup2(prior_fd, 1);
              close(pipe_fd[0]);
            } else {
              dup2(pipe_fd[0], 0);
              dup2(prior_fd, 1);
            }
            close(pipe_fd[1]);
          }

          // file redirection logic
          if (stdout_redir == 1 && i == pipe_count) {
            int fd =
                open(stdout_redir_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd == -1) {
              write(1, "error: ", 7);
              write(1, "failed to open ", 15);
              write(1, stdout_redir_file, strlen(stdout_redir_file));
              write(1, "\n", 1);
              exit(-1);
            }
            dup2(fd, 1);
            close(fd);
          }

          if (stderr_redir == 1 && i == pipe_count) {
            int fd =
                open(stderr_redir_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd == -1) {
              write(1, "error: ", 7);
              write(1, "failed to open ", 15);
              write(1, stderr_redir_file, strlen(stderr_redir_file));
              write(1, "\n", 1);
              exit(-1);
            }
            dup2(fd, 2);
            close(fd);
          }

          if (stdin_redir == 1 && i == 0) {
            int fd = open(stdin_redir_file, O_RDONLY, 0644);
            if (fd == -1) {
              write(1, "error: ", 7);
              write(1, "failed to open ", 15);
              write(1, stdin_redir_file, strlen(stdin_redir_file));
              write(1, "\n", 1);
              exit(-1);
            }
            dup2(fd, 0);
            close(fd);
          }

          // execute next command
          execvp(cmd_list[i][0], cmd_list[i]);
          // exec calls do not return upon success, print error otherwise
          dup2(orig_stdout, 1);  // reset stdout to original fd
          write(1, "error: ", 7);
          write(1, cmd_list[i][0], strlen(cmd_list[i][0]));
          write(1, ": command not found\n", 20);
          exit(-1);
        }

        // manage pipes
        if (i != pipe_count) {
          close(prior_fd);
        }
        prior_fd = pipe_fd[1];
        close(pipe_fd[0]);
      }

      // wait on all children
      for (int i = 0; i <= pipe_count; ++i) {
        wait(NULL);
      }
    }

  cleanup:
    // manage heap memory
    for (int i = 0; i <= pipe_count; ++i) {
      free(cmd_list[i]);
    }
    free(cmd_list);
    free(stdout_redir_file);
    free(stderr_redir_file);
    free(stdin_redir_file);

    // if multi command, skip reading input
    // to continue executing next command
    if (multi_command == 1) {
      goto parse;
    }
  }
}

char *tokenize(char *str) {
  static char *s = NULL;
  char *token;

  if (str != NULL) {
    s = str;
  }

  if (s == NULL) {
    return NULL;
  }

  // skip leading spaces
  while (*s == ' ') {
    s++;
  }

  if (*s == '\0') {
    s = NULL;
    return NULL;
  }

  // quoted string
  if (*s == '"') {
    // skip opening quote
    s++;
    token = s;

    while (*s && *s != '"') {
      s++;
    }

    if (*s == '"') {
      // terminate token
      *s = '\0';
      // skip closing quote
      s++;
    }
  } else {
    // normal token
    token = s;

    while (*s && *s != ' ') {
      s++;
    }

    if (*s) {
      *s = '\0';
      s++;
    }
  }

  return token;
}
