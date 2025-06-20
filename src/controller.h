/* 
    João Victor Furukawa - 2021238987
    Gladys Maquena - 2022242385
*/
#ifndef CONTROLLER_H
#define CONTROLLER_H

#include "common.h"



// Configuration functions
Config read_config();
void setup(Config config);

// Thread and process management

// Miner functions
void create_miner_process(int num_miners);
void miner_process(int num_miners);
int wait_for_miner_threads();

// Validator functions
void create_validator_process();
void validator_process();

// Statistics functions
void create_statistics_process();
void print_statistics(Statistics *stats);
void initialize_statistics(Statistics *stats);
void free_statistics(Statistics *stats);
void update_statistics(Statistics *stats, MessageToStatistics *message);
void statistics_process();

// Message queue functions
int create_message_queue();
void send_message(int msqid, MessageToStatistics *message);
void receive_message(int msqid, MessageToStatistics *message);
void print_message(MessageToStatistics* message);
MessageToStatistics* prepare_message(int miner_id, int valid_block, int credits, time_t block_timestamp, Transaction* transactions);

// Blockchain functions
void get_transactions_from_transaction_pool();
Transaction* get_random_transactions(int num_transactions);
Block* create_block(int miner_id, Transaction* selected_tx);
void send_block(Block* block, int miner_id);
int receive_block(MessageToValidator* message);
void remove_validated_transactions(Block* block);
void add_block_to_blockchain(Block* block);
void dump_ledger();
void initialize_ledger_log();
void append_block_to_ledger_log(Block* block);
void age_transactions_in_pool();


// Cleanup functions
void terminate_processes();
void cleanup_message_queue();
void cleanup_all_resources();
void cleanup_mutexes();
void cleanup_semaphores();
void cleanup_named_pipes();

// Signal handling
void signal_handler(int signum);
void miner_signal_handler(int signum);
void validator_signal_handler(int signum);
void statistics_signal_handler(int signum);
void controller_sigusr2_handler(int signum);

#endif /* CONTROLLER_H */
