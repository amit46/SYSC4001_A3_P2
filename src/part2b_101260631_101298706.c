#define _XOPEN_SOURCE 700

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <string.h>
#include <time.h>
#include <dirent.h>
#include <sys/wait.h>
#include <semaphore.h>

#include "shared_defs.h"

#define SHM_NAME   "/sysc4001_a3p2_shm"

// semaphores
#define SEM_RUBRIC   "/a3_rubric_sem"
#define SEM_QUEST    "/a3_question_sem"
#define SEM_LOADER   "/a3_loader_sem"

static sem_t *sem_rubric = NULL;  // protects rubric + rubric file
static sem_t *sem_questions = NULL;  // protects exam_state.question_done[]
static sem_t *sem_loader = NULL;  // protects load_exam / exam index

static void die(const char *msg) {
    perror(msg);
    exit(EXIT_FAILURE);
}

// generate a random double
static double rand_range(double min, double max) {
    return min + ((double)rand() / RAND_MAX) * (max - min);
}

// sleep for a given num of seconds
static void sleep_for(double seconds) {
    struct timespec ts;
    ts.tv_sec  = (time_t) seconds;
    ts.tv_nsec = (long)((seconds - ts.tv_sec) * 1e9);
    nanosleep(&ts, NULL);
}

// load rubric.txt into shared memory
static void load_rubric(SharedData *shm) {
    FILE *f = fopen("data/rubric.txt", "r");
    if (!f) die("fopen rubric");

    for (int i = 0; i < NUM_QUESTIONS; i++) {
        int qnum;
        char comma, character;
        if (fscanf(f, "%d %c %c", &qnum, &comma, &character) != 3) {
            // fprintf(stderr, "Bad rubric format on line %d\n", i + 1); // for debugging a rubric error
            exit(EXIT_FAILURE);
        }
        shm->rubric[i].question_num = qnum;
        shm->rubric[i].rubric_char  = character;
    }

    fclose(f);
}

// save current rubric from shared memory back to file
static void save_rubric(SharedData *shm) {
    FILE *f = fopen("data/rubric.txt", "w");
    if (!f) die("fopen rubric for write");

    for (int i = 0; i < NUM_QUESTIONS; i++) {
        fprintf(f, "%d, %c\n", shm->rubric[i].question_num, shm->rubric[i].rubric_char);
    }

    fclose(f);
}

// scan data/exams and populate exam_list in shared memory (not neccessarily needed but reduces hardcoded names)
static void build_exam_list(SharedData *shm) {
    DIR *dir = opendir("data/exams");
    if (!dir) die("opendir exams");

    struct dirent *ent;
    int i = 0;
    while ((ent = readdir(dir)) != NULL) {
        // skip current and parent directory entries
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }

        if (i >= MAX_EXAMS) {
            break;
        }

        snprintf(shm->exam_list.filenames[i], sizeof(shm->exam_list.filenames[i]), "data/exams/%s", ent->d_name);
        i++;
    }
    closedir(dir);
    shm->exam_list.num_exams = i;
}

// load exam at index into shared memory (exam_state)
static void load_exam(SharedData *shm, int index) {
    if (index < 0 || index >= shm->exam_list.num_exams) {
        fprintf(stderr, "No more exams. Index=%d\n", index);
        return;
    }

    const char *fname = shm->exam_list.filenames[index];
    FILE *f = fopen(fname, "r");
    if (!f) die("fopen exam");

    // error handling
    int sid;
    if (fscanf(f, "%d", &sid) != 1) {
        fprintf(stderr, "Bad exam file: %s\n", fname); // for debugging
        exit(EXIT_FAILURE);
    }
    fclose(f);

    shm->exam_state.student_id         = sid;
    shm->exam_state.current_exam_index = index;
    for (int i = 0; i < NUM_QUESTIONS; i++) {
        shm->exam_state.question_done[i] = false;
    }

    printf("[PARENT] Loaded exam %s (student %04d)\n", fname, sid);
    fflush(stdout);
}

// worker loop for each TA process
static void ta_loop(int ta_id, SharedData *shm) {
    srand(time(NULL) + getpid());

    while (1) {
        int sid = shm->exam_state.student_id;
        if (sid == 9999) {
            printf("[TA %d] Sentinel exam reached. Exiting.\n", ta_id);
            fflush(stdout);
            break;
        }

        printf("[TA %d] Starting exam for student %04d\n", ta_id, sid);
        fflush(stdout);

        // go through rubric and correct if needed
        for (int i = 0; i < NUM_QUESTIONS; i++) {
            sleep_for(rand_range(0.5, 1.0)); // 0.5-1 second random delay

            // enter critical section for rubric
            sem_wait(sem_rubric);

            int q = shm->rubric[i].question_num;
            char old = shm->rubric[i].rubric_char;

            // 50% chance to correct rubric
            if ((rand() % 2) == 0) {
                // if correcting, it moves character to the next ASCII
                char newc = old + 1;
                shm->rubric[i].rubric_char = newc;
                printf("[TA %d] Correcting rubric for Q%d: %c -> %c\n", ta_id, q, old, newc);
                fflush(stdout);
                save_rubric(shm);
            } else {
                printf("[TA %d] No change to rubric for Q%d (%c)\n", ta_id, q, old);
                fflush(stdout);
            }

            sem_post(sem_rubric);
        }

        // mark questions on the current exam. protected with semaphore
        while (1) {
            // check if all questions done and/or claim one (using semaphore)
            sem_wait(sem_questions);

            // check if all questions are done
            int all_done = 1;
            for (int i = 0; i < NUM_QUESTIONS; i++) {
                if (!shm->exam_state.question_done[i]) {
                    all_done = 0;
                    break;
                }
            }

            if (all_done) {
                sem_post(sem_questions);
                break;  // this TA is finished with this exam
            }

            // try to claim a random question
            int q = rand() % NUM_QUESTIONS;

            if (!shm->exam_state.question_done[q]) {
                // claim this question
                shm->exam_state.question_done[q] = true;
                sem_post(sem_questions);

                // simulate marking time between 1-2s
                sleep_for(rand_range(1.0, 2.0));

                printf("[TA %d] Marked question %d for student %04d\n", ta_id, q + 1, sid);
                fflush(stdout);
            } else {
                // someone else already claimed the semaphore
                sem_post(sem_questions);
                sleep_for(0.1);
            }
        }

        printf("[TA %d] Finished all questions for student %04d\n", ta_id, sid);
        fflush(stdout);

        // load next exam if this TA is TA 0
        if (ta_id == 0) {
            sem_wait(sem_loader); // protect since only one TA can access at a time
            int next_index = shm->exam_state.current_exam_index + 1;
            load_exam(shm, next_index);
            sem_post(sem_loader);
        }
    }
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <num_TAs>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    int num_TAs = atoi(argv[1]);
    if (num_TAs < 2) {
        fprintf(stderr, "num_TAs must be >= 2\n");
        exit(EXIT_FAILURE);
    }

    // create + size shared memory
    int fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (fd == -1) die("shm_open");

    if (ftruncate(fd, sizeof(SharedData)) == -1)
        die("ftruncate");

    SharedData *shm = mmap(NULL, sizeof(SharedData),
                            PROT_READ | PROT_WRITE,
                            MAP_SHARED, fd, 0);
    if (shm == MAP_FAILED) die("mmap");

    // only parent initializes the content
    memset(shm, 0, sizeof(SharedData));
    load_rubric(shm);
    build_exam_list(shm);
    load_exam(shm, 0);  // first exam

    // semaphore setup
    sem_unlink(SEM_RUBRIC); // clean up (due to previous crashes and errors)
    sem_unlink(SEM_QUEST);
    sem_unlink(SEM_LOADER);

    sem_rubric = sem_open(SEM_RUBRIC, O_CREAT | O_EXCL, 0666, 1);
    sem_questions = sem_open(SEM_QUEST, O_CREAT | O_EXCL, 0666, 1);
    sem_loader = sem_open(SEM_LOADER, O_CREAT | O_EXCL, 0666, 1);

    if (sem_rubric == SEM_FAILED || sem_questions == SEM_FAILED || sem_loader == SEM_FAILED) {
        die("sem_open");
    }

    // fork TAs
    for (int i = 0; i < num_TAs; i++) {
        pid_t pid = fork();
        if (pid < 0) die("fork");
        if (pid == 0) {
            // child process
            ta_loop(i, shm);

            munmap(shm, sizeof(SharedData));
            close(fd);

            // close semaphores in child
            sem_close(sem_rubric);
            sem_close(sem_questions);
            sem_close(sem_loader);

            exit(EXIT_SUCCESS);
        }
    }

    // parent waits for children
    for (int i = 0; i < num_TAs; i++) {
        wait(NULL);
    }

    printf("[PARENT] All TAs finished. Cleaning up.\n");

    // close and clean semaphores
    sem_close(sem_rubric);
    sem_close(sem_questions);
    sem_close(sem_loader);

    sem_unlink(SEM_RUBRIC);
    sem_unlink(SEM_QUEST);
    sem_unlink(SEM_LOADER);

    munmap(shm, sizeof(SharedData));
    close(fd);
    shm_unlink(SHM_NAME);

    return 0;
}
