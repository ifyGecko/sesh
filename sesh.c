#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#define MAX_STR 256
#define MAX_TOK 16
#define MAX_HIS 10
#define MAX_JOBS 16

#define JOB_RUNNING 0
#define JOB_STOPPED 1
#define JOB_DONE 2

typedef struct {
  int used;
  int jid;
  pid_t pgid;
  pid_t pids[MAX_TOK];
  int npids;
  int state;
  int notified;
  char *cmd;
} job_t;

static job_t jobs[MAX_JOBS];
static pid_t shell_pgid;
static int shell_interactive;
static int shell_terminal = 0;

char *tokenize(char *);

static int next_jid(void) {
  int max = 0;
  for (int i = 0; i < MAX_JOBS; ++i) {
    if (jobs[i].used && jobs[i].jid > max) max = jobs[i].jid;
  }
  return max + 1;
}

static job_t *job_add(pid_t pgid, pid_t *pids, int npids, const char *cmd) {
  for (int i = 0; i < MAX_JOBS; ++i) {
    if (!jobs[i].used) {
      memset(&jobs[i], 0, sizeof(jobs[i]));
      jobs[i].used = 1;
      jobs[i].jid = next_jid();
      jobs[i].pgid = pgid;
      jobs[i].npids = npids;
      for (int k = 0; k < npids; ++k) jobs[i].pids[k] = pids[k];
      jobs[i].state = JOB_RUNNING;
      jobs[i].notified = 0;
      jobs[i].cmd = cmd ? strdup(cmd) : NULL;
      return &jobs[i];
    }
  }
  return NULL;
}

static job_t *job_find_by_jid(int jid) {
  for (int i = 0; i < MAX_JOBS; ++i) {
    if (jobs[i].used && jobs[i].jid == jid) return &jobs[i];
  }
  return NULL;
}

static job_t *job_find_by_pid(pid_t pid) {
  for (int i = 0; i < MAX_JOBS; ++i) {
    if (!jobs[i].used) continue;
    for (int k = 0; k < jobs[i].npids; ++k) {
      if (jobs[i].pids[k] == pid) return &jobs[i];
    }
  }
  return NULL;
}

static job_t *job_most_recent(void) {
  job_t *best = NULL;
  for (int i = 0; i < MAX_JOBS; ++i) {
    if (!jobs[i].used) continue;
    if (jobs[i].state == JOB_DONE) continue;
    if (!best || jobs[i].jid > best->jid) best = &jobs[i];
  }
  return best;
}

static void job_free(job_t *j) {
  if (j->cmd) free(j->cmd);
  memset(j, 0, sizeof(*j));
}

static int job_all_done(job_t *j) {
  for (int k = 0; k < j->npids; ++k) {
    if (j->pids[k] > 0) return 0;
  }
  return 1;
}

static void job_mark_pid_done(job_t *j, pid_t pid) {
  for (int k = 0; k < j->npids; ++k) {
    if (j->pids[k] == pid) {
      j->pids[k] = -1;
      return;
    }
  }
}

static void reap_children(void) {
  int status;
  pid_t pid;
  while ((pid = waitpid(-1, &status, WNOHANG | WUNTRACED | WCONTINUED)) > 0) {
    job_t *j = job_find_by_pid(pid);
    if (!j) continue;
    if (WIFSTOPPED(status)) {
      j->state = JOB_STOPPED;
      j->notified = 0;
    } else if (WIFCONTINUED(status)) {
      j->state = JOB_RUNNING;
      j->notified = 0;
    } else if (WIFEXITED(status) || WIFSIGNALED(status)) {
      job_mark_pid_done(j, pid);
      if (job_all_done(j)) {
        j->state = JOB_DONE;
        j->notified = 0;
      }
    }
  }
}

static void sigchld_handler(int sig) {
  (void)sig;
  int saved_errno = errno;
  reap_children();
  errno = saved_errno;
}

static void block_sigchld(sigset_t *old) {
  sigset_t m;
  sigemptyset(&m);
  sigaddset(&m, SIGCHLD);
  sigprocmask(SIG_BLOCK, &m, old);
}

static void restore_sigmask(sigset_t *old) {
  sigprocmask(SIG_SETMASK, old, NULL);
}

static void print_job_status(job_t *j) {
  char buf[64];
  int n;
  const char *st = j->state == JOB_RUNNING   ? "Running"
                   : j->state == JOB_STOPPED ? "Stopped"
                                             : "Done";
  n = snprintf(buf, sizeof(buf), "[%d] %s\t", j->jid, st);
  write(1, buf, n);
  if (j->cmd) write(1, j->cmd, strlen(j->cmd));
  write(1, "\n", 1);
}

static void notify_completed_jobs(void) {
  sigset_t old;
  block_sigchld(&old);
  for (int i = 0; i < MAX_JOBS; ++i) {
    if (jobs[i].used && !jobs[i].notified &&
        (jobs[i].state == JOB_DONE || jobs[i].state == JOB_STOPPED)) {
      print_job_status(&jobs[i]);
      jobs[i].notified = 1;
      if (jobs[i].state == JOB_DONE) job_free(&jobs[i]);
    }
  }
  restore_sigmask(&old);
}

static int wait_for_foreground(job_t *j, pid_t last_pid) {
  int last_status = 0;
  sigset_t old;
  block_sigchld(&old);
  while (1) {
    int status;
    pid_t pid = waitpid(-1, &status, WUNTRACED);
    if (pid < 0) {
      if (errno == EINTR) continue;
      break;
    }
    job_t *jp = job_find_by_pid(pid);
    if (!jp) continue;
    if (WIFSTOPPED(status)) {
      jp->state = JOB_STOPPED;
      jp->notified = 0;
      if (jp == j) {
        char buf[64];
        int n = snprintf(buf, sizeof(buf), "\n[%d] Stopped\t", jp->jid);
        write(1, buf, n);
        if (jp->cmd) write(1, jp->cmd, strlen(jp->cmd));
        write(1, "\n", 1);
        jp->notified = 1;
        break;
      }
    } else if (WIFEXITED(status) || WIFSIGNALED(status)) {
      if (pid == last_pid) last_status = status;
      job_mark_pid_done(jp, pid);
      if (job_all_done(jp)) {
        jp->state = JOB_DONE;
        jp->notified = 1;
        if (jp == j) {
          job_free(jp);
          break;
        }
      }
    }
  }
  restore_sigmask(&old);
  return last_status;
}

int main(int argc, char **argv) {
  int fd = 1;
  char str[MAX_STR] = {0};
  char input_line[MAX_STR] = {0};
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

  shell_interactive = (fd == 1) && isatty(shell_terminal);

  {
    struct sigaction sa;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sa.sa_handler = sigchld_handler;
    sigaction(SIGCHLD, &sa, NULL);
  }

  if (shell_interactive) {
    // wait until shell is in the foreground process group
    while (tcgetpgrp(shell_terminal) != (shell_pgid = getpgrp())) {
      kill(-shell_pgid, SIGTTIN);
    }

    struct sigaction sa;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sa.sa_handler = SIG_IGN;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGQUIT, &sa, NULL);
    sigaction(SIGTSTP, &sa, NULL);
    sigaction(SIGTTIN, &sa, NULL);
    sigaction(SIGTTOU, &sa, NULL);

    // put the shell in its own pgrp and grab the terminal
    shell_pgid = getpid();
    if (setpgid(shell_pgid, shell_pgid) < 0 && errno != EPERM) {
      write(1, "error: setpgid failed\n", 22);
      exit(-1);
    }
    tcsetpgrp(shell_terminal, shell_pgid);
  } else {
    // preserve original non-interactive behavior of ignoring SIGINT in shell
    struct sigaction sa;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sa.sa_handler = SIG_IGN;
    sigaction(SIGINT, &sa, NULL);
  }

  while (1) {
    // terminal prompt
  loop:

    if (shell_interactive) notify_completed_jobs();

    // init prompt string
    memset(str, 0, sizeof(str));
    if (user_name == NULL) {
      strncpy(str, "unknown", MAX_STR);
    } else {
      strncpy(str, user_name, MAX_STR);
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
        if (bytes_read == -1 && errno == EINTR) {
          --i;
          continue;
        }
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

    // snapshot the input before history/tokenize mutations
    strncpy(input_line, str, MAX_STR);
    input_line[MAX_STR - 1] = '\0';

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
    int exit_code = 0;
    int pipe_count = 0;
    int stdin_redir = 0;
    int stdout_redir = 0;
    int stderr_redir = 0;
    int background = 0;
    char ***cmd_list =
        (char ***)calloc(MAX_TOK, sizeof(char **));
    cmd_list[pipe_count] =
        (char **)calloc(MAX_TOK, sizeof(char *));
    char *stdin_redir_file =
        (char *)calloc(MAX_STR, sizeof(char));
    char *stdout_redir_file =
        (char *)calloc(MAX_STR, sizeof(char));
    char *stderr_redir_file =
        (char *)calloc(MAX_STR, sizeof(char));
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
      } else if (strcmp(token, "&") == 0) {
        background = 1;
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
    if (strcmp(cmd_list[0][0], "help") == 0 ||
        strcmp(cmd_list[0][0], "?") == 0) {
      write(1, "0.  cd <path> - change directory\n", 33);
      write(1, "1.  setenv <VAR> <VAL> - set environment variable\n", 50);
      write(1, "2.  unsetenv <VAR> - unset environment variable\n", 48);
      write(1, "3.  history - list input command history\n", 42);
      write(1, "4.  history clean - remove all commands from history\n", 54);
      write(1,
            "5.  history exec <n> - execute the nth command from this list\n",
            63);
      write(1, "6.  exit - terminate sesh instance\n", 35);
      write(1, "7.  $? - get exit code of last cmd ran\n", 39);
      write(1, "8.  cmd & - run command in background\n", 38);
      write(1, "9.  jobs - list active jobs\n", 28);
      write(1, "10. fg [%n] - bring job to foreground\n", 38);
      write(1, "11. bg [%n] - resume stopped job in background\n", 47);
      write(1, "12. kill %n - send SIGTERM to job\n", 34);
    } else if (strcmp(cmd_list[0][0], "cd") == 0) {
      chdir(cmd_list[0][1]);
    } else if (strcmp(cmd_list[0][0], "setenv") == 0) {
      setenv(cmd_list[0][1], cmd_list[0][2], 1);
    } else if (strcmp(cmd_list[0][0], "unsetenv") == 0) {
      unsetenv(cmd_list[0][1]);
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
          strncpy(input_line, str, MAX_STR);
          input_line[MAX_STR - 1] = '\0';
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

    } else if (strcmp(cmd_list[0][0], "jobs") == 0) {
      sigset_t old;
      block_sigchld(&old);
      for (int k = 0; k < MAX_JOBS; ++k) {
        if (jobs[k].used) {
          print_job_status(&jobs[k]);
          jobs[k].notified = 1;
          if (jobs[k].state == JOB_DONE) job_free(&jobs[k]);
        }
      }
      restore_sigmask(&old);

    } else if (strcmp(cmd_list[0][0], "fg") == 0) {
      sigset_t old;
      block_sigchld(&old);
      job_t *j = NULL;
      if (cmd_list[0][1] && cmd_list[0][1][0] == '%') {
        j = job_find_by_jid(atoi(cmd_list[0][1] + 1));
      } else {
        j = job_most_recent();
      }
      if (!j) {
        restore_sigmask(&old);
        write(1, "error: no such job\n", 19);
      } else {
        pid_t target_pgid = j->pgid;
        pid_t last_pid = j->pids[j->npids - 1];
        if (j->cmd) {
          write(1, j->cmd, strlen(j->cmd));
          write(1, "\n", 1);
        }
        if (shell_interactive) tcsetpgrp(shell_terminal, target_pgid);
        if (j->state == JOB_STOPPED) {
          j->state = JOB_RUNNING;
          j->notified = 0;
          kill(-target_pgid, SIGCONT);
        }
        int status = wait_for_foreground(j, last_pid);
        if (shell_interactive) tcsetpgrp(shell_terminal, shell_pgid);
        restore_sigmask(&old);
        if (WIFEXITED(status)) {
          exit_code = WEXITSTATUS(status);
        }
      }

    } else if (strcmp(cmd_list[0][0], "bg") == 0) {
      sigset_t old;
      block_sigchld(&old);
      job_t *j = NULL;
      if (cmd_list[0][1] && cmd_list[0][1][0] == '%') {
        j = job_find_by_jid(atoi(cmd_list[0][1] + 1));
      } else {
        // most recent stopped
        for (int k = 0; k < MAX_JOBS; ++k) {
          if (jobs[k].used && jobs[k].state == JOB_STOPPED) {
            if (!j || jobs[k].jid > j->jid) j = &jobs[k];
          }
        }
      }
      if (!j) {
        write(1, "error: no such job\n", 19);
      } else {
        char buf[64];
        int n = snprintf(buf, sizeof(buf), "[%d] ", j->jid);
        write(1, buf, n);
        if (j->cmd) write(1, j->cmd, strlen(j->cmd));
        write(1, " &\n", 3);
        if (j->state == JOB_STOPPED) {
          kill(-j->pgid, SIGCONT);
        }
        j->state = JOB_RUNNING;
        j->notified = 0;
      }
      restore_sigmask(&old);

    } else if (strcmp(cmd_list[0][0], "kill") == 0 && cmd_list[0][1] &&
               cmd_list[0][1][0] == '%') {
      sigset_t old;
      block_sigchld(&old);
      job_t *j = job_find_by_jid(atoi(cmd_list[0][1] + 1));
      if (!j) {
        write(1, "error: no such job\n", 19);
      } else {
        kill(-j->pgid, SIGTERM);
      }
      restore_sigmask(&old);

    } else if (strcmp(cmd_list[0][0], "exit") == 0) {
      // manage heap memory & exit
      for (int i = 0; i <= pipe_count; ++i) {
        free(cmd_list[i]);
      }
      free(cmd_list);
      free(stdout_redir_file);
      free(stdin_redir_file);
      free(stderr_redir_file);

      for (int i = 0; i < hist_cnt; ++i) {
        free(history[i]);
      }

      for (int i = 0; i < MAX_JOBS; ++i) {
        if (jobs[i].used) job_free(&jobs[i]);
      }

      exit(0);

    } else if (fd != 1 && cmd_list[0][0][0] == '#' &&
               cmd_list[0][0][1] == '!') {
      // if processing script file, skip shebang line
      goto cleanup;
    } else if (strcmp(cmd_list[0][0], "$?") == 0) {
      char ret_str[MAX_STR];
      int len = sprintf(ret_str, "%d\n", exit_code);
      write(1, ret_str, len);
    } else {
      int prior_fd = -1;
      int pipe_fd[2];
      int orig_stdout = dup(1);  // save stdout for printing errors
      pid_t *pids = calloc(pipe_count + 1, sizeof(pid_t));
      pid_t pipeline_pgid = 0;

      sigset_t saved_mask;
      block_sigchld(&saved_mask);

      // connect pipes in reverse order
      for (int i = pipe_count; i >= 0; --i) {
        // creates pipes
        pipe(pipe_fd);

        pid_t cur_pid = fork();
        if (cur_pid == 0) {
          // child
          pid_t my_pid = getpid();
          if (shell_interactive) {
            if (pipeline_pgid == 0) pipeline_pgid = my_pid;
            setpgid(my_pid, pipeline_pgid);
            if (!background && i == pipe_count) {
              tcsetpgrp(shell_terminal, pipeline_pgid);
            }
            struct sigaction sa;
            sigemptyset(&sa.sa_mask);
            sa.sa_flags = 0;
            sa.sa_handler = SIG_DFL;
            sigaction(SIGINT, &sa, NULL);
            sigaction(SIGQUIT, &sa, NULL);
            sigaction(SIGTSTP, &sa, NULL);
            sigaction(SIGTTIN, &sa, NULL);
            sigaction(SIGTTOU, &sa, NULL);
            sigaction(SIGCHLD, &sa, NULL);
          } else {
            signal(SIGINT, SIG_DFL);
            signal(SIGCHLD, SIG_DFL);
          }
          sigset_t empty;
          sigemptyset(&empty);
          sigprocmask(SIG_SETMASK, &empty, NULL);

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
            int rfd =
                open(stdout_redir_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);

            if (rfd == -1) {
              write(1, "error: ", 7);
              write(1, "failed to open ", 15);
              write(1, stdout_redir_file, strlen(stdout_redir_file));
              write(1, "\n", 1);
              exit(-1);
            }

            dup2(rfd, 1);
            close(rfd);
          }

          if (stderr_redir == 1 && i == pipe_count) {
            int rfd =
                open(stderr_redir_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (rfd == -1) {
              write(1, "error: ", 7);
              write(1, "failed to open ", 15);
              write(1, stderr_redir_file, strlen(stderr_redir_file));
              write(1, "\n", 1);
              exit(-1);
            }

            dup2(rfd, 2);
            close(rfd);
          }

          if (stdin_redir == 1 && i == 0) {
            int rfd = open(stdin_redir_file, O_RDONLY, 0644);
            if (rfd == -1) {
              write(1, "error: ", 7);
              write(1, "failed to open ", 15);
              write(1, stdin_redir_file, strlen(stdin_redir_file));
              write(1, "\n", 1);
              exit(-1);
            }

            dup2(rfd, 0);
            close(rfd);
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

        // parent
        pids[i] = cur_pid;
        if (shell_interactive) {
          if (pipeline_pgid == 0) pipeline_pgid = cur_pid;
          setpgid(cur_pid, pipeline_pgid);
        }

        // manage pipes
        if (i != pipe_count) {
          close(prior_fd);
        }

        prior_fd = pipe_fd[1];
        close(pipe_fd[0]);
      }

      const char *cmd_label = input_line[0] ? input_line : "(unknown)";
      job_t *j = job_add(pipeline_pgid ? pipeline_pgid : pids[pipe_count], pids,
                         pipe_count + 1, cmd_label);

      if (background) {
        if (j) {
          char buf[64];
          int n = snprintf(buf, sizeof(buf), "[%d] %d\n", j->jid,
                           (int)(pipeline_pgid ? pipeline_pgid : pids[pipe_count]));
          write(1, buf, n);
        }
        restore_sigmask(&saved_mask);
      } else {
        pid_t last_pid = pids[pipe_count];
        if (shell_interactive && pipeline_pgid) {
          tcsetpgrp(shell_terminal, pipeline_pgid);
        }

        int status = wait_for_foreground(j, last_pid);

        if (shell_interactive) {
          tcsetpgrp(shell_terminal, shell_pgid);
        }
        restore_sigmask(&saved_mask);

        if (WIFEXITED(status)) {
          exit_code = WEXITSTATUS(status);
        }
      }

      free(pids);
      close(orig_stdout);
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

  } else if (*s == '$' && *(s + 1) != '?') {
    // env var expansion
    // skip $
    s++;

    // pointer ref to substring
    char *sub = s;

    // iterate over remainder
    while (*s && *s != ' ') {
      s++;
    }

    // null terminate
    if (*s == ' ') {
      *s = '\0';
    }

    token = getenv(sub);

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
