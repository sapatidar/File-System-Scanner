#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <stdatomic.h>
#include <limits.h>
#include <stdint.h>
#include <pwd.h>
#include <grp.h>

#define _XOPEN_SOURCE 700
#define MAX_PATH_LENGTH 4096
#define MAX_THREADS 8
#define INITIAL_QUEUE_SIZE 1000

typedef struct {
    int files_processed;
    int dirs_processed;
} ThreadStats;

typedef struct {
    char** paths;
    int front;
    int rear;
    int count;
    int capacity;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
    int waiting_threads;
    atomic_int active_processes;
} WorkQueue;

typedef struct {
    char path[MAX_PATH_LENGTH];
    off_t size;
    mode_t mode;
    time_t mtime;
    char owner[256];    // Added to store owner name
    char group[256];    // Added to store group name
} FileInfo;

ThreadStats thread_stats[MAX_THREADS];
WorkQueue work_queue;
pthread_t thread_pool[MAX_THREADS];
volatile sig_atomic_t running = 1;
FILE* output_file;
char* output_format;
pthread_mutex_t output_mutex = PTHREAD_MUTEX_INITIALIZER;

char** allowed_extensions = NULL;
int num_extensions = 0;
off_t size_threshold = 0;
off_t maxSize = (off_t)(((uintmax_t)1 << (sizeof(off_t) * CHAR_BIT - 1)) - 1);
char* owner_filter = NULL;
char* group_filter = NULL;
time_t mtime_after = 0;
time_t mtime_before = 0;
mode_t permission_filter = 0;

void queue_init(WorkQueue* queue, int initial_capacity) {
    queue->paths = malloc(initial_capacity * sizeof(char*));
    if (!queue->paths) {
        perror("Failed to allocate memory for the queue");
        exit(EXIT_FAILURE);
    }
    for (int i = 0; i < initial_capacity; i++) {
        queue->paths[i] = malloc(MAX_PATH_LENGTH);
        if (!queue->paths[i]) {
            perror("Failed to allocate memory for queue paths");
            exit(EXIT_FAILURE);
        }
    }
    queue->front = 0;
    queue->rear = -1;
    queue->count = 0;
    queue->capacity = initial_capacity;
    queue->waiting_threads = 0;
    atomic_init(&queue->active_processes, 0);
    pthread_mutex_init(&queue->mutex, NULL);
    pthread_cond_init(&queue->not_empty, NULL);
    pthread_cond_init(&queue->not_full, NULL);
}

void queue_destroy(WorkQueue* queue) {
    for (int i = 0; i < queue->capacity; i++) {
        free(queue->paths[i]);
    }
    free(queue->paths);
    pthread_mutex_destroy(&queue->mutex);
    pthread_cond_destroy(&queue->not_empty);
    pthread_cond_destroy(&queue->not_full);
}

void queue_resize(WorkQueue* queue) {
    int new_capacity = queue->capacity * 2;
    char** new_paths = malloc(new_capacity * sizeof(char*));
    if (!new_paths) {
        perror("Failed to allocate memory for resized queue");
        exit(EXIT_FAILURE);
    }
    for (int i = 0; i < new_capacity; i++) {
        new_paths[i] = malloc(MAX_PATH_LENGTH);
        if (!new_paths[i]) {
            perror("Failed to allocate memory for resized queue paths");
            exit(EXIT_FAILURE);
        }
    }

    for (int i = 0; i < queue->count; i++) {
        int index = (queue->front + i) % queue->capacity;
        strncpy(new_paths[i], queue->paths[index], MAX_PATH_LENGTH);
    }

    for (int i = 0; i < queue->capacity; i++) {
        free(queue->paths[i]);
    }
    free(queue->paths);

    queue->paths = new_paths;
    queue->capacity = new_capacity;
    queue->front = 0;
    queue->rear = queue->count - 1;
}

void queue_push(WorkQueue* queue, const char* path) {
    pthread_mutex_lock(&queue->mutex);
    while (queue->count >= queue->capacity && running) {
        queue_resize(queue);
    }
    if (!running) {
        pthread_mutex_unlock(&queue->mutex);
        return;
    }
    queue->rear = (queue->rear + 1) % queue->capacity;
    strncpy(queue->paths[queue->rear], path, MAX_PATH_LENGTH - 1);
    queue->paths[queue->rear][MAX_PATH_LENGTH - 1] = '\0';
    queue->count++;
    pthread_cond_signal(&queue->not_empty);
    pthread_mutex_unlock(&queue->mutex);
}

int queue_pop(WorkQueue* queue, char* path) {
    pthread_mutex_lock(&queue->mutex);
    queue->waiting_threads++;
    while (queue->count == 0 && running) {
        pthread_cond_wait(&queue->not_empty, &queue->mutex);
    }
    queue->waiting_threads--;
    if (queue->count == 0) {
        pthread_mutex_unlock(&queue->mutex);
        return 0;
    }
    strncpy(path, queue->paths[queue->front], MAX_PATH_LENGTH);
    queue->front = (queue->front + 1) % queue->capacity;
    queue->count--;
    pthread_cond_signal(&queue->not_full);
    pthread_mutex_unlock(&queue->mutex);
    return 1;
}

void check_termination_condition(WorkQueue* queue) {
    int active = atomic_load(&queue->active_processes);
    if (active == 0 && queue->count == 0) {
        // If no active processes and queue is empty, terminate
        printf("No active processes and empty queue. Initiating self-termination.\n");
        kill(getpid(), SIGTERM);
    }
}

void handle_signal(int signum) {
    running = 0;
    pthread_cond_broadcast(&work_queue.not_empty);
    pthread_cond_broadcast(&work_queue.not_full);
}

int matches_extension(const char* path) {
    if (num_extensions == 0) return 1;  // Process all files if no extensions are specified
    const char* ext = strrchr(path, '.');
    if (!ext) return 0;  // No extension, skip file

    for (int i = 0; i < num_extensions; i++) {
        if (strcmp(ext, allowed_extensions[i]) == 0) return 1;
    }

    return 0;
}

int matches_size(off_t size) {
    return size >= size_threshold && size <= maxSize;
}

int matches_owner(const char* path) {
    if (!owner_filter) return 1;
    struct stat st;
    if (lstat(path, &st) == -1) return 0;
    struct passwd* pw = getpwuid(st.st_uid);
    return pw && strcmp(pw->pw_name, owner_filter) == 0;
}

int matches_group(const char* path) {
    if (!group_filter) return 1;
    struct stat st;
    if (lstat(path, &st) == -1) return 0;
    struct group* grp = getgrgid(st.st_gid);
    return grp && strcmp(grp->gr_name, group_filter) == 0;
}

int matches_mtime(const char* path) {
    struct stat st;
    if (lstat(path, &st) == -1) return 0;
    if (mtime_after && st.st_mtime < mtime_after) return 0;
    if (mtime_before && st.st_mtime > mtime_before) return 0;
    return 1;
}

int matches_permissions(const char* path) {
    if(permission_filter==0) return 1;
    struct stat st;
    if (lstat(path, &st) == -1) return 0;
    return (st.st_mode & 0777) == permission_filter;
}

void process_file(const char* path) {
    struct stat st;
    if (lstat(path, &st) == -1) {
        fprintf(stderr, "Error: Failed to stat file %s: %s\n", path, strerror(errno));
        return;
    }

    if (!matches_extension(path) ||
        !matches_size(st.st_size) ||
        !matches_owner(path) ||
        !matches_group(path) ||
        !matches_mtime(path) ||
        !matches_permissions(path)
        )
        return;

    FileInfo info;
    strncpy(info.path, path, MAX_PATH_LENGTH - 1);
    info.path[MAX_PATH_LENGTH - 1] = '\0';
    info.size = st.st_size;
    info.mode = st.st_mode;
    info.mtime = st.st_mtime;

    // Get owner information
    struct passwd* pwd = getpwuid(st.st_uid);
    if (pwd != NULL) {
        strncpy(info.owner, pwd->pw_name, 255);
        info.owner[255] = '\0';
    } else {
        snprintf(info.owner, 256, "%d", st.st_uid);
    }

    // Get group information
    struct group* grp = getgrgid(st.st_gid);
    if (grp != NULL) {
        strncpy(info.group, grp->gr_name, 255);
        info.group[255] = '\0';
    } else {
        snprintf(info.group, 256, "%d", st.st_gid);
    }

    pthread_mutex_lock(&output_mutex);

    if (strcmp(output_format, "csv") == 0) {
        // CSV format: Path, Size, Type, Permissions, Owner, Group, Last Modified
        fprintf(output_file, "\"%s\",\"%ld\",\"%s\",\"%o\",\"%s\",\"%s\",\"%s\"\n", 
                info.path, 
                (long)info.size,
                S_ISDIR(info.mode) ? "Directory" :
                S_ISREG(info.mode) ? "Regular File" :
                S_ISLNK(info.mode) ? "Symbolic Link" : "Other",
                info.mode & 0777,
                info.owner,
                info.group,
                ctime(&info.mtime));
    } else if (strcmp(output_format, "json") == 0) {
        // JSON format
        fprintf(output_file, "{\n");
        fprintf(output_file, "  \"path\": \"%s\",\n", info.path);
        fprintf(output_file, "  \"size\": %ld,\n", (long)info.size);
        fprintf(output_file, "  \"type\": \"%s\",\n", 
                S_ISDIR(info.mode) ? "Directory" :
                S_ISREG(info.mode) ? "Regular File" :
                S_ISLNK(info.mode) ? "Symbolic Link" : "Other");
        fprintf(output_file, "  \"permissions\": %o,\n", info.mode & 0777);
        fprintf(output_file, "  \"owner\": \"%s\",\n", info.owner);
        fprintf(output_file, "  \"group\": \"%s\",\n", info.group);
        fprintf(output_file, "  \"last_modified\": \"%s\"\n", ctime(&info.mtime));
        fprintf(output_file, "},\n");
    } else {
        // Default to plain text format
        fprintf(output_file, "Path: %s\n", info.path);
        fprintf(output_file, "Size: %ld bytes\n", (long)info.size);
        fprintf(output_file, "Type: %s\n", 
                S_ISDIR(info.mode) ? "Directory" :
                S_ISREG(info.mode) ? "Regular File" :
                S_ISLNK(info.mode) ? "Symbolic Link" : "Other");
        fprintf(output_file, "Permissions: %o\n", info.mode & 0777);
        fprintf(output_file, "Owner: %s\n", info.owner);
        fprintf(output_file, "Group: %s\n", info.group);
        fprintf(output_file, "Last Modified: %s", ctime(&info.mtime));
        fprintf(output_file, "-------------------\n");
    }

    fflush(output_file);
    pthread_mutex_unlock(&output_mutex);
}


void* worker_thread(void* arg) {
    char path[MAX_PATH_LENGTH];
    int thread_id = (intptr_t)arg;  // Pass thread ID as an argument
    thread_stats[thread_id].files_processed = 0;
    thread_stats[thread_id].dirs_processed = 0;

    while (running) {
        if (!queue_pop(&work_queue, path)) {
            break;
        }
        atomic_fetch_add(&work_queue.active_processes, 1);
        DIR* dir = opendir(path);
        if (!dir) {
            fprintf(stderr, "Error: Failed to open directory %s: %s\n", path, strerror(errno));
            atomic_fetch_sub(&work_queue.active_processes, 1);
            continue;
        }
        struct dirent* entry;
        while ((entry = readdir(dir)) != NULL && running) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                continue;
            }
            char full_path[MAX_PATH_LENGTH];
            if (snprintf(full_path, MAX_PATH_LENGTH, "%s/%s", path, entry->d_name) >= MAX_PATH_LENGTH) {
                fprintf(stderr, "Error: Path too long for %s/%s\n", path, entry->d_name);
                continue;
            }
            struct stat st;
            if (lstat(full_path, &st) == -1) {
                fprintf(stderr, "Error: Failed to stat file %s: %s\n", full_path, strerror(errno));
                continue;
            }

            process_file(full_path);

            if (S_ISDIR(st.st_mode)) {
                thread_stats[thread_id].dirs_processed++;  // Update directory count
                queue_push(&work_queue, full_path);
            } else {
                thread_stats[thread_id].files_processed++;  // Update file count
            }
        }
        closedir(dir);
        atomic_fetch_sub(&work_queue.active_processes, 1);
        check_termination_condition(&work_queue);
    }
    pthread_exit(NULL);
}

int main(int argc, char* argv[]) {
    clock_t start_time, end_time;

    if (argc < 4) {
        fprintf(stderr, "Usage: %s <directory> <output_file> <output_file_type> [options]...\n", argv[0]);
        fprintf(stderr, "Options:\n");
        fprintf(stderr, "  extension <num> <ext1> ... <extN>   Include only files with these extensions\n");
        fprintf(stderr, "  minSize <size>                     Include only files larger than this size\n");
        fprintf(stderr, "  maxSize <size>                     Include only files smaller than this size\n");
        fprintf(stderr, "  owner <owner_name>                 Include only files owned by this user\n");
        fprintf(stderr, "  group <group_name>                 Include only files belonging to this group\n");
        fprintf(stderr, "  mtime_after <YYYY-MM-DD>           Include only files modified after this date\n");
        fprintf(stderr, "  mtime_before <YYYY-MM-DD>          Include only files modified before this date\n");
        fprintf(stderr, "  permissions <octal_perm>           Include only files with these permissions\n");
        return EXIT_FAILURE;
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    queue_init(&work_queue, 100);
    output_file = fopen(argv[2], "w");
    if (!output_file) {
        perror("Failed to open output file");
        return EXIT_FAILURE;
    }
    output_format = argv[3];

    if(argc>4){
        for(int i=4; i<argc; i=i+2){
            if (strcmp("extension", argv[i]) == 0){
                num_extensions = atoi(argv[i+1]);
                allowed_extensions = malloc(num_extensions * sizeof(char*));

                for(int j=0; j<num_extensions; j++){
                    allowed_extensions[j] = argv[i+j+2];
                }
                i = i+num_extensions;
            }else if(strcmp("minSize", argv[i]) == 0){
                size_threshold = atoll(argv[i+1]);
            }else if(strcmp("maxSize", argv[i]) == 0){
                maxSize = atoll(argv[i+1]);
            }else if (strcmp(argv[i], "owner") == 0) {
                owner_filter = argv[i + 1];
            } else if (strcmp(argv[i], "group") == 0) {
                group_filter = argv[i + 1];
            } else if (strcmp(argv[i], "mtime_after") == 0) {
                struct tm tm;
                strptime(argv[i + 1], "%Y-%m-%d", &tm);
                mtime_after = mktime(&tm);
            } else if (strcmp(argv[i], "mtime_before") == 0) {
                struct tm tm;
                strptime(argv[i + 1], "%Y-%m-%d", &tm);
                mtime_before = mktime(&tm);
            } else if (strcmp(argv[i], "permissions") == 0) {
                permission_filter = strtol(argv[i + 1], NULL, 8); // Octal input
            }else{
                printf("Invalid arguments.\n");
                fprintf(stderr, "Usage: %s <directory> <output_file> <output_file_type> [options]...\n", argv[0]);
                fprintf(stderr, "Options:\n");
                fprintf(stderr, "  extension <num> <ext1> ... <extN>   Include only files with these extensions\n");
                fprintf(stderr, "  minSize <size>                     Include only files larger than this size\n");
                fprintf(stderr, "  maxSize <size>                     Include only files smaller than this size\n");
                fprintf(stderr, "  owner <owner_name>                 Include only files owned by this user\n");
                fprintf(stderr, "  group <group_name>                 Include only files belonging to this group\n");
                fprintf(stderr, "  mtime_after <YYYY-MM-DD>           Include only files modified after this date\n");
                fprintf(stderr, "  mtime_before <YYYY-MM-DD>          Include only files modified before this date\n");
                fprintf(stderr, "  permissions <octal_perm>           Include only files with these permissions\n");
                return EXIT_FAILURE;
            }
        }
    }

    printf("\n=== Scanner Configuration ===\n");
    printf("Directory to scan: %s\n", argv[1]);
    printf("Output file: %s\n", argv[2]);
    printf("Output format: %s\n", output_format);

    if (num_extensions > 0) {
        printf("File extensions to include:");
        for (int i = 0; i < num_extensions; i++) {
            printf(" %s", allowed_extensions[i]);
        }
        printf("\n");
    } else {
        printf("File extensions: All\n");
    }

    printf("Size filters:\n");
    printf("  Minimum size: %ld bytes\n", (long)size_threshold);
    printf("  Maximum size: %ld bytes\n", (long)maxSize);

    printf("Owner/Group filters:\n");
    printf("  Owner filter: %s\n", owner_filter ? owner_filter : "None");
    printf("  Group filter: %s\n", group_filter ? group_filter : "None");

    if (permission_filter) {
        printf("Permission filter: 0%o\n", permission_filter);
    } else {
        printf("Permission filter: None\n");
    }
    printf("=========================\n\n");

    queue_push(&work_queue, argv[1]);
    start_time = clock();  // Start timer

    for (int i = 0; i < MAX_THREADS; i++) {
        if (pthread_create(&thread_pool[i], NULL, worker_thread, (void*)(intptr_t)i) != 0) {
            fprintf(stderr, "Failed to create thread %d\n", i);
            running = 0;
            break;
        }
    }

    for (int i = 0; i < MAX_THREADS; i++) {
        pthread_join(thread_pool[i], NULL);
    }

    end_time = clock();  // End timer
    double execution_time = ((double)(end_time - start_time)) / CLOCKS_PER_SEC;

    // Log total execution time
    printf("Total execution time: %.2f seconds\n", execution_time);

    // Log file and directory counts for each thread
    for (int i = 0; i < MAX_THREADS; i++) {
        printf("Thread %d processed %d files and %d directories\n", i, thread_stats[i].files_processed, thread_stats[i].dirs_processed);
    }

    fclose(output_file);
    queue_destroy(&work_queue);
    pthread_mutex_destroy(&output_mutex);

    return EXIT_SUCCESS;
}

