/* 
    João Victor Furukawa - 2021238987
    Gladys Maquena - 2022242385
*/

#include "logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <time.h>
#include <string.h>
#include <pthread.h>

// External debug control flags
extern int CONSOLE_DEBUG;
extern int LOG_DEBUG;

// File pointer for the log file
static FILE* log_file = NULL;

// Mutex for thread-safe logging
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

// Default log file path
static char log_file_path[256] = "DEIChain_log.txt";

void logger_init(const char* file_path) {
    pthread_mutex_lock(&log_mutex);
    
    // If a path is provided, use it
    if (file_path != NULL) {
        strncpy(log_file_path, file_path, sizeof(log_file_path) - 1);
    }
    
    // Open the log file
    log_file = fopen(log_file_path, "a");
    if (log_file == NULL) {
        fprintf(stderr, "Error: Could not open log file %s\n", log_file_path);
    } else {
        // Write a separator to indicate a new session
        time_t now = time(NULL);
        struct tm* timeinfo = localtime(&now);
        char time_str[20];
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", timeinfo);
        
        fprintf(log_file, "\n--- New logging session started at %s ---\n", time_str);
        fflush(log_file);
    }
    
    pthread_mutex_unlock(&log_mutex);
}

void logger_close(void) {
    pthread_mutex_lock(&log_mutex);
    
    if (log_file != NULL) {
        // Write a separator to indicate end of session
        time_t now = time(NULL);
        struct tm* timeinfo = localtime(&now);
        char time_str[20];
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", timeinfo);
        
        fprintf(log_file, "--- Logging session ended at %s ---\n\n", time_str);
        fclose(log_file);
        log_file = NULL;
    }
    
    pthread_mutex_unlock(&log_mutex);
}

void log_message(const char* format, ...) {
    pthread_mutex_lock(&log_mutex);
    
    // Get current time
    time_t now = time(NULL);
    struct tm* timeinfo = localtime(&now);
    char time_str[20];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", timeinfo);
    
    // Format the message
    va_list args;
    va_start(args, format);
    
    // Print to console
    printf("[%s] ", time_str);
    vprintf(format, args);
    
    // Reset va_list for file output
    va_end(args);
    va_start(args, format);
    
    // Write to log file if open
    if (log_file != NULL) {
        fprintf(log_file, "[%s] ", time_str);
        vfprintf(log_file, format, args);
        fflush(log_file);
    }
    
    va_end(args);
    pthread_mutex_unlock(&log_mutex);
}

void debug_message(const char* format, ...) {
    // Only proceed if DEBUG is enabled
    #if DEBUG
    pthread_mutex_lock(&log_mutex);
    
    // Get current time
    time_t now = time(NULL);
    struct tm* timeinfo = localtime(&now);
    char time_str[20];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", timeinfo);
    
    // Format the message
    va_list args;
    va_start(args, format);
    
    // Print to console if console debug is enabled
    #if CONSOLE_DEBUG
    printf("[DEBUG %s] ", time_str);
    vprintf(format, args);
    #endif
    
    // Reset va_list for file output
    va_end(args);
    va_start(args, format);
    
    // Write to log file if log debug is enabled and file is open
    #if LOG_DEBUG
    if (log_file != NULL) {
        fprintf(log_file, "[DEBUG %s] ", time_str);
        vfprintf(log_file, format, args);
        fflush(log_file);
    }
    #endif
    
    va_end(args);
    pthread_mutex_unlock(&log_mutex);
    #endif
}