// using sh61's parsing facility & claim_foreground to make life easier
#include "sh61.hh"
#include <string>
#include <vector>
#include <utility>
#include <map>
#include <iostream>
#include <iomanip>
#include <cstring>
#include <climits>
#include <cassert>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
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
#define REDIRECT_OP_ERR2OUT 5   // 2>&1

#define RUNNING     0
#define STOPPED     1
#define COMPLETED   2
#define TERMINATED  3  // signaled, i.e. terminated by some signal

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
    int state = RUNNING;
};

// a list of simple commands joined by | and/or |& (2>&1 |)
// e.g. du --max-depth=1 . -h | sort -h > stat.txt
// The exit status of a pipeline is the exit status of the last command
// in the pipeline. See also
// https://www.gnu.org/software/bash/manual/html_node/Pipelines.html.
struct Pipeline {
    string str; // the pipeline string, used for messages
    struct termios tmodes; // each job has their own terminal modes
    Command* first_cmd = nullptr;
    Pipeline* next = nullptr;
    pid_t pgid = -1;
    int state = RUNNING;
    bool notified = true;    // only terminated jobs set it false
    bool foreground = true;
    bool next_is_or = false; // || or && (valid when `last == false`)
    bool last = false;       // is pipeline last one in a conditional
    // In our implementation, all the pipelines are linked together in
    // a singly linked list, thus field `last` is required. Otherwise,
    // it's not (can be told from whether `next == nullptr`).
};

typedef Command process;
typedef Pipeline job;

struct termios shell_tmodes;
int shell_terminal = -1;
bool shell_owns_foreground = false;
job* current_fg_job = nullptr;

int last_executed_job_status = 0;  // value of $?, and for conditionals,
                                   // whether to do the job after || or &&

// The following three job-control lists are crucial for the shell to preform
// job controlling, please make sure you block undesired signals before you
// manipulate them.
map<pid_t, process*> process_list; // to quickly find a process and set state
map<pid_t, job*> job_list;         // to quickly find a job when given a pgid
// job-control id starts from `rbegin()->first + 1`
map<int, job*> stopped_or_bg_jobs;


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


const char* state_strings[] = { "Running", "Stopped", "Done", "Terminated" };

int find_job_in_stopped_or_bg_jobs(job* j)
{
    for (const auto& x : stopped_or_bg_jobs) {
        if (x.second == j)
            return x.first;
    }
    return 0; // not found
}

// find the job that has a child process with `pid`
job* find_job(pid_t pid)
{
    for (const auto& j : job_list) {
        for (process* p = j.second->first_cmd; p; p = p->next) {
            if (p->pid == pid)
                return j.second;
        }
    }
    return nullptr;
}

bool job_in_state(job* j, int state)
{
    for (process* p = j->first_cmd; p; p = p->next) {
        if (p->state != COMPLETED && p->state != state)
            return false;
    }
    return true;
}

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
    if (chdir(expand_tilde(path).c_str()) == -1) {
        perror("chdir");
        last_executed_job_status = EXIT_FAILURE;
    }
    else {
        get_cwd();
        last_executed_job_status = 0;
    }
}

void pwd()
{
    cout << cwd << endl;
    last_executed_job_status = 0;
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

void release_cmds(Command* cmd)
{
    Command* del;
    while (cmd) {
        del = cmd;
        cmd = cmd->next;
        delete del;
    }
}

// for command line paring if on error
void release_cmdline(Command* cmd)
{
    release_cmds(cmd);
}

// this routine just releases resources, while the `delete_job`
// routine also removes the job from the job-control lists
void release_job(job* j)
{
    release_cmds(j->first_cmd);
    delete j;
}

// used when a job is completed or for cleaning on shell exiting
auto delete_job(job* j)
{
    for (process* p = j->first_cmd; p; p = p->next) {
        process_list.erase(p->pid);
    }

    int job_index = find_job_in_stopped_or_bg_jobs(j);
    if (job_index != 0) {
        stopped_or_bg_jobs.erase(job_index);
    }

    auto it = job_list.find(j->pgid);
    assert(it != job_list.end());
    auto next = job_list.erase(it);

    release_job(j);
    return next;
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
            if (op == "2>&1") { // without operand
                // redirect stderr to where stdout (fd 1) is referring to
                ccur->errfd = STDOUT_FILENO;
                ccur->redirections.emplace_back(REDIRECT_OP_ERR2OUT, "");
                break;
            }
            ++it;
            if (op == "<") {
                if (it.type() != TYPE_NORMAL) {
                    msh_error("expected an input file after <");
                    release_cmdline(chead);
                    return nullptr;
                }
                ccur->redirections.emplace_back(REDIRECT_OP_INPUT, it.str());
            }
            else if (op == ">" || op == "&>" || op == ">&") {
                if (it.type() != TYPE_NORMAL) {
                    string msg = "expected an output file after " + op;
                    msh_error(msg.c_str());
                    release_cmdline(chead);
                    return nullptr;
                }
                // &> outfile  <==>  > outfile 2>&1
                ccur->redirections.emplace_back(REDIRECT_OP_OUTPUT, it.str());
                if (op != ">") { // &> or >&
                    ccur->errfd = STDOUT_FILENO;
                    ccur->redirections.emplace_back(REDIRECT_OP_ERR2OUT, "");
                }
            }
            else if (op == "2>") {
                if (it.type() != TYPE_NORMAL) {
                    msh_error("expected an output file for stderr after 2>");
                    release_cmdline(chead);
                    return nullptr;
                }
                ccur->redirections.emplace_back(REDIRECT_OP_ERROUT, it.str());
            }
            else if (op == ">>" || op == "&>>" || op == ">>&") {
                if (it.type() != TYPE_NORMAL) {
                    string msg = "expected an output file for appending after " + op;
                    msh_error(msg.c_str());
                    release_cmdline(chead);
                    return nullptr;
                }
                // &>> outfile  <==>  >> outfile 2>&1
                ccur->redirections.emplace_back(REDIRECT_OP_APPEND, it.str());
                if (op != ">>") { // &>> or >>&
                    ccur->errfd = STDOUT_FILENO;
                    ccur->redirections.emplace_back(REDIRECT_OP_ERR2OUT, "");
                }
            }
            else if (op == "2>>") {
                if (it.type() != TYPE_NORMAL) {
                    msh_error("expected an output file for appending stderr after 2>>");
                    release_cmdline(chead);
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
            auto next = it;
            ++next;
            if (!ccur || (next == parser.end() && (it.type() == TYPE_PIPE ||
                          it.type() == TYPE_AND || it.type() == TYPE_OR))) {
                string msg = "syntax error near unexpected token " + it.str();
                msh_error(msg.c_str());
                release_cmdline(chead);
                return nullptr;
            }
            if (it.type() == TYPE_PIPE && it.str() == "|&") {
                ccur->errfd = STDOUT_FILENO;
                ccur->redirections.emplace_back(REDIRECT_OP_ERR2OUT, "");
            }
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
    }
    bool first_cmd_in_pipeline = true;
    Command* prev_pipeline_last_cmd = nullptr;
    for (Command* cmd = chead; cmd; cmd = cmd->next) {
        if (first_cmd_in_pipeline) {
            jcur->first_cmd = cmd;
            jcur->str = cmd->str;
            first_cmd_in_pipeline = false;
            if (prev_pipeline_last_cmd)
                prev_pipeline_last_cmd->next = nullptr;
        }
        if (cmd->op == TYPE_PIPE) {
            jcur->str += " | " + cmd->next->str;
        }
        else { // might need to create a new pipeline
            if (cmd->next) {
                jcur->next = new Pipeline;
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

// block all signals and save current sigset to oldset if it isn't null
// used when we are entering a critical region/section
void block_all_signals(sigset_t* oldset)
{
    sigset_t mask;
    sigfillset(&mask);
    sigprocmask(SIG_BLOCK, &mask, oldset);
}

void block_chld_signal(sigset_t* oldset)
{
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    sigprocmask(SIG_BLOCK, &mask, oldset);
}

void unblock_chld_signal()
{
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    sigprocmask(SIG_UNBLOCK, &mask, nullptr);
}

int get_job_index(job* j)
{
    for (const auto& x: stopped_or_bg_jobs) {
        if (x.second == j)
            return x.first;
    }
    return 0; // not found
}

void format_job_info(int job_index, job* j)
{
    cout << '[' << job_index << ']' << '\t'
         << std::left << std::setfill(' ')
         << std::setw(11) << j->pgid
         << std::setw(13) << state_strings[j->state]
         << j->str << endl;
}

// set uncompleted processes in job `j` with `state`
void set_job_state(job* j, int state)
{
    for (process* p = j->first_cmd; p; p = p->next) {
        if (p->state != COMPLETED)
            p->state = state;
    }
    j->state = state;
}

// delete completed and terminated jobs
void delete_finished_jobs()
{
    for (auto it = job_list.begin(); it != job_list.end(); ) {
        job* j = it->second;
        if (j->state == COMPLETED || j->state == TERMINATED) {
            if (!j->notified) { // `notified` is false only for terminated jobs
                int job_index = find_job_in_stopped_or_bg_jobs(j);
                if (job_index != 0)
                    format_job_info(job_index, j);
                else
                    cerr << "Killed" << endl;
            }
            it = delete_job(j);
        }
        else {
            ++it;
        }
    }
}

void jobs()
{
    sigset_t oldset;
    block_all_signals(&oldset);
    for (const auto& j : stopped_or_bg_jobs) {
        format_job_info(j.first, j.second);
        j.second->notified = true;
    }
    delete_finished_jobs();
    sigprocmask(SIG_SETMASK, &oldset, nullptr);
    last_executed_job_status = 0;
}

void wait_for_job(job* j, sigset_t* oldset)
{
    while (j->state == RUNNING)
        sigsuspend(oldset);
}

void put_job_in_foreground(job* j, bool cont, sigset_t* oldset)
{
    claim_foreground(j->pgid);
    if (cont) {
        // set terminal modes for the job
        tcsetattr(shell_terminal, TCSADRAIN, &j->tmodes);
        if (kill(- j->pgid, SIGCONT) < 0)
            perror("kill (SIGCONT)");
        else
            set_job_state(j, RUNNING);
    }
    wait_for_job(j, oldset);
    claim_foreground(0);
    if (j->state == STOPPED) {
        cout << '\n';
        format_job_info(find_job_in_stopped_or_bg_jobs(j), j);
        // we don't need to save tmodes if the job is completed or terminated
        tcgetattr(shell_terminal, &j->tmodes);
    }
    // We need to restore shell tmodes since the previous foreground job
    // (e.g. vim) may have messed up the terminal modes.
    tcsetattr(shell_terminal, TCSADRAIN, &shell_tmodes);
}

void fg(int job_index)
{
    sigset_t oldset;
    block_all_signals(&oldset);
    delete_finished_jobs();
    if (stopped_or_bg_jobs.empty()) {
        msh_error("fg: no jobs");
        sigprocmask(SIG_SETMASK, &oldset, nullptr);
        last_executed_job_status = EXIT_FAILURE;
        return;
    }
    if (job_index == 0)
        job_index = stopped_or_bg_jobs.rbegin()->first;

    if (stopped_or_bg_jobs.count(job_index) == 1) {
        current_fg_job = stopped_or_bg_jobs[job_index];
        current_fg_job->foreground = true; // if it was a background job before
        cout << current_fg_job->str << endl;

        if (current_fg_job->state == RUNNING) // previously ran in background
            put_job_in_foreground(current_fg_job, /*cont=*/false, &oldset);
        else // stopped
            put_job_in_foreground(current_fg_job, /*cont=*/true, &oldset);

        if (current_fg_job->state == TERMINATED && !current_fg_job->notified) {
            cerr << "Terminated" << endl;
            delete_job(current_fg_job);
        }

        current_fg_job = nullptr;
        last_executed_job_status = 0;
    }
    else {
        string msg = "fg: %" + to_string(job_index) + ": no such job";
        msh_error(msg.c_str());
        last_executed_job_status = EXIT_FAILURE;
    }
    sigprocmask(SIG_SETMASK, &oldset, nullptr);
}

void bg(int job_index)
{
    sigset_t oldset;
    block_all_signals(&oldset);
    delete_finished_jobs();
    if (stopped_or_bg_jobs.empty()) {
        msh_error("bg: no jobs");
        sigprocmask(SIG_SETMASK, &oldset, nullptr);
        last_executed_job_status = EXIT_FAILURE;
        return;
    }
    if (job_index == 0)
        job_index = stopped_or_bg_jobs.rbegin()->first;

    if (stopped_or_bg_jobs.count(job_index) == 1) {
        job* j = stopped_or_bg_jobs[job_index];
        j->foreground = false; // if it was a foreground job before
        cout << '[' << job_index << ']' << '\t' << j->str << " &" << endl;
        if (kill(- j->pgid, SIGCONT) < 0)
            perror("kill (SIGCONT)");
        else
            set_job_state(j, RUNNING);
        last_executed_job_status = 0;
    }
    else {
        string msg = "bg: %" + to_string(job_index) + ": no such job";
        msh_error(msg.c_str());
        last_executed_job_status = EXIT_FAILURE;
    }
    sigprocmask(SIG_SETMASK, &oldset, nullptr);
}

// true if there're no stopped jobs or already being notified
// if it's true, also clean resources
bool can_exit()
{
    // block signals to prevent sigchld handler from changing
    // the job-control lists and freeing jobs too
    sigset_t oldset;
    block_all_signals(&oldset);

    static bool stopped_jobs_notified = false;
    bool has_stopped_jobs = false;
    for (const auto& j : stopped_or_bg_jobs) {
        if (j.second->state == STOPPED) {
            has_stopped_jobs = true;
            break;
        }
    }
    if (has_stopped_jobs && !stopped_jobs_notified) {
        msh_error("There are stopped jobs");
        stopped_jobs_notified = true;
        // restore old signals if can not exit yet
        sigprocmask(SIG_SETMASK, &oldset, nullptr);
        last_executed_job_status = EXIT_FAILURE;
        return false;
    }
    for (auto it = job_list.begin(); it != job_list.end(); ) {
        int state = it->second->state;
        if (state != COMPLETED && state != TERMINATED) {
            if (kill(- it->second->pgid, SIGQUIT) < 0)
                perror("kill (SIGQUIT)");
        }
        it = delete_job(it->second);
    }
    return true;
}

void set_up_pipes_and_redirections(Command* cmd)
{
    // note that pipes are set up before redirections

    // set up pipes (process i/o), if any
    if (cmd->infd != 0) {  // stdin reads from a pipe read end
        dup2(cmd->infd, STDIN_FILENO);
        close(cmd->infd);
    }
    if (cmd->outfd != 1) { // stdout writes to a pipe write end
        dup2(cmd->outfd, STDOUT_FILENO);
        close(cmd->outfd);
    }

    // set up redirections, if any
    for (const auto& x : cmd->redirections) {
        // 2>&1, redirect stderr to where stdout is referring to
        if (x.first == REDIRECT_OP_ERR2OUT) {
            dup2(cmd->errfd, STDERR_FILENO); // dup2(1, 2);
            continue; // no file to open
        }

        // O_CLOEXEC flag permits a program to avoid additional
        // fcntl(2) F_SETFD operations to set the FD_CLOEXEC flag.
        int flags = cmd->pid == 0 ? 0 : O_CLOEXEC;
        if (x.first == REDIRECT_OP_INPUT)
            flags |= O_RDONLY;
        else if (x.first == REDIRECT_OP_OUTPUT || x.first == REDIRECT_OP_ERROUT)
            flags |= O_CREAT | O_WRONLY | O_TRUNC;
        else if (x.first == REDIRECT_OP_APPEND || x.first == REDIRECT_OP_ERRAPP)
            flags |= O_CREAT | O_APPEND | O_WRONLY;

        int fd = open(x.second.c_str(), flags, /* mode: rw-r--r-- */0644);
        if (fd == -1) {
            perror("open");
            // only exit in child processes if `open` failed
            if (cmd->pid > 0)
                _exit(EXIT_FAILURE);
        }

        /* Note that
         *     cmd > out 2>&1
         * differs from
         *     cmd 2>&1 > out
         * The 1st redirects both stdout and stderr to the file `out` (first
         * redirects stdout to the file and then redirects stderr to where
         * stdout now has been tied to, i.e. where fd 1 is pointing to);
         * whereas the 2nd first redirects stderr to the file which fd 1
         * was pointing to (not necessarily the stdout file stream, may be
         * a pipe), then redirects stdout to the file `out`.
         *
         * The latter can be quite useful if we want to pipe only stderr:
         *
         *     cmd 2>&1 > /dev/null | do_something_with_stderr_from_cmd
         *
         * So, we need to pay attention to the order in which the operators
         * 2>&1 and > occur. This order can be determined at parsing phase
         * by emplacing back the redirections as the parser walks through
         * the command line from left to right.
         */

        // <, redirect input, stdin reads from file
        if (x.first == REDIRECT_OP_INPUT) {
            cmd->infd = fd;
            dup2(cmd->infd, STDIN_FILENO);
            close(cmd->infd);
        }
        // >/>>, redirect stdout to a file
        else if (x.first == REDIRECT_OP_OUTPUT || x.first == REDIRECT_OP_APPEND) {
            cmd->outfd = fd;
            dup2(cmd->outfd, STDOUT_FILENO);
            close(cmd->outfd);
        }
        // 2>/2>>, redirect stderr to a file
        else if (x.first == REDIRECT_OP_ERROUT || x.first == REDIRECT_OP_ERRAPP) {
            cmd->errfd = fd;
            dup2(cmd->errfd, STDERR_FILENO);
            close(cmd->errfd);
        }
    }
}

// return 0 if it's a built-in command that doesn't need to fork
// otherwise, -1 is returned
pid_t run_built_in_cmd(Command* cmd)
{
    const string& cmd_name = cmd->args[0];
    if (cmd_name != "exit" && cmd_name != "cd" && cmd_name != "jobs" &&
        cmd_name != "fg"   && cmd_name != "bg")
        return -1;

    // Redirections for built-in commands require us to make backups for
    // stdin/out/err, and then copy them back.
    int saved_infd  = dup(0);
    int saved_outfd = dup(1);
    int saved_errfd = dup(2);
    cmd->pid = 0;
    // can only be set once, child processes MUST NOT set it again
    set_up_pipes_and_redirections(cmd);

    if (cmd_name == "exit") {
        if (can_exit()) {
            int exit_code = EXIT_SUCCESS;
            if (cmd->args.size() > 1) {
                try {
                    exit_code = stoi(cmd->args[1]);
                }
                catch(const std::invalid_argument& e) {
                    msh_error("numeric argument required");
                    exit_code = EXIT_FAILURE;
                }
                catch(const std::out_of_range& e) {
                    msh_error("exit number out of range");
                    exit_code = EXIT_FAILURE;
                }
                catch (...) {
                    exit_code = EXIT_FAILURE;
                }
            }
            close(saved_infd);
            close(saved_outfd);
            close(saved_errfd);
            exit(exit_code);
        }
    }
    if (cmd_name == "cd") {
        string path = cmd->args.size() == 1 ? "~" : cmd->args[1];
        cd(path);
    }
    else if (cmd_name == "jobs") {
        jobs();
    }
    else if (cmd_name == "fg" || cmd_name == "bg") {
        // `fg` needs the original sigset to support `sigsuspend`
        unblock_chld_signal();
        if (cmd->args.size() == 1) {
            if (cmd_name == "fg")
                fg(0);
            else
                bg(0);
        }
        else {
            int job_index = -1;
            string index;
            try {
                if (cmd->args[1][0] != '%')
                    throw "job index not started with %";
                index = &cmd->args[1][1];
                job_index = stoi(index); // might throw
                if (job_index <= 0) // stoi succeeded, but might not be valid
                    throw "job index must be positive";
                if (cmd_name == "fg")
                    fg(job_index);
                else
                    bg(job_index);
            }
            catch (...) {
                string msg = cmd_name + ": " + cmd->args[1] + ": no such job";
                msh_error(msg.c_str());
                last_executed_job_status = EXIT_FAILURE;
            }
        }
    }

    // copy back
    if (cmd->infd != 0)
        dup2(saved_infd, 0);
    if (cmd->outfd != 1)
        dup2(saved_outfd, 1);
    if (cmd->errfd != 2)
        dup2(saved_errfd, 2);
    close(saved_infd);
    close(saved_outfd);
    close(saved_errfd);

    return 0;
}

// `getconf ARG_MAX` on my machine gives 2'097'152 (scary)
#define MAX_ARGS 32767 // 2^15 - 1

// spawn a child process to run `cmd`
// only a handful of built-in commands don't need to fork to run
pid_t run_command(Command* cmd, pid_t pgid, bool foreground)
{
    static const char* argv[MAX_ARGS];
    assert(cmd->args.size() > 0 && cmd->args.size() < MAX_ARGS);

    pid_t id = run_built_in_cmd(cmd);
    if (id == 0) // doesn't need to fork to run
        return 0;

    const string& cmd_name = cmd->args[0];

    // substitute $?
    if (cmd_name == "echo" && cmd->args.size() >= 2 && cmd->args[1] == "$?") {
        cmd->args[1] = to_string(last_executed_job_status);
    }

    // set up argv for execvp
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
        // since we blocked SIGCHLD in parent (subshells, etc. need it)
        unblock_chld_signal();

        if (shell_owns_foreground) {
            cmd->pid = getpid();
            if (pgid == -1) // not set yet
                pgid = cmd->pid;
            setpgid(cmd->pid, pgid);
            if (foreground)
                claim_foreground(pgid);

            // reset job-control & interactive signals to default for children
            signal(SIGCHLD, SIG_DFL);
            signal(SIGTSTP, SIG_DFL);
            signal(SIGTTIN, SIG_DFL);
            signal(SIGTTOU, SIG_DFL);
            signal(SIGINT, SIG_DFL);
            signal(SIGQUIT, SIG_DFL);
        }

        set_up_pipes_and_redirections(cmd);

        // run a forked built-in command or execvp one
        if (cmd_name == "pwd") {
            pwd();
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
    // phase by setting errfd = 1 (instead of stderr, 2), and then stderr will
    // be redirected to where stdout (fd 1) is pointing to (that is, the write
    // end of the pipe).
    sigset_t oldset;
    block_chld_signal(&oldset);

    int pipefd[2]{-1, -1}, prev_pipe_read_end = 0;
    for (Command* cmd = pipeline->first_cmd; cmd; cmd = cmd->next) {
        // pipe2 with O_CLOEXEC flag can close the file descriptors created by
        // pipe2 automatically in the child processes when they call execvp
        // (although closing them isn't an error). Using it enables us to only
        // focus on the file descriptors created in the parent process.
        if (cmd->next) {
            if (pipe2(pipefd, O_CLOEXEC) == -1)
                handle_error("pipe2");
            cmd->outfd = pipefd[1];
            cmd->next->infd = pipefd[0];
        }
        run_command(cmd, pipeline->pgid, pipeline->foreground);
        // add all child processes in pipeline to the same process group
        /* Note that there are race conditions (child processes may `execvp`
         * before parent `setpgid` or may not; ditto `claim_foreground`).
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
        if (shell_owns_foreground) {
            if (pipeline->pgid == -1 && cmd->pid != 0)
                pipeline->pgid = cmd->pid;
            if (cmd->pid != 0) {
                setpgid(cmd->pid, pipeline->pgid);
                process_list[cmd->pid] = cmd;
            }
        }
        // Draw pictures!
        // Parent closes current pipe's write end & previous pipe's read end.
        // There are a collection of great drawings in Harvard SEAS School's
        // CS61 course site. They clearly demonstrate how piping works in a
        // step-by-step fashion. Please see the subsection "Pipe in a shell"
        // in https://cs61.seas.harvard.edu/site/2021/ProcessControl/.
        if (cmd->next)
            close(pipefd[1]);
        if (prev_pipe_read_end != 0)
            close(prev_pipe_read_end);
        prev_pipe_read_end = pipefd[0];
    }

    if (pipeline->pgid == -1) { // job without child processes
        sigprocmask(SIG_SETMASK, &oldset, nullptr);
        return;
    }

    // add job to list
    job_list[pipeline->pgid] = pipeline;

    if (pipeline->foreground) {
        current_fg_job = pipeline;

        put_job_in_foreground(current_fg_job, /*cont=*/false, &oldset);

        current_fg_job = nullptr;
    }
    else { // always return success (0) for background jobs
        last_executed_job_status = 0;
        int next = stopped_or_bg_jobs.empty() ?
                   1 : stopped_or_bg_jobs.rbegin()->first + 1;
        stopped_or_bg_jobs[next] = pipeline;
        cout << '[' << next << ']' << '\t' << pipeline->pgid << endl;
    }

    sigprocmask(SIG_SETMASK, &oldset, nullptr);
}

void release_jobs(job* j)
{
    while (j) {
        job* del = j;
        j = j->next;
        release_job(del);
    }
}

void run_list_of_jobs(job* j)
{
    bool chain_is_true = true;
    job* prev_job = nullptr;
    vector<job*> skipped_jobs;
    while (j) {
        run_pipeline(j);
        if (j->pgid == -1) { // built-in commands
            j->state = COMPLETED;
            // release built-in commands like canceled jobs
            skipped_jobs.push_back(j);
        }
        if (j->state == TERMINATED) {
            release_jobs(j->next);
            break;
        }

        // true && false && true || echo yes || echo no
        //                 !eval                 !eval
        bool this_job_succeeded = last_executed_job_status == 0;
        if (prev_job && prev_job->next_is_or)
            chain_is_true = chain_is_true || this_job_succeeded;
        else
            chain_is_true = chain_is_true && this_job_succeeded;

        if (!j->last) { // not the last pipeline in a conditional
            sigset_t oldset;
            block_chld_signal(&oldset);
#if 1
            /*
             * In fish shell, `sleep 10 && echo foo &` is parsed as running
             * `sleep 10` in the foreground and running `echo foo` in the
             * background. However, `sleep 10 && echo foo` is treated as a
             * whole running in the background in bash shell.
             * Thus,
             * `sleep 0.2 && echo Second & sleep 0.1 && echo First` gives
             * "Second First" in fish and "First Second" in bash.
             * For now jobs in a background conditional are also waited in
             * the foregrond. To implement waiting jobs that are run in the
             * background, we'll need some extra data structures.
             * For example, we may need a vector that holds a list of
             * conditionals that run in the background. And we will decide
             * whether to run the next job after an AND/OR based on the
             * return status of the background job in the exactly same
             * fashion as we do here in the foreground.
             */
            wait_for_job(j, &oldset);
#else
            // wait for the job in a conditional chain in background
            if (!j->foreground) {
                // to be implemented...
            }
#endif
            sigprocmask(SIG_SETMASK, &oldset, nullptr);

            // skip subsequent ORs if chain is true or
            //      subsequent ANDs if chain is false
            while ((chain_is_true && j->next_is_or) ||
                  (!chain_is_true && !j->next_is_or)) {
                // ... chain || job2  (chain status is true)
                // ... chain && job2  (chain status is false)
                skipped_jobs.push_back(j->next);
                j = j->next;
                if (j->last)
                    break;
            }
        }
        else {
            // reset chain status
            chain_is_true = true;
        }
        prev_job = j;
        j = j->next;
    }

    for (job* skip : skipped_jobs)
        release_job(skip);

    sigset_t oldset;
    block_all_signals(&oldset);
    delete_finished_jobs();
    sigprocmask(SIG_SETMASK, &oldset, nullptr);
}

void sigchld_handler(int sig)
{
    (void) sig;
    int old_errno = errno;
    int wstatus;
    pid_t pid;
    while ((pid = waitpid(-1, &wstatus, WNOHANG | WUNTRACED)) > 0) {
        job* j = find_job(pid);
        assert(j);
        process* waited_process = process_list[pid];
        if (WIFEXITED(wstatus)) {
            if (!waited_process->next) { // the last process in the job
                last_executed_job_status = WEXITSTATUS(wstatus);
            }
            waited_process->state = COMPLETED;
            if (job_in_state(j, COMPLETED)) {
                j->state = COMPLETED;
                break;
            }
        }
        else if (WIFSIGNALED(wstatus)) {
            if (!waited_process->next) {
                last_executed_job_status = WTERMSIG(wstatus);
                // don't notify interrupted jobs to the user
                if (last_executed_job_status != SIGINT)
                    j->notified = false;
            }
            waited_process->state = TERMINATED;
            if (job_in_state(j, TERMINATED)) {
                j->state = TERMINATED;
                break;
            }
        }
        else if (WIFSTOPPED(wstatus)) {
            if (!waited_process->next) {
                last_executed_job_status = WSTOPSIG(wstatus);
                int job_index = find_job_in_stopped_or_bg_jobs(j);
                if (job_index == 0) { // not found
                    job_index = stopped_or_bg_jobs.empty() ?
                        1 : stopped_or_bg_jobs.rbegin()->first + 1;
                    stopped_or_bg_jobs[job_index] = j;
                }
            }
            waited_process->state = STOPPED;
            if (job_in_state(j, STOPPED)) {
                j->state = STOPPED;
                break;
            }
        }
    }
    errno = old_errno;
}

void sigint_handler(int sig)
{
    (void) sig;
    int old_errno = errno;
    if (current_fg_job) {
        // not necessarily kills it, e.g. vim, emacs, etc.
        if (kill(- current_fg_job->pgid, SIGINT) < 0)
            perror("kill (SIGINT)");
    }
    else {
        cout << '\n';
        print_prompt();
    }
    errno = old_errno;
}

void sigtstp_handler(int sig)
{
    (void) sig;
    int old_errno = errno;
    if (current_fg_job) {
        if (kill(- current_fg_job->pgid, SIGTSTP) < 0)
            perror("kill (SIGTSTP)");
    }
    errno = old_errno;
}

// claim_foreground(pgid)
//    Mark `pgid` as the current foreground process group for this terminal.
//    This uses some ugly Unix warts, so we provide it for you.
int claim_foreground(pid_t pgid)
{
    // YOU DO NOT NEED TO UNDERSTAND THIS.

    // Initialize state first time we're called.
    static int ttyfd = -1;
    static pid_t shell_pgid = -1;
    if (ttyfd < 0) { // initialize the shell
        // We need a fd for the current terminal, so open /dev/tty.
        int fd = open("/dev/tty", O_RDWR);
        assert(fd >= 0);
        // Re-open to a large file descriptor (>=10) so that pipes and such
        // use the expected small file descriptors.
        ttyfd = fcntl(fd, F_DUPFD, 10);
        assert(ttyfd >= 0);
        close(fd);
        // The /dev/tty file descriptor should be closed in child processes.
        fcntl(ttyfd, F_SETFD, FD_CLOEXEC);
        // Only mess with /dev/tty's controlling process group if the shell
        // is in /dev/tty's controlling process group.
        shell_terminal = ttyfd;
        shell_pgid = getpgrp();
        shell_owns_foreground = (shell_pgid == tcgetpgrp(ttyfd));

        // set signal handlers for the shell
        if (shell_owns_foreground) {
            signal(SIGTTIN, SIG_IGN);
            signal(SIGTTOU, SIG_IGN);
            signal(SIGCHLD, sigchld_handler);
            signal(SIGTSTP, sigtstp_handler);
            signal(SIGINT, sigint_handler);
            // SIGQUIT produces a core dump when it terminates the process
            signal(SIGQUIT, SIG_IGN);

            // save default terminal attributes for shell
            tcgetattr(shell_terminal, &shell_tmodes);
        }
    }

    // Set the terminal's controlling process group to `p` (so processes in
    // group `p` can output to the screen, read from the keyboard, etc.).
    if (shell_owns_foreground && pgid)
        return tcsetpgrp(shell_terminal, pgid);
    if (shell_owns_foreground)
        return tcsetpgrp(shell_terminal, shell_pgid);
    // shell is not run interactively
    return 0;
}

#ifndef SH61_TESTS

int main(int argc, char* argv[])
{
    claim_foreground(0);

    FILE* command_file = stdin;
    // check for filename option: read commands from file
    if (argc > 1) {
        command_file = fopen(argv[1], "rb");
        if (!command_file)
            handle_error(argv[1]);
    }

    char buf[BUFSIZ];
    int bufpos = 0;
    while (true) {
        print_prompt();

        // read a string, checking for error or EOF
        if (fgets(&buf[bufpos], BUFSIZ - bufpos, command_file) == nullptr) {
            if (ferror(command_file)) {
                // ignore EINTR errors
                if (errno == EINTR) {
                    clearerr(command_file);
                    buf[bufpos] = 0;
                    continue;
                }
                else {
                    perror("fgets");
                    break;
                }
            }
            if (feof(command_file)) {
                cout << "exit\n";
                if (can_exit()) // clean resources before exiting
                    break;
                else {
                    clearerr(command_file);
                    continue;
                }
            }
        }

        // if a complete command line has been provided, run it
        bufpos = strlen(buf);
        if (bufpos == BUFSIZ - 1 || (bufpos > 0 && buf[bufpos - 1] == '\n')) {
            if (job* j = parse_line(buf)) {
                run_list_of_jobs(j);
            }
            bufpos = 0;
        }
    }

    return 0;
}

#else // sh61 tests

int main(int argc, char* argv[])
{
    // init cwd
    get_cwd();

    FILE* command_file = stdin;
    bool quiet = false;

    // Check for `-q` option: be quiet (print no prompts)
    if (argc > 1 && strcmp(argv[1], "-q") == 0) {
        quiet = true;
        --argc, ++argv;
    }

    // Check for filename option: read commands from file
    if (argc > 1) {
        command_file = fopen(argv[1], "rb");
        if (!command_file) {
            perror(argv[1]);
            return 1;
        }
    }

    // - Put the shell into the foreground
    // - Ignore the SIGTTOU signal, which is sent when the shell is put back
    //   into the foreground
    claim_foreground(0);
    set_signal_handler(SIGTTOU, SIG_IGN);

    char buf[BUFSIZ];
    int bufpos = 0;
    bool needprompt = true;

    while (true) {
        // Print the prompt at the beginning of the line
        if (needprompt && !quiet) {
            printf("sh61[%d]$ ", getpid());
            fflush(stdout);
            needprompt = false;
        }

        // Read a string, checking for error or EOF
        if (fgets(&buf[bufpos], BUFSIZ - bufpos, command_file) == nullptr) {
            if (ferror(command_file)) {
                // ignore EINTR errors
                if (errno == EINTR) {
                    clearerr(command_file);
                    buf[bufpos] = 0;
                    continue;
                }
                else {
                    perror("fgets");
                    break;
                }
            }
            if (feof(command_file)) {
                if (can_exit()) // clean resources before exiting
                    break;
                else {
                    clearerr(command_file);
                    continue;
                }
            }
        }

        // If a complete command line has been provided, run it
        bufpos = strlen(buf);
        if (bufpos == BUFSIZ - 1 || (bufpos > 0 && buf[bufpos - 1] == '\n')) {
            if (job* j = parse_line(buf)) {
                run_list_of_jobs(j);
            }
            bufpos = 0;
            needprompt = 1;
        }
    }

    return 0;
}
#endif
