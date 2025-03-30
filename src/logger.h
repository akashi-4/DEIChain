/* 
    João Victor Furukawa - 2021238987
    Gladys Maquena - 2022242385
*/
#ifndef LOGGER_H
#define LOGGER_H

// Initialize the logger
void logger_init(const char* log_file_path);

// Close the logger and free resources
void logger_close(void);

// Log a message to both console and file
void log_message(const char* format, ...);

// Debug message with configurable output
void debug_message(const char* format, ...);

#endif /* LOGGER_H */