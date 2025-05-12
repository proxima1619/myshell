#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <errno.h>

#define MAX_INPUT 2048
#define MAX_TOKENS 128
#define MAX_PIPES 16

// 함수 원형 선언 부분
void display_prompt(void);
int tokenize_input(char *input, char *tokens[], const char *delim);
int execute_builtin(char **args);
void execute_pipeline(char **commands, int cmd_count);
int run_command(char *command, int background);
void process_input(char *input);
void handle_zombie(int sig);

// 마지막 명령어 상태
int last_status = 0;

int main(void) {
    // 좀비 프로세스 방지를 위한 시그널 핸들러 설정
    struct sigaction sa;
    sa.sa_handler = handle_zombie;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);
    
    char input[MAX_INPUT];
    
    while (1) {
        display_prompt();
        
        // 사용자 입력 읽기
        if (!fgets(input, sizeof(input), stdin)) {
            break; // EOF
        }
        
        // 개행 문자 제거
        size_t len = strlen(input);
        if (len > 0 && input[len-1] == '\n') {
            input[len-1] = '\0';
        }
        
        // exit 명령어 확인
        if (strcmp(input, "exit") == 0) {
            break;
        }
        
        // 명령어 처리
        process_input(input);
    }
    
    return 0;
}

// 현재 디렉토리를 포함한 프롬프트 표시
void display_prompt(void) {
    char cwd[MAX_INPUT];
    if (getcwd(cwd, sizeof(cwd))) {
        printf("\033[1;34m%s\033[0m$ ", cwd); // 파란색 프롬프트
    } else {
        printf("$ ");
    }
    fflush(stdout);
}

// 입력을 토큰으로 분리
int tokenize_input(char *input, char *tokens[], const char *delim) {
    int count = 0;
    char *saveptr;
    char *token = strtok_r(input, delim, &saveptr);
    
    while (token && count < MAX_TOKENS - 1) {
        tokens[count++] = token;
        token = strtok_r(NULL, delim, &saveptr);
    }
    
    tokens[count] = NULL;
    return count;
}

// 내장 명령어 처리
int execute_builtin(char **args) {
    if (!args[0]) return 0;
    
    // cd 명령어
    if (strcmp(args[0], "cd") == 0) {
        char *target = args[1] ? args[1] : getenv("HOME");
        if (chdir(target) != 0) {
            perror("cd");
            return 1;
        }
        return 0;
    }
    
    // pwd 명령어
    if (strcmp(args[0], "pwd") == 0) {
        char cwd[MAX_INPUT];
        if (getcwd(cwd, sizeof(cwd))) {
            printf("%s\n", cwd);
            return 0;
        } else {
            perror("pwd");
            return 1;
        }
    }
    
    return -1; // 내장 명령어가 아님
}

// 파이프라인 실행 (멀티 파이프 지원)
void execute_pipeline(char **commands, int cmd_count) {
    int pipes[MAX_PIPES][2];
    pid_t pids[MAX_PIPES];
    
    // 모든 파이프 먼저 생성
    for (int i = 0; i < cmd_count - 1; i++) {
        if (pipe(pipes[i]) == -1) {
            perror("pipe");
            return;
        }
    }
    
    // 각 명령어에 대해 프로세스 생성
    for (int i = 0; i < cmd_count; i++) {
        pids[i] = fork();
        
        if (pids[i] < 0) {
            perror("fork");
            return;
        }
        
        if (pids[i] == 0) { // 자식 프로세스
            // 입력 리다이렉션 (첫 번째 명령어 제외)
            if (i > 0) {
                dup2(pipes[i-1][0], STDIN_FILENO);
            }
            
            // 출력 리다이렉션 (마지막 명령어 제외)
            if (i < cmd_count - 1) {
                dup2(pipes[i][1], STDOUT_FILENO);
            }
            
            // 중요: 모든 파이프 끝을 자식에서 닫음
            for (int j = 0; j < cmd_count - 1; j++) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }
            
            // 명령어 파싱 및 실행
            char *args[MAX_TOKENS];
            tokenize_input(commands[i], args, " \t\r\n");
            
            // 내장 명령어 처리
            int builtin_result = execute_builtin(args);
            if (builtin_result >= 0) {
                exit(builtin_result);
            }
            
            // 외부 명령어 실행
            execvp(args[0], args);
            perror("execvp");
            exit(EXIT_FAILURE);
        }
    }
    
    // 부모에서도 모든 파이프 끝을 닫음 - 매우 중요!
    for (int i = 0; i < cmd_count - 1; i++) {
        close(pipes[i][0]);
        close(pipes[i][1]);
    }
    
    // 모든 자식 프로세스 대기
    for (int i = 0; i < cmd_count; i++) {
        int status;
        waitpid(pids[i], &status, 0);
        
        // 마지막 명령어의 종료 상태 저장
        if (i == cmd_count - 1) {
            last_status = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
        }
    }
}

// 단일 명령어 실행
int run_command(char *command, int background) {
    char *args[MAX_TOKENS];
    tokenize_input(command, args, " \t\r\n");
    
    if (!args[0]) return 0; // 빈 명령어
    
    // 내장 명령어 확인
    int builtin_result = execute_builtin(args);
    if (builtin_result >= 0) {
        return builtin_result;
    }
    
    // fork 및 실행
    pid_t pid = fork();
    
    if (pid < 0) {
        perror("fork");
        return 1;
    }
    
    if (pid == 0) { // 자식
        execvp(args[0], args);
        perror("execvp");
        exit(EXIT_FAILURE);
    } else { // 부모
        if (background) {
            printf("[백그라운드 프로세스 PID: %d]\n", pid);
            return 0;
        } else {
            int status;
            waitpid(pid, &status, 0);
            return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
        }
    }
}

// 다중 명령어 및 연산자 지원 입력 처리
void process_input(char *input) {
    // 세미콜론으로 구분된 명령어 처리
    char *semicolon_cmds[MAX_TOKENS];
    int semicolon_count = tokenize_input(input, semicolon_cmds, ";");
    
    // 각 세미콜론 구분 명령어 처리
    for (int i = 0; i < semicolon_count; i++) {
        char *and_cmds[MAX_TOKENS];
        int and_count = tokenize_input(semicolon_cmds[i], and_cmds, "&&");
        
        // AND 연결 명령어 처리
        for (int j = 0; j < and_count; j++) {
            // AND 체인에서 이전 명령어가 실패하면 중지
            if (j > 0 && last_status != 0) break;
            
            char *or_cmds[MAX_TOKENS];
            int or_count = tokenize_input(and_cmds[j], or_cmds, "||");
            
            // OR 연결 명령어 처리
            for (int k = 0; k < or_count; k++) {
                // OR 체인에서 이전 명령어가 성공하면 중지
                if (k > 0 && last_status == 0) break;
                
                // 파이프라인 처리
                char *pipe_cmds[MAX_TOKENS];
                int pipe_count = tokenize_input(or_cmds[k], pipe_cmds, "|");
                
                // 백그라운드 실행 확인
                int bg = 0;
                char *cmd = pipe_cmds[pipe_count - 1];
                int len = strlen(cmd);
                if (len > 0 && cmd[len - 1] == '&') {
                    cmd[len - 1] = '\0';
                    bg = 1;
                }
                
                // 명령어 또는 파이프라인 실행
                if (pipe_count > 1) {
                    execute_pipeline(pipe_cmds, pipe_count);
                } else {
                    last_status = run_command(pipe_cmds[0], bg);
                }
            }
        }
    }
}

// 좀비 프로세스 방지 시그널 핸들러
void handle_zombie(int sig) {
    (void)sig; // 미사용 매개변수 경고 방지
    
    // 가능한 모든 좀비 자식 프로세스 정리
    while (waitpid(-1, NULL, WNOHANG) > 0);
}

