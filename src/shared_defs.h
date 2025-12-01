#ifndef SHARED_DEFS_H
#define SHARED_DEFS_H

#include <stdbool.h>

#define NUM_QUESTIONS 5
#define MAX_EXAMS     128   // more than enough for your 20+ exams
#define MAX_NAME_LEN  64

// One line of the rubric: "1, A"
typedef struct {
    int  question_num;   // 1..5
    char rubric_char;    // 'A', 'B', ...
} RubricEntry;

// Metadata for the exam currently loaded in shared memory
typedef struct {
    int student_id;                   // 4-digit student number (0001..9999)
    int current_exam_index;           // index in exam list
    bool question_done[NUM_QUESTIONS];
} ExamState;

// List of exam filenames
typedef struct {
    int  num_exams;
    char filenames[MAX_EXAMS][MAX_NAME_LEN];
} ExamList;

// Everything we keep in shared memory
typedef struct {
    RubricEntry rubric[NUM_QUESTIONS];
    ExamState   exam_state;
    ExamList    exam_list;
    // Part 2.b will add semaphores here
} SharedData;

#endif // SHARED_DEFS_H
