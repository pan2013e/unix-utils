#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <assert.h>

#include <fcntl.h>
#include <unistd.h>
#include <getopt.h>
#include <dirent.h>

const char* SHORT_OPTS = "hnpV";

#define NULL_OPTS {0, 0, 0, 0}

const struct option LONG_OPTS[] = {
    { "help",         no_argument, NULL, 'h' },
    { "numeric-sort", no_argument, NULL, 'n' },
    { "show-pids",    no_argument, NULL, 'p' },
    { "version",      no_argument, NULL, 'V' },
    NULL_OPTS
};

#define REDIRECT_TO_STDERR(...) fprintf(stderr, ##__VA_ARGS__)
#define PRINT REDIRECT_TO_STDERR

#define USAGE \
    "Usage: pstree [OPTION]\n" \
    "Print a tree of the specified process and its descendants.\n" \
    "\n" \
    "  -n, --numeric-sort\n" \
    "      Sort by PID instead of name.\n" \
    "  -p, --show-pids\n" \
    "      Show PIDs in addition to names.\n" \
    "  -V, --version\n" \
    "      Print version information and exit.\n" \
    "  -h, --help\n" \
    "      Print this help message and exit.\n"

#define VERSION_STRING "1.0.0"
#define VERSION \
    "pstree (psutils) " VERSION_STRING "\n" \
    "Copyright (C) 2022 Zhiyuan Pan\n" \

#define PROMPT_USAGE   PRINT(USAGE)
#define PROMPT_VERSION PRINT(VERSION)

#define MAX_NAME_LEN    256
#define MAX_TREE_DEPTH  256
#define MAX_TASKS       1024

#define SHOW_PID     1 << 0
#define NUMERIC_SORT 1 << 1
#define FORKED_TASK  1 << 2

#define IS_SHOW_PID(flag)     (((flag) & SHOW_PID) == SHOW_PID)
#define IS_NUMERIC_SORT(flag) (((flag) & NUMERIC_SORT) == NUMERIC_SORT)
#define IS_FORKED_TASK(flag)  (((flag) & FORKED_TASK) == FORKED_TASK)

#define START_PID 1
#define PROCFS_ROOT   "/proc"

#define PROC_DIR      PROCFS_ROOT "/" "%d"
#define PROC_NAME     PROC_DIR "/comm"
#define PROC_TASKS    PROC_DIR "/task"
#define PROC_CHILDREN PROC_TASKS "/" "%d" "/children"
#define GET_FMT_DIR(str, fmt, ...)   sprintf(str, fmt, ##__VA_ARGS__)

#define IS_DOT_PATH(path) ((path)[0] == '.')

#define INCR_INDENT(indent) (indent) += 1
#define DECR_INDENT(indent) (indent) -= 1

#define GET_PROC_NAME(pid) \
    char name[MAX_NAME_LEN] = { 0 }, path[MAX_NAME_LEN] = { 0 }; \
    GET_FMT_DIR(path, PROC_NAME, pid); \
    FILE* fp = fopen(path, "r"); \
    if (fp == NULL) { \
        perror("fopen"); \
        exit(EXIT_FAILURE); \
    } \
    fgets(name, MAX_NAME_LEN, fp); \
    name[strlen(name) - 1] = '\0'; \

#define PRINT_INDENT(level) \
    for (int i = 0; i < level; i++) { \
        if(i == level - 1) { \
            if(last_child[level]) \
                PRINT("└──"); \
            else \
                PRINT("├──"); \
        } else { \
            if (last_child[i + 1]) { \
                PRINT("   "); \
            } else { \
                PRINT("│  "); \
            } \
        } \
    }

int last_child[MAX_TREE_DEPTH];

int compare_pid(const void* a, const void* b) {
    return *(int*)a >= *(int*)b;
}

int parse_args(int argc, char** argv) {
    return getopt_long(argc, argv, SHORT_OPTS, LONG_OPTS, NULL);
}

void print_proc_name(int pid, int tabs, int flag) {
    GET_PROC_NAME(pid);
    char fmt[20] = { 0 };
    switch (IS_FORKED_TASK(flag) << 1 | IS_SHOW_PID(flag)) {
        case 0b00:
            strcpy(fmt,"%s\n");
            break;
        case 0b01:
            strcpy(fmt,"%s(%d)\n");
            break;
        case 0b10:
            strcpy(fmt,"{%s}\n");
            break;
        case 0b11:
            strcpy(fmt,"{%s}(%d)\n");
            break;
    }
    PRINT_INDENT(tabs);
    IS_SHOW_PID(flag) ? PRINT(fmt, name, pid) : PRINT(fmt, name);
    fclose(fp);
}

void print_forked_proc(int pid, int tabs, int num_tasks, int show_pid) {
    GET_PROC_NAME(pid);
    char fmt[20] = { 0 };
    PRINT_INDENT(tabs);
    switch ((show_pid & 0x1) << 1 | (num_tasks > 1)) {
        case 0b00:
            strcpy(fmt, "{%s}\n");
            PRINT(fmt, name);
            break;
        case 0b01:
            strcpy(fmt, "%d*[{%s}]\n");
            PRINT(fmt, num_tasks, name);
            break;
        case 0b10:
        case 0b11:
            strcpy(fmt, "{%s}(%d)\n");
            PRINT(fmt, name, pid);
            break;
    }
    fclose(fp);
}

void get_proc_tasks(int pid, int* tasks) {
    char path[MAX_NAME_LEN] = { 0 };
    GET_FMT_DIR(path, PROC_TASKS, pid);
    DIR* dir = opendir(path);
    if (dir == NULL) {
        perror("opendir");
        exit(EXIT_FAILURE);
    }
    struct dirent* ent;
    int cnt = 0;
    while ((ent = readdir(dir)) != NULL) {
        if (IS_DOT_PATH(ent->d_name)) {
            continue;
        }
        int tid = atoi(ent->d_name);
        tasks[cnt++] = tid;
    }
    closedir(dir);
}

void get_children(int pid, int tid, int* children) {
    char path[MAX_NAME_LEN] = { 0 };
    GET_FMT_DIR(path, PROC_CHILDREN, pid, tid);
    FILE* fp = fopen(path, "r");
    if (fp == NULL) {
        perror("fopen");
        exit(EXIT_FAILURE);
    }
    int cnt = 0;
    while ((fscanf(fp, "%d", &children[cnt++])) != EOF);
}

int get_arr_len(int* a) {
    int cnt = 0;
    while (a[cnt] != 0) {
        cnt++;
    }
    return cnt;
}

void do_pstree_impl(int flag, int pid, int is_last_child) {
    static int tabs = 0;
    last_child[tabs] |= is_last_child;
    print_proc_name(pid, tabs, flag);
    INCR_INDENT(tabs);
    int tasks[MAX_TASKS] = { 0 }, children[MAX_TASKS] = { 0 };
    get_proc_tasks(pid, tasks);
    int tasks_len = get_arr_len(tasks);
    assert(tasks_len > 0);
    for (int i = 0; i < tasks_len; i++) {
        get_children(pid, tasks[i], children);
    }
    int children_len = get_arr_len(children);
    if (IS_NUMERIC_SORT(flag)) {
        qsort(children, children_len, sizeof(int), compare_pid);
    }
    if (tasks_len > 1 && !IS_SHOW_PID(flag)) { // not showing pids, so merge the forked processes
        // last_child[tabs] |= children_len == 0;
        print_forked_proc(tasks[1], tabs, tasks_len - 1, 0); // ignore the task itself
    }
    for (int i = 1; i < tasks_len; i++) {
        if (tasks_len > 1 && IS_SHOW_PID(flag)) { // need to print the pid
            // last_child[tabs] |= i == tasks_len - 1 && children_len == 0;
            print_forked_proc(tasks[i], tabs, 1, 1);
        }
    }
    for (int j = 0; j < children_len; j++) {
        do_pstree_impl(flag, children[j], j == children_len - 1);
    }
    DECR_INDENT(tabs);
    last_child[tabs] = 0;
}

void do_pstree(int flag, int pid) {
    do_pstree_impl(flag, pid, 0);
}

int main(int argc, char** argv) {
    int opt = 0, flag = 0;
    while ((opt = parse_args(argc, argv)) != -1) {
        switch (opt) {
        case 'h':
            PROMPT_USAGE;
            return EXIT_SUCCESS;
        case 'V':
            PROMPT_VERSION;
            return EXIT_SUCCESS;
        case 'n':
            flag |= NUMERIC_SORT;
            break;
        case 'p':
            flag |= SHOW_PID;
            break;
        default:
            PROMPT_USAGE;
            return EXIT_FAILURE;
        } 
    }
    do_pstree(flag, START_PID);
    return EXIT_SUCCESS;
}