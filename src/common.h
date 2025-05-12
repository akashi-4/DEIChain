/* 
    João Victor Furukawa - 2021238987
    Gladys Maquena - 2022242385
*/

#ifndef COMMON_H
#define COMMON_H

#include <time.h>  // Needed for time_t
#include <pthread.h>  // Needed for pthread_cond_t and pthread_mutex_t

// Shared memory keys
#define TX_POOL_KEY_PATH "/tmp"       // Path for ftok
#define TX_POOL_KEY_ID 'T'            // Project ID for Transaction Pool
#define BLOCKCHAIN_KEY_PATH "/tmp"    // Path for ftok
#define BLOCKCHAIN_KEY_ID 'B'         // Project ID for Blockchain
#define HASH_SIZE 65                  // SHA256_DIGEST_LENGTH * 2 + 1
#define INITIAL_HASH \
  "00006a8e76f31ba74e21a092cca1015a418c9d5f4375e7a4fec676e1d2ec1436"

#define TX_ID_LEN 64
#define TXB_ID_LEN 64

// Semaphore names
#define TX_POOL_SEM "/tx_pool_sem"    // Semaphore for transaction pool access

// Data structures 
typedef struct {
    int tx_id; // PID of transaction generator + incremented number
    int reward; // 1 to 3 but it can be higher due to aging
    int value; // quantity associated with the transaction
    time_t tx_timestamp; // time of transaction
} Transaction;

typedef struct {
    int txb_id; // Miner's thread ID + incremented number
    char prev_hash[HASH_SIZE]; // Hash of the previous block
    time_t txb_timestamp; // time of block
    int nonce; // nonce for PoW
    Transaction transactions[]; // Transactions in the block
}Block;
    
typedef struct {
    int empty;      // 1 if empty, 0 if occupied
    int age;        // Incremented when validator touches the pool
    Transaction t;  // The actual transaction
} TransactionEntry;

typedef struct {
    int *num_valid_blocks;
    int *num_invalid_blocks;
    double avg_time_to_verify_transaction;
    int *credits_of_each_miner;
    int total_number_of_blocks_validated;
    int total_number_of_blocks_in_the_blockchain;
    time_t *tx_timestamp;
} Statistics;

typedef struct {
    int size;                   // Size of the pool (TX_POOL_SIZE from config)
    int transactions_pending;   // Number of pending transactions
    int num_transactions_per_block;  // Number of transactions needed per block
    int next_tx_id;            // Global counter for generating unique transaction IDs
    pthread_mutex_t mutex;      // Mutex for condition variable
    pthread_cond_t enough_tx;   // Condition variable to signal when enough transactions are available
    TransactionEntry entries[]; // Flexible array member for the transaction entries
} TransactionPool;

typedef struct {
    int num_blocks;
    Block blocks[]; // Array of blocks
} BlockchainLedger;

typedef struct {
    int num_miners;
    int tx_pool_size;
    int transactions_per_block;
    int blockchain_blocks;
} Config;

typedef struct {
    long mtype;
    int miner_id;
    int valid_block;
    int credits;
    time_t block_timestamp;
    int num_timestamps;     // Store the number of timestamps
    time_t *tx_timestamp;   // Pointer to timestamp array
} MessageToStatistics;

// Message from miner to validator
typedef struct {
    int miner_id;
    Block block;
} MessageToValidator;

#endif