#include <string>
#include <vector>
#include <unordered_set>
#include <iostream>
#include <cstring> // std::strtok
#include <climits>
#include <cassert>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>

using namespace std;

#define handle_error(msg)           \
    do {                            \
        perror(msg);                \
        exit(EXIT_FAILURE);         \
    } while (0)

// a simple command, e.g. grep test > tmp (<) Makefile
struct Command {
    vector<string> args;
    Command* next = nullptr; // next command in pipeline
    int infd  = 0;
    int outfd = 1;
    int errfd = 2;
};

// a list of simple commands joined by | or |& (2>&1 |)
// e.g. du --max-depth=1 . -h | sort -h > stat.txt
struct Pipeline {
    Command* first_cmd = nullptr;
    pid_t pgid = -1;
    bool background = false;
};

const unordered_set<string> built_in_cmds = { "exit", "cd", "pwd",
                                              "fg", "bg", "jobs", "history" };

bool is_built_in_command(const string& cmd)
{
    return built_in_cmds.count(cmd) == 1;
}

char cwd[PATH_MAX];

string get_cwd()
{
    if (getcwd(cwd, PATH_MAX) != nullptr)
        return cwd;
    else
        handle_error("getcwd");
}

void cd(const char* path)
{
    if (chdir(path) == -1)
        handle_error("chdir");
    strcpy(cwd, path);
}

void print_prompt()
{
    static bool cwd_is_initialized = false;
    if (!cwd_is_initialized) {
        get_cwd();
        cwd_is_initialized = true;
    }
    cout << cwd << "% ";
    cout.flush();
}


// `getconf ARG_MAX` on my machine gives 2'097'152 (scary)
#define MAX_ARGS 1024
const char* argv[MAX_ARGS];

// spawn a child process to run `cmd`
void run_command(const Command& cmd)
{
    if (cmd.args.size() == 0)
        return;
    assert(cmd.args.size() < MAX_ARGS);
    for (size_t i = 0; i < cmd.args.size(); ++i)
        argv[i] = cmd.args[i].c_str();
    argv[cmd.args.size()] = nullptr;
    pid_t pid = fork();
    if (pid == -1) {
        handle_error("fork");
    }
    else if (pid == 0) { // child
        if (cmd.infd != 0) {  // <, redirect a file to stdin
            dup2(cmd.infd, STDIN_FILENO);
            close(cmd.infd);
        }
        if (cmd.outfd != 1) { // >, redirect stdout to a file
            dup2(cmd.outfd, STDOUT_FILENO);
            close(cmd.outfd);
        }
        if (cmd.errfd != 2) { // 2>, redirect stderr to a file
                              // 2>&1, redirect stderr to stdout (&1, fd 1)
            dup2(cmd.errfd, STDERR_FILENO);
            close(cmd.errfd);
        }
        execvp(argv[0], (char* const*) argv);
        perror("execvp");
        _exit(EXIT_FAILURE);
    }
    // parent, done
}

void run_pipeline(const Pipeline& pipeline)
{
    // e.g. a | b | c | d
    // |& form (e.g. a |& b or a 2>&1 | b) will be preprocessed in the parsing
    // phase by setting errfd = 1 (instead of stderr, 2) which then will be
    // redirected to stdout which will be redirected to some file descriptor.
    int pipefd[2];
    for (Command* cmd = pipeline.first_cmd; cmd; cmd = cmd->next) {
        if (pipe(pipefd) == -1)
            handle_error("pipe");
        if (cmd->next) {
            cmd->outfd = pipefd[1];
            cmd->next->infd = pipefd[0];
        }
        run_command(*cmd);
        // Parent closes write end (& keeps read end, the next child process
        // will fork the file descriptors of parent and read from pipefd[0]).
        // There are a collection of great drawings in Harvard SEAS School's
        // CS61 course site. They clearly demonstrate how piping works in a
        // step-by-step fashion. Please see the subsection "Pipe in a shell"
        // in https://cs61.seas.harvard.edu/site/2021/ProcessControl/.
        close(pipefd[1]);
    }
    close(pipefd[0]);

    int status, wpid;
    if (!pipeline.background)
        // wait for all child processes to finish
        while ((wpid = wait(&status)) > 0);
}

void run_command_list(const vector<Pipeline>& cmd_list)
{
    // a & b | c && d ; e | f &
    (void) cmd_list;
}

void parse_commands(const char* cmd_line, vector<Pipeline>& cmd_list)
{
    (void) cmd_line;
    (void) cmd_list;
}

int main()
{
    print_prompt();
    //string cmd_line;
    //while (getline(cin, cmd_line)) {
    //    vector<Pipeline> cmd_list;
    //    parse_commands(cmd_line.c_str(), cmd_list);
    //    run_command_list(cmd_list);
    //}
    Command cmd_1, cmd_2, cmd_3;
#ifndef TEST2
    // grep pipeline msh.cc | cat | wc > tmp.txt
    cmd_1.args = { "grep", "pipeline", "msh.cc" };
    cmd_2.args = { "cat" };
    cmd_3.args = { "wc" };
    int outfile = open("tmp.txt", O_CREAT | O_WRONLY | O_TRUNC, S_IRUSR | S_IWUSR);
    if (outfile == -1)
        handle_error("open");
    cmd_3.outfd = outfile;
    cmd_1.next = &cmd_2;
    cmd_2.next = &cmd_3;
#else
    // gcc unused.c -Wall -Wextra -o unused |& wc
    cmd_1.args = { "gcc", "unused.c", "-Wall", "-Wextra", "-o", "unused" };
    cmd_2.args = { "wc" };
    cmd_1.next = &cmd_2;
    cmd_1.errfd = 1;
#endif
    Pipeline pipeline;
    pipeline.first_cmd = &cmd_1;
    run_pipeline(pipeline);
#ifndef TEST2
    close(outfile);
#endif
    return 0;
}
