// using sh61's parsing facility, etc. to make life easier
#include "sh61.hh"
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>
#include <utility>
#include <map>
#include <iostream>
#include <cstring>
#include <climits>
#include <cassert>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <pwd.h>

using namespace std;

#define handle_error(msg)           \
    do {                            \
        perror(msg);                \
        exit(EXIT_FAILURE);         \
    } while (0)

#define REDIRECT_OP_INPUT   0   // <
#define REDIRECT_OP_OUTPUT  1   // >
#define REDIRECT_OP_ERROUT  2   // 2>
#define REDIRECT_OP_APPEND  3   // >>
#define REDIRECT_OP_ERRAPP  4   // 2>>

// a simple command, e.g. grep test > tmp (<) Makefile
struct Command {
    string str; // the command string
    vector<string> args;
    vector<pair<int, string>> redirections;
    Command* next = nullptr; // next command in pipeline
    int infd  = 0;
    int outfd = 1;
    int errfd = 2;
    pid_t pid = -1;
    int op = TYPE_SEQUENCE;  // operator type the cmd ends with
    int status = 0;          // return status
    // !completed && !stopped => running
    bool completed = false;
    bool stopped = false;
};

// a list of simple commands joined by | and/or |& (2>&1 |)
// e.g. du --max-depth=1 . -h | sort -h > stat.txt
// The exit status of a pipeline is the exit status of the last command
// in the pipeline. See also
// https://www.gnu.org/software/bash/manual/html_node/Pipelines.html.
struct Pipeline {
    string str; // the pipeline string, used for messages
    Command* first_cmd = nullptr;
    Pipeline* next = nullptr;
    pid_t pgid = -1;
    bool notified = false;
    bool foreground = true;
    bool next_is_or = false; // || or && (valid when `last == false`)
    bool last = false;       // is pipeline last one in a conditional
    // In our implementation, all the pipelines are linked together in
    // a singly linked list, thus field `last` is required. Otherwise,
    // it's not (can be told from whether `next == nullptr`).
};

typedef Pipeline job;

job* current_foreground_job = nullptr;
int last_job_exit_status = 0;
map<pid_t, job*> job_list;
map<int, job*> stopped_jobs;

#if 1 // for completeness, though we won't use them here

// a chain of pipelines linked by && and/or ||
// e.g. echo foo bar | wc && sleep 5 || echo baz
// The return status of AND and OR lists is the exit status of the last
// command executed in the list.
struct Conditional {
    Pipeline* first_pipeline = nullptr;
    Conditional* next = nullptr;
    bool background = false; // ; or &
};

// a list of conditionals separated by ; and/or &
// e.g. echo foo & echo bar | wc && echo baz ; sleep 7 || cat nonexist &
struct List {
    Conditional* first_cond;
};

#endif

// is the conditional chain starting at `cmd` run in the background
bool chain_in_background(Command* cmd)
{
    while (cmd->op != TYPE_SEQUENCE && cmd->op != TYPE_BACKGROUND)
        cmd = cmd->next;
    return cmd->op == TYPE_BACKGROUND;
}

char cwd[PATH_MAX];

string get_cwd()
{
    if (getcwd(cwd, PATH_MAX) != nullptr)
        return cwd;
    else
        handle_error("getcwd");
}

string get_homedir()
{
    static const string homedir = getpwuid(getuid())->pw_dir;
    return homedir;
}

string expand_tilde(const string& path)
{
    if (path[0] == '~')
        return get_homedir() + &path[1];
    return path;
}

void cd(const string& path)
{
    if (chdir(expand_tilde(path).c_str()) == -1)
        perror("chdir");
    get_cwd();
}

void pwd()
{
    cout << cwd << endl;
}

#define GREEN "\033[32m"
#define BLUE  "\033[34m"
#define END   "\033[0m"
#define COLORED_TEXT(text, color) color << text << END

void print_prompt()
{
    static const string homedir = get_homedir();
    static const string tilde = "~";
    static char hostname[HOST_NAME_MAX];
    static char username[LOGIN_NAME_MAX];
    static bool initialized = false;
    if (!initialized) {
        get_cwd();
        gethostname(hostname, HOST_NAME_MAX);
        getlogin_r(username, LOGIN_NAME_MAX);
        initialized = true;
    }
    string usr_at_host = string(username) + '@' + hostname;
    cout << COLORED_TEXT(usr_at_host, GREEN) << ':';
    string path = cwd;
    // homedir '/home/usrname' is a prefix of cwd
    if (path.find(homedir) == 0)
        path = tilde + path.substr(homedir.size());
    cout << COLORED_TEXT(path, BLUE) << "% ";
    cout.flush();
}

void release_list_of_cmds(Command* cmd)
{
    Command* del;
    while (cmd) {
        del = cmd;
        cmd = cmd->next;
        delete del;
    }
}

// release resources (list of jobs) created in `parse_line`
void release_list_of_jobs(job* j)
{
    job* del;
    while (j) {
        del = j;
        j = j->next;
        release_list_of_cmds(del->first_cmd);
        delete del;
    }
}

void msh_error(const char* msg)
{
    cerr << "msh: " << msg << endl;
}

// parse the command line into a list of jobs
// returns nullptr if `s` is empty (only spaces)
// or on error (redirection/pipeline error)
job* parse_line(const char* s)
{
    shell_parser parser(s);
    Command* chead = nullptr;
    Command* clast = nullptr;
    Command* ccur  = nullptr;
    string op;
    for (auto it = parser.begin(); it != parser.end(); ++it) {
        switch (it.type()) {
        case TYPE_NORMAL:
            // add a new argument to the current command
            // might require creating a new command
            if (!ccur) {
                ccur = new Command;
                if (clast)
                    clast->next = ccur;
                else
                    chead = ccur;
            }
            ccur->args.push_back(expand_tilde(it.str()));
            if (ccur->str.size() != 0)
                ccur->str += " ";
            ccur->str += it.str();
            break;
        case TYPE_REDIRECT_OP:
            op = it.str();
            ccur->str += ' ' + op;
            if (op == "2>&1") {
                // redirect stderr to stdout
                ccur->errfd = STDOUT_FILENO;
                break;
            }
            ++it;
            if (op == "<") {
                if (it.type() != TYPE_NORMAL) {
                    msh_error("expected an input file after <");
                    release_list_of_cmds(chead);
                    return nullptr;
                }
                ccur->redirections.emplace_back(REDIRECT_OP_INPUT, it.str());
            }
            if (op == ">" || op == "&>" || op == ">&") {
                if (it.type() != TYPE_NORMAL) {
                    string msg = "expected an output file after " + op;
                    msh_error(msg.c_str());
                    release_list_of_cmds(chead);
                    return nullptr;
                }
                // > outfile 2>&1  <==>  &> outfile
                // the left op is preferred
                if (op == "&>" || op == ">&") // redirect stderr to stdout
                    ccur->errfd = STDOUT_FILENO;
                ccur->redirections.emplace_back(REDIRECT_OP_OUTPUT, it.str());
            }
            else if (op == "2>") {
                if (it.type() != TYPE_NORMAL) {
                    msh_error("expected an output file for stderr after 2>");
                    release_list_of_cmds(chead);
                    return nullptr;
                }
                ccur->redirections.emplace_back(REDIRECT_OP_ERROUT, it.str());
            }
            // >> logfile 2>&1  <==>  &>> logfile
            if (op == ">>" || op == "&>>" || op == ">>&") {
                if (it.type() != TYPE_NORMAL) {
                    string msg = "expected an output file for appending after " + op;
                    msh_error(msg.c_str());
                    release_list_of_cmds(chead);
                    return nullptr;
                }
                if (op == "&>>" || op == ">>&")
                    ccur->errfd = STDOUT_FILENO;
                ccur->redirections.emplace_back(REDIRECT_OP_APPEND, it.str());
            }
            else if (op == "2>>") {
                if (it.type() != TYPE_NORMAL) {
                    msh_error("expected an output file for appending stderr after 2>>");
                    release_list_of_cmds(chead);
                    return nullptr;
                }
                ccur->redirections.emplace_back(REDIRECT_OP_ERRAPP, it.str());
            }
            ccur->str += ' ' + it.str();
            break;
        case TYPE_SEQUENCE:
        case TYPE_BACKGROUND:
        case TYPE_PIPE:
        case TYPE_AND:
        case TYPE_OR:
            // these operators terminate the current command
            if (!ccur) {
                string msg = "syntax error near unexpected token " + it.str();
                msh_error(msg.c_str());
                release_list_of_cmds(chead);
                return nullptr;
            }
            if (it.type() == TYPE_PIPE && it.str() == "|&")
                ccur->errfd = STDOUT_FILENO;
            clast = ccur;
            clast->op = it.type();
            ccur = nullptr;
            break;
        }
    }
    // now link these commands into pipelines (jobs)
    job* jhead = nullptr;
    job* jcur  = nullptr;
    if (chead) {
        jcur = jhead = new Pipeline;
        jcur->first_cmd = chead;
        jcur->str = chead->str;
    }
    bool first_cmd_in_pipeline = true;
    Command* prev_pipeline_last_cmd = nullptr;
    for (Command* cmd = chead; cmd; cmd = cmd->next) {
        if (cmd->op == TYPE_PIPE) {
            jcur->str += " | " + cmd->next->str;
            if (first_cmd_in_pipeline) {
                jcur->first_cmd = cmd;
                first_cmd_in_pipeline = false;
                if (prev_pipeline_last_cmd)
                    prev_pipeline_last_cmd->next = nullptr;
            }
        }
        else { // might need to create a new pipeline
            if (cmd->next) {
                jcur->next = new Pipeline;
                jcur->next->str = cmd->next->str;
                first_cmd_in_pipeline = true;
                prev_pipeline_last_cmd = cmd;
                if (cmd->op == TYPE_OR || cmd->op == TYPE_AND) {
                    jcur->foreground = !chain_in_background(cmd);
                    if (cmd->op == TYPE_OR)
                        jcur->next_is_or = true;
                }
                else {
                    jcur->last = true;
                    if (cmd->op == TYPE_BACKGROUND)
                        jcur->foreground = false;
                }
                jcur = jcur->next;
            }
            else { // last cmd in last pipeline
                //jcur->next = nullptr;
                jcur->last = true;
                if (cmd->op == TYPE_BACKGROUND)
                    jcur->foreground = false;
            }
        }
    }
    return jhead;
}

// `getconf ARG_MAX` on my machine gives 2'097'152 (scary)
#define MAX_ARGS 32767 // 2^15 - 1

// spawn a child process to run `cmd`
pid_t run_command(Command* cmd, pid_t pgid)
{
    static const char* argv[MAX_ARGS];
    assert(cmd->args.size() > 0 && cmd->args.size() < MAX_ARGS);

    const string& cmd_name = cmd->args[0];
    if (cmd_name == "exit") {
        // if has any stopped/running jobs, terminate them
        // ... to kill unfinished jobs
        if (cmd->args.size() == 1)
            exit(EXIT_SUCCESS);
        else {
            int exit_code = 1;
            try {
                exit_code = stoi(cmd->args[1]);
            }
            catch(const std::invalid_argument& e) {
                msh_error("numeric argument required");
                exit(EXIT_FAILURE);
            }
            catch(const std::out_of_range& e) {
                msh_error("exit number out of range");
                exit(EXIT_FAILURE);
            }
            catch (...) {
                exit(EXIT_FAILURE);
            }
            exit(exit_code);
        }
    }
    // this is the only command the shell doesn't fork to run
    if (cmd_name == "cd") {
        string path = cmd->args.size() == 1 ? "~" : cmd->args[1];
        cd(path);
        return cmd->pid = 0; // represents parent (no forking)
    }

    for (size_t i = 0; i < cmd->args.size(); ++i)
        argv[i] = cmd->args[i].c_str();
    argv[cmd->args.size()] = nullptr;
    // optional, make ls print with color by default
    if (cmd_name == "ls") {
        argv[cmd->args.size()] = "--color";
        argv[cmd->args.size() + 1] = nullptr;
    }

    pid_t pid = fork();
    if (pid == -1) {
        handle_error("fork");
    }
    else if (pid == 0) { // child
        cmd->pid = getpid();
        if (pgid == -1) // not set yet
            pgid = cmd->pid;
        setpgid(cmd->pid, pgid);

        // open redirection files if any
        for (const auto& x : cmd->redirections) {
            // O_CLOEXEC flag permits a program to avoid additional
            // fcntl(2) F_SETFD operations to set the FD_CLOEXEC flag.
            int flags = O_CLOEXEC;
            if (x.first == REDIRECT_OP_INPUT)
                flags |= O_RDONLY;
            else if (x.first == REDIRECT_OP_OUTPUT || x.first == REDIRECT_OP_ERROUT)
                flags |= O_CREAT | O_WRONLY | O_TRUNC;
            else if (x.first == REDIRECT_OP_APPEND || x.first == REDIRECT_OP_ERRAPP)
                flags |= O_CREAT | O_APPEND | O_WRONLY;

            int fd = open(x.second.c_str(), flags, /* mode: rw-r--r-- */0644);
            if (fd == -1)
                handle_error("open");

            // redirect
            if (x.first == REDIRECT_OP_INPUT)
                cmd->infd = fd;
            else if (x.first == REDIRECT_OP_OUTPUT || x.first == REDIRECT_OP_APPEND)
                cmd->outfd = fd;
            else if (x.first == REDIRECT_OP_ERROUT || x.first == REDIRECT_OP_ERRAPP)
                cmd->errfd = fd;
        }

        // set up redirections if any
        if (cmd->infd != 0) {  // <, redirect a file to stdin
            dup2(cmd->infd, STDIN_FILENO);
            close(cmd->infd);
        }
        if (cmd->outfd != 1) { // >, redirect stdout to a file
            dup2(cmd->outfd, STDOUT_FILENO);
            close(cmd->outfd);
        }
        if (cmd->errfd != 2) { // 2>, redirect stderr to a file
                               // 2>&1, redirect stderr to stdout (&1, fd 1)
            dup2(cmd->errfd, STDERR_FILENO);
            if (cmd->errfd != 1)
                close(cmd->errfd);
        }

        // run a built-in command or execvp one
        if (cmd_name == "pwd") {
            pwd();
        }
        else if (cmd_name == "jobs") {
        }
        else if (cmd_name == "bg") {
        }
        else if (cmd_name == "bg") {
        }
        else if (cmd_name == "history") {
        }
        else { // external commands
            execvp(argv[0], (char* const*) argv);
            perror("execvp");
            _exit(EXIT_FAILURE);
        }
        _exit(EXIT_SUCCESS);
    }
    // parent returns child process pid
    return cmd->pid = pid;
}

void run_pipeline(Pipeline* pipeline)
{
    // e.g. a | b | c | d
    // |& form (e.g. a |& b or a 2>&1 | b) will be preprocessed in the parsing
    // phase by setting errfd = 1 (instead of stderr, 2) which then will be
    // redirected to stdout which will be redirected to some file descriptor.
    int pipefd[2]{-1, -1}, prev_pipe_read_end = 0;
    for (Command* cmd = pipeline->first_cmd; cmd; cmd = cmd->next) {
        // pipe2 with O_CLOEXEC flag can close the file descriptors created by
        // pipe2 automatically in the child processes when they call execvp
        // (although closing them isn't an error). Using it enables us to only
        // focus on the file descriptors created in the parent process.
        if (pipe2(pipefd, O_CLOEXEC) == -1)
            handle_error("pipe2");
        if (cmd->next) {
            cmd->outfd = pipefd[1];
            cmd->next->infd = pipefd[0];
        }
        run_command(cmd, pipeline->pgid);
        // add all child processes in pipeline to the same process group
        /* Note that here are race conditions (child processes may exit or
         * become zombies before parent setpgid).
         * """
         * In order to avoid some race conditions, you should call setpgid in
         * the parent and in each of the children. Why does the parent need to
         * call it? Because it needs to ensure the process group exists before
         * it advances on to add other processes in the pipeline to the same
         * group. Why do child processes need to call it? Because if the child
         * relies on the parent to do it, the child may execvp (and invalidate
         * its own pid as a valid setpgid argument) before the parent gets
         * around to it.
         * """ (quoted from the Stanford Shell "Tips and Tidbits" section)
         * https://web.stanford.edu/class/cs110/summer-2021/assignments/assign4-stanford-shell/
         */
        if (pipeline->pgid == -1 && cmd->pid != 0) // ignore leading `cd`
            pipeline->pgid = cmd->pid;
        if (cmd->pid != 0)
            setpgid(cmd->pid, pipeline->pgid);
        // Draw pictures!
        // Parent closes current pipe's write end & previous pipe's read end.
        // There are a collection of great drawings in Harvard SEAS School's
        // CS61 course site. They clearly demonstrate how piping works in a
        // step-by-step fashion. Please see the subsection "Pipe in a shell"
        // in https://cs61.seas.harvard.edu/site/2021/ProcessControl/.
        close(pipefd[1]);
        if (prev_pipe_read_end != 0)
            close(prev_pipe_read_end);
        prev_pipe_read_end = pipefd[0];
    }
    if (pipefd[0] != -1)
        close(pipefd[0]); // close last pipe read end

    if (pipeline->pgid == -1) { // job with only `cd` command
        last_job_exit_status = 0;
        return;
    }
    if (pipeline->foreground) {
        current_foreground_job = pipeline;
        int wstatus;
        while (waitpid(-pipeline->pgid, &wstatus, WUNTRACED | WCONTINUED) > 0);
#if 0
        int w = 1;
        while (w > 0) {
            do {
                w = waitpid(-pipeline->pgid, &pipeline->wstatus, WUNTRACED | WCONTINUED);
                if (w == -1 && errno != ECHILD)
                    handle_error("waitpid");

                if (WIFEXITED(pipeline->wstatus))
                    /* normal exit */
                    last_job_exit_status = WEXITSTATUS(pipeline->wstatus);
                else if (WIFSIGNALED(pipeline->wstatus))
                    /* type Ctrl+C while the shell is executing a foreground pipeline */
                    last_job_exit_status = WTERMSIG(pipeline->wstatus);
                else if (WIFSTOPPED(pipeline->wstatus))
                    /* type Ctrl+Z while the shell is executing a foreground pipeline */
                    last_job_exit_status = WSTOPSIG(pipeline->wstatus);
                else if (WIFCONTINUED(pipeline->wstatus))
                    /* `fg` sends SIGCONT signal to the stopped process */
                    (void) 1;
            } while (!WIFEXITED(pipeline->wstatus) && !WIFSIGNALED(pipeline->wstatus));
        }
#endif
    }
    else { // always return success (0) for background jobs
        last_job_exit_status = 0;
    }
}

void run_list_of_jobs(job* j)
{
    while (j) {
        run_pipeline(j);
        if (!j->last) { // not the last pipeline in a conditional
            if (j->next_is_or) {
                //if (last_job_exit_status);
            }
            else { // &&

            }
        }
        j = j->next;
    }
}

void sigint_handler(int sig)
{
    (void) sig;
    if (current_foreground_job) {
        kill(- current_foreground_job->pgid, SIGKILL);
        // discard the remaining jobs also
        release_list_of_jobs(current_foreground_job);
        current_foreground_job->next = nullptr;
    }
}

int main(int argc, char* argv[])
{
    // install SIGINT handler
    signal(SIGINT, sigint_handler);

    FILE* command_file = stdin;
    // check for filename option: read commands from file
    if (argc > 1) {
        command_file = fopen(argv[1], "rb");
        if (!command_file)
            handle_error(argv[1]);
    }

    // - Put the shell into the foreground
    // - Ignore the SIGTTOU signal, which is sent when the shell is put back
    //   into the foreground
    //claim_foreground(0);
    set_signal_handler(SIGTTOU, SIG_IGN);

    char buf[BUFSIZ];
    int bufpos = 0;
    while (!feof(command_file)) {
        print_prompt();

        // read a string, checking for error or EOF
        if (fgets(&buf[bufpos], BUFSIZ - bufpos, command_file) == nullptr) {
            if (ferror(command_file) && errno == EINTR) {
                // ignore EINTR errors
                clearerr(command_file);
                buf[bufpos] = 0;
            } else {
                if (ferror(command_file))
                    perror("ferror");
                break;
            }
        }

        // if a complete command line has been provided, run it
        bufpos = strlen(buf);
        if (bufpos == BUFSIZ - 1 || (bufpos > 0 && buf[bufpos - 1] == '\n')) {
            if (job* j = parse_line(buf)) {
                run_list_of_jobs(j);
                release_list_of_jobs(j);
            }
            bufpos = 0;
        }

        // handle zombie processes and/or interrupt requests
    }

    return 0;
}
