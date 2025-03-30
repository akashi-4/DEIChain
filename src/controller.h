/* 
    João Victor Furukawa - 2021238987
    Gladys Maquena - 2022242385
*/
#ifndef CONTROLLER_H
#define CONTROLLER_H

#include "common.h"

// Configuration functions
Config read_config();
void setup_shared_memory(Config config);
void cleanup_shared_memory();

// Thread and process management
void create_miner_process(int num_miners);
void miner_process(int num_miners);
int wait_for_miner_threads();
void create_validator_process();
void create_statistics_process();

// Signal handling
void signal_handler(int signum);

// Statistics
void print_system_statistics();

#endif /* CONTROLLER_H */
