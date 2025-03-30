#ifndef COMMON_H
#define COMMON_H

#include <time.h>


// Data structures 
typedef struct{
    int id; // PID of transaction generator + incremented number
    int reward; // 1 to 3 but it can be higher due to aging
    int sender_id; // PID of process
    int receiver_id; // random number
    double value; // quantity associated with the transaction
    time_t timestamp; // time of transaction
} Transaction;

typedef struct{
    int empty;
    int age;
    Transaction t;
} TransactionEntry;

typedef struct{
    int current_block_id;
    int transactions_pending_set;
    TransactionEntry ts[];
} TransactionPool;

typedef struct{
    int id;
    int transactions_count;
    Transaction transactions[];
} BlockchainLedger;

typedef struct{
    int num_miners;
    int tx_pool_size;
    int transactions_per_block;
    int blockchain_blocks;
} Config;

#endif