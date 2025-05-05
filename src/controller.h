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
void validator_process();
void print_statistics();
void initialize_statistics();
void free_statistics();
void update_statistics(MessageToStatistics message);
void statistics_process();
int create_message_queue();
void send_message(int msqid, MessageToStatistics *message);
void receive_message(int msqid, MessageToStatistics *message);
void free_message(MessageToStatistics *message);
void print_message(MessageToStatistics* message);
void get_transactions_from_transaction_pool();
Transaction* get_random_transactions(int num_transactions);
Block* create_block(int miner_id, Transaction* selected_tx);

// Signal handling
void signal_handler(int signum);

#endif /* CONTROLLER_H */
