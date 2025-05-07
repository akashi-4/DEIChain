/* 
    João Victor Furukawa - 2021238987
    Gladys Maquena - 2022242385
*/
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <pthread.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/shm.h>
#include <semaphore.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/errno.h>

#include "common.h"
#include "logger.h"  // Include the logger header
#include "controller.h"
#include "pow.h"

#define CONFIG_FILE "config.cfg"
#define DEBUG 0        // Set to 1 to enable debug messages
#define MSG_SIZE (sizeof(MessageToStatistics) - sizeof(long))

// Booting the system, reading the configuration file, validating the data in the
// file, and applying the read configurations
// Creation of processes Miner, Validator, and Statistics 
// Setting up two shared memory segments (for Transaction Pool and Blockchain Ledger)
// Begin implementation for SIGINT signal capture (preliminary)

// Global flag for miner thread termination
volatile sig_atomic_t running_miner_threads = 1;
volatile sig_atomic_t print_stats_flag = 0;

// Global variables to manage threads
pthread_t *miner_threads = NULL;
int *thread_ids = NULL;
int num_miners_global = 0;
int num_transactions_per_block_global = 0;
int msqid = -1;

// Global variables for shared memory
int tx_pool_shmid = -1;
int blockchain_shmid = -1;
TransactionPool *transaction_pool = NULL;
BlockchainLedger *blockchain_ledger = NULL;
sem_t *tx_pool_sem = NULL;

Statistics stats;

// Add these global variables at the top with other globals
pid_t validator_pid = -1;
pid_t statistics_pid = -1;
pid_t miner_process_pid = -1;

Config read_config(){
    Config config = {0};
    FILE *config_file = fopen(CONFIG_FILE, "r");
    if(config_file == NULL){
        perror("Failed to open config file");
        exit(1);
    }
    
    char line[256];
    while(fgets(line, sizeof(line), config_file)){
        if(line[0] == '#' || line[0] == '\n'){
            continue;
        }

        char *key = strtok(line, "=");
        char *value = strtok(NULL, "=");

        if(key != NULL && value != NULL){
            key = strtok(key, " ");
            value = strtok(value, " ");

            if(strcmp(key, "NUM_MINERS") == 0){
                config.num_miners = atoi(value);
            }
            else if(strcmp(key, "TX_POOL_SIZE") == 0){
                config.tx_pool_size = atoi(value);
            }
            else if(strcmp(key, "BLOCKCHAIN_BLOCKS") == 0){
                config.blockchain_blocks = atoi(value);
            }
            else if(strcmp(key, "TRANSACTIONS_PER_BLOCK") == 0){
                config.transactions_per_block = atoi(value);
            }
        }
    }

    fclose(config_file);

    // Validate the config
    if(config.num_miners <= 0 || config.tx_pool_size <= 0 || config.blockchain_blocks <= 0 || config.transactions_per_block <= 0){
        #ifdef DEBUG
        log_message("Invalid configuration values\n");
        #endif
        exit(1);
    }
    
    // Store the number of miners globally and the number of transactions per block
    num_miners_global = config.num_miners;
    num_transactions_per_block_global = config.transactions_per_block;


    #ifdef DEBUG
    log_message("Configuration read successfully\n");
    log_message("NUM_MINERS = %d\n", config.num_miners);
    log_message("TX_POOL_SIZE = %d\n", config.tx_pool_size);
    log_message("TRANSACTIONS_PER_BLOCK = %d\n", config.transactions_per_block);
    log_message("BLOCKCHAIN_BLOCKS = %d\n", config.blockchain_blocks);
    #endif

    return config;
}

// Function to set up shared memory
void setup_shared_memory(Config config) {
    // Create and initialize the semaphore first
    // Unlink any existing semaphore
    sem_unlink(TX_POOL_SEM);
    tx_pool_sem = sem_open(TX_POOL_SEM, O_CREAT, 0644, config.tx_pool_size);
    if (tx_pool_sem == SEM_FAILED) {
        perror("CONTROLLER: Failed to create semaphore");
        exit(1);
    }
    log_message("CONTROLLER: Transaction pool semaphore created with count %d\n", config.tx_pool_size);

    // Size of pool and ledger
    size_t pool_size = sizeof(TransactionPool) + config.tx_pool_size * sizeof(TransactionEntry);
    size_t ledger_size = sizeof(BlockchainLedger);

    // Generate keys for shared memory segments
    key_t tx_pool_key = ftok(TX_POOL_KEY_PATH, TX_POOL_KEY_ID);
    key_t blockchain_key = ftok(BLOCKCHAIN_KEY_PATH, BLOCKCHAIN_KEY_ID);

    if (tx_pool_key == -1 || blockchain_key == -1) {
        perror("Failed to generate shared memory keys");
        exit(1);
    }

    // Create the shared memory segments using the generated keys
    tx_pool_shmid = shmget(tx_pool_key, pool_size, IPC_CREAT | 0664);
    blockchain_shmid = shmget(blockchain_key, ledger_size, IPC_CREAT | 0664);

    #ifdef DEBUG
    log_message("Transaction pool shared memory ID: %d\n", tx_pool_shmid);
    log_message("Blockchain ledger shared memory ID: %d\n", blockchain_shmid);
    #endif

    if (tx_pool_shmid == -1) {
        perror("Failed to create transaction pool shared memory segment");
        exit(1);
    }

    if (blockchain_shmid == -1) {
        perror("Failed to create blockchain ledger shared memory segment");
        exit(1);
    }

    // Attach the shared memory segments
    transaction_pool = (TransactionPool *)shmat(tx_pool_shmid, NULL, 0);
    if (transaction_pool == (TransactionPool *) -1) {
        perror("Failed to attach transaction pool shared memory segment");
        exit(1);
    }

    blockchain_ledger = (BlockchainLedger *)shmat(blockchain_shmid, NULL, 0);
    if (blockchain_ledger == (BlockchainLedger *) -1) {
        perror("Failed to attach blockchain ledger shared memory segment");
        exit(1);
    }

    #ifdef DEBUG
    log_message("Transaction pool attached at address: %p\n", transaction_pool);
    log_message("Blockchain ledger attached at address: %p\n", blockchain_ledger);
    #endif

    // Initialize the transaction pool
    transaction_pool->size = config.tx_pool_size;
    transaction_pool->transactions_pending = 0;
    transaction_pool->num_transactions_per_block = config.transactions_per_block;
    
    // Initialize mutex and condition variable
    pthread_mutexattr_t mutex_attr;
    pthread_condattr_t cond_attr;
    
    pthread_mutexattr_init(&mutex_attr);
    pthread_mutexattr_setpshared(&mutex_attr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&transaction_pool->mutex, &mutex_attr);
    
    pthread_condattr_init(&cond_attr);
    pthread_condattr_setpshared(&cond_attr, PTHREAD_PROCESS_SHARED);
    pthread_cond_init(&transaction_pool->enough_tx, &cond_attr);
    
    pthread_mutexattr_destroy(&mutex_attr);
    pthread_condattr_destroy(&cond_attr);

    for(int i = 0; i < config.tx_pool_size; i++){
        transaction_pool->entries[i].empty = 1;
        transaction_pool->entries[i].age = 0;
    }

    // Initialize the blockchain ledger
    blockchain_ledger->num_blocks = 0;
    strncpy(blockchain_ledger->blocks[0].prev_hash, INITIAL_HASH, HASH_SIZE);
}

// Function to clean up shared memory
void cleanup_shared_memory() {
    // Close and unlink semaphore
    if (tx_pool_sem != NULL) {
        sem_close(tx_pool_sem);
        sem_unlink(TX_POOL_SEM);
        tx_pool_sem = NULL;
        log_message("CONTROLLER: Transaction pool semaphore removed\n");
    }

    // Clean up mutex and condition variable
    if (transaction_pool != NULL) {
        pthread_mutex_destroy(&transaction_pool->mutex);
        pthread_cond_destroy(&transaction_pool->enough_tx);
    }

    // Detach from shared memory segments
    if (transaction_pool != NULL && transaction_pool != (TransactionPool *)-1) {
        if (shmdt(transaction_pool) == -1) {
            perror("Failed to detach transaction pool shared memory");
        } else {
            #ifdef DEBUG
            log_message("Detached from transaction pool shared memory\n");
            #endif
            transaction_pool = NULL;
        }
    }
    
    if (blockchain_ledger != NULL && blockchain_ledger != (BlockchainLedger *)-1) {
        if (shmdt(blockchain_ledger) == -1) {
            perror("Failed to detach blockchain ledger shared memory");
        } else {
            #ifdef DEBUG
            log_message("Detached from blockchain ledger shared memory\n");
            #endif
            blockchain_ledger = NULL;
        }
    }
    
    // Remove shared memory segments
    if (tx_pool_shmid != -1) {
        if (shmctl(tx_pool_shmid, IPC_RMID, NULL) == -1) {
            perror("Failed to remove transaction pool shared memory");
        } else {
            #ifdef DEBUG
            log_message("Removed transaction pool shared memory\n");
            #endif
            tx_pool_shmid = -1;
        }
    }
    
    if (blockchain_shmid != -1) {
        if (shmctl(blockchain_shmid, IPC_RMID, NULL) == -1) {
            perror("Failed to remove blockchain ledger shared memory");
        } else {
            #ifdef DEBUG
            log_message("Removed blockchain ledger shared memory\n");
            #endif
            blockchain_shmid = -1;
        }
    }
}

void cleanup_message_queue(){
    // Remove the message queue
    if(msqid != -1) {
        log_message("CONTROLLER: Removing message queue...\n");
        if(msgctl(msqid, IPC_RMID, 0) == -1){
            perror("CONTROLLER: Failed to remove message queue");
        } else {
            log_message("CONTROLLER: Message queue removed successfully\n");
        }
    }
}

// Function to create a block with transactions
Block* create_block(int miner_id, Transaction* selected_tx) {
    // Allocate memory for the block including space for transactions
    size_t block_size = sizeof(Block) + num_transactions_per_block_global * sizeof(Transaction);
    Block* block = (Block*)malloc(block_size);
    if (!block) {
        log_message("MINER: Failed to allocate memory for block\n");
        return NULL;
    }

    // Initialize block fields
    block->txb_id = miner_id;
    block->txb_timestamp = time(NULL);
    block->nonce = 0;

    // Copy the initial hash for the first block, or the previous block's hash otherwise
    if (blockchain_ledger->num_blocks == 0) {
        strncpy(block->prev_hash, INITIAL_HASH, HASH_SIZE);
    } else {
        Block* prev_block = &blockchain_ledger->blocks[blockchain_ledger->num_blocks - 1];
        char prev_hash[HASH_SIZE];
        compute_sha256(prev_block, prev_hash, num_transactions_per_block_global);
        strncpy(block->prev_hash, prev_hash, HASH_SIZE);
    }

    // Copy transactions
    memcpy(block->transactions, selected_tx, num_transactions_per_block_global * sizeof(Transaction));

    return block;
}

// Thread function
void *miner_thread(void *arg) {
    int id = *((int*)arg);
    log_message("MINER: Thread %d started\n", id);
    int printin = 1;

    while (running_miner_threads) {
        pthread_mutex_lock(&transaction_pool->mutex);
        while (running_miner_threads && 
               transaction_pool->transactions_pending < transaction_pool->num_transactions_per_block) {
            pthread_cond_wait(&transaction_pool->enough_tx, &transaction_pool->mutex);
        }
        
        // Check if we were woken up for shutdown
        if (!running_miner_threads) {
            pthread_mutex_unlock(&transaction_pool->mutex);
            break;
        }
        pthread_mutex_unlock(&transaction_pool->mutex);

        // Try to get random transactions
        Transaction* selected_tx = get_random_transactions(num_transactions_per_block_global);
        if (selected_tx != NULL) {
            printin = 1;
            log_message("MINER: Thread %d selected %d transactions\n", id, num_transactions_per_block_global);
            
            // Create a new block
            Block* block = create_block(id, selected_tx);
            if (!block) {
                free(selected_tx);
                continue;
            }

            // Try to mine the block
            PoWResult result = proof_of_work(block, num_transactions_per_block_global);
            if (!result.error) {
                log_message("MINER: Thread %d successfully mined block %d with hash %s\n", 
                          id, block->txb_id, result.hash);
                // TODO: Submit block to validator
                // MessageToStatistics* msg = prepare_message(id, 1, result.operations, 
                //                                           block->txb_timestamp, selected_tx);
                // send_message(msqid, msg);
                // free_message(msg);
            } else {
                log_message("MINER: Thread %d failed to mine block %d\n", id, block->txb_id);
            }

            free(block);  // Free the block after we're done with it
            free(selected_tx);  // Free the selected transactions
            sleep(1); // Small delay between mining attempts
        } else if(printin == 1){
            printin = 0;
            log_message("MINER: Thread %d - Not enough transactions in pool, waiting...\n", id);
        }
    }
    
    log_message("MINER: Thread %d shutting down\n", id);
    return NULL;
}

// Signal handler
void signal_handler(int signum) {
    if (signum == SIGINT) {
        log_message("\nReceived SIGINT, shutting down...\n");
        running_miner_threads = 0;  // Signal threads to stop
        
        // Wake up all waiting threads by modifying the condition they're waiting on
        pthread_mutex_lock(&transaction_pool->mutex);
        // Artificially set transactions_pending to trigger the wake-up condition
        transaction_pool->transactions_pending = transaction_pool->num_transactions_per_block;
        // Broadcast to wake up ALL waiting threads
        pthread_cond_broadcast(&transaction_pool->enough_tx);
        pthread_mutex_unlock(&transaction_pool->mutex);
    } else if (signum == SIGUSR1) {
        log_message("\nReceived SIGUSR1, printing statistics...\n");
        print_stats_flag = 1;  // Set flag to print statistics
    }
}

// Function to get random transactions from the pool
Transaction* get_random_transactions(int num_transactions_needed) {
    // Lock the mutex
    pthread_mutex_lock(&transaction_pool->mutex);
    
    // Wait until we have enough transactions
    while (transaction_pool->transactions_pending < transaction_pool->num_transactions_per_block) {
        pthread_cond_wait(&transaction_pool->enough_tx, &transaction_pool->mutex);
    }
    
    // Wait for semaphore
    if (sem_wait(tx_pool_sem) == -1) {
        pthread_mutex_unlock(&transaction_pool->mutex);
        perror("MINER: Failed to lock semaphore");
        return NULL;
    }

    Transaction* selected_transactions = malloc(num_transactions_needed * sizeof(Transaction));
    if (!selected_transactions) {
        log_message("MINER: Failed to allocate memory for transactions\n");
        sem_post(tx_pool_sem);
        pthread_mutex_unlock(&transaction_pool->mutex);
        return NULL;
    }

    // Count available transactions
    int available_transactions = 0;
    int* available_indices = malloc(transaction_pool->size * sizeof(int));
    if (!available_indices) {
        free(selected_transactions);
        log_message("MINER: Failed to allocate memory for indices\n");
        sem_post(tx_pool_sem);
        pthread_mutex_unlock(&transaction_pool->mutex);
        return NULL;
    }

    // Get indices of all non-empty transactions
    for (int i = 0; i < transaction_pool->size; i++) {
        if (!transaction_pool->entries[i].empty) {
            available_indices[available_transactions++] = i;
        }
    }

    // Check if we have enough transactions
    if (available_transactions < num_transactions_needed) {
        free(selected_transactions);
        free(available_indices);
        sem_post(tx_pool_sem);
        pthread_mutex_unlock(&transaction_pool->mutex);
        return NULL;
    }

    // Randomly select transactions
    for (int i = 0; i < num_transactions_needed; i++) {
        // Get a random index from available transactions
        int random_idx = rand() % available_transactions;
        int pool_idx = available_indices[random_idx];

        // Copy the transaction
        selected_transactions[i] = transaction_pool->entries[pool_idx].t;

        // Remove this index from available_indices by replacing it with the last one
        available_indices[random_idx] = available_indices[--available_transactions];
    }

    free(available_indices);
    
    // Release semaphore
    if (sem_post(tx_pool_sem) == -1) {
        perror("MINER: Failed to unlock semaphore");
        free(selected_transactions);
        pthread_mutex_unlock(&transaction_pool->mutex);
        return NULL;
    }

    pthread_mutex_unlock(&transaction_pool->mutex);
    return selected_transactions;
}

// Function to access the transaction pool shared memory
TransactionPool* access_transaction_pool() {
    // Generate the same key as in controller
    key_t tx_pool_key = ftok(TX_POOL_KEY_PATH, TX_POOL_KEY_ID);
    if (tx_pool_key == -1) {
        perror("MINER: Failed to generate transaction pool key");
        return NULL;
    }

    // Get the shared memory segment
    int shmid = shmget(tx_pool_key, 0, 0664); // Size 0 means we're accessing existing segment
    if (shmid == -1) {
        perror("MINER: Failed to access transaction pool shared memory");
        return NULL;
    }

    // Attach to the shared memory
    TransactionPool *pool = (TransactionPool *)shmat(shmid, NULL, 0);
    if (pool == (TransactionPool *)-1) {
        perror("MINER: Failed to attach to transaction pool");
        return NULL;
    }

    return pool;
}

void miner_process(int num_miners) {
    // Access the transaction pool
    transaction_pool = access_transaction_pool();
    if (transaction_pool == NULL) {
        log_message("MINER: Failed to access transaction pool\n");
        return;
    } else {
        log_message("MINER: Successfully accessed transaction pool\n");
    }

    // Open the semaphore
    tx_pool_sem = sem_open(TX_POOL_SEM, 0);
    if (tx_pool_sem == SEM_FAILED) {
        perror("MINER: Failed to open semaphore");
        shmdt(transaction_pool);
        return;
    }
    
    // Allocate memory for thread IDs and handles
    miner_threads = (pthread_t *)malloc(num_miners * sizeof(pthread_t));
    thread_ids = (int *)malloc(num_miners * sizeof(int));
    
    // Create the threads
    for (int i = 0; i < num_miners; i++) {
        thread_ids[i] = i + 1;  // Store thread ID
        
        // Create the Miner threads
        if (pthread_create(&miner_threads[i], NULL, miner_thread, &thread_ids[i]) != 0) {
            perror("MINER: Failed to create miner thread");
            exit(1);
        } else {
            log_message("MINER: Successfully created miner thread %d\n", i + 1);
        }
    }
    
    // Wait for threads to complete (they will run until running_miner_threads is set to 0)
    wait_for_miner_threads();
    
    // Cleanup
    if (tx_pool_sem != NULL) {
        sem_close(tx_pool_sem);
    }
    if (transaction_pool != NULL) {
        shmdt(transaction_pool);
    }
    
    log_message("MINER: Process shutting down\n");
}

// Function to wait for miner threads to complete (call this at the end)
int wait_for_miner_threads() {
    if (miner_threads == NULL) return 0;
    
    log_message("Waiting for all miner threads to finish...\n");
    
    // Wait for all Miner threads to complete
    for (int i = 0; i < num_miners_global; i++) {
        #if DEBUG
        debug_message("Joining miner thread %d\n", i + 1);
        #endif
        pthread_join(miner_threads[i], NULL);
    }
    
    // Free the memory allocated
    free(miner_threads);
    free(thread_ids);
    miner_threads = NULL;
    thread_ids = NULL;

    log_message("All miner threads have finished\n");
    log_message("MINER: Process shutting down\n");
    return 1;
}

void create_validator_process(){
    // Create the Validator process
    pid_t validator_id = fork();
    if(validator_id == 0){
        #if DEBUG
        debug_message("Creating validator process\n");
        #endif
        // Child process
        validator_process();
        exit(0);  // Exit after validator process completes
    } else if (validator_id < 0) {
        // Error handling
        perror("Failed to create validator process");
    } else {
        // Parent process
        validator_pid = validator_id;
        log_message("CONTROLLER: Created validator process with PID %d\n", validator_pid);
    }
}

void validator_process(){
    // whenever a block is received, it is validated and sent to the statistics process through the message queue
    // validation is done by: 
    // checking if the block is valid (has a valid PoW)
    // checking if the hash of the previous block is correct
    // check if the transactions are still in the transaction pool

    while(1){
        sleep(1);
    }

}

void print_statistics(){
    // Number of valid blocks submitted by each specific miner
    // Number of invalid blocks submitted by each specific miner
    // Average time to verify a transaction
    // Credits of each miner
    // Total number of blocks validated (both valid and invalid)
    // Total number of blocks in the blockchain
    log_message("STATISTICS: SIGUSR1 RECEIVED\n");


    // Print the statistics
    log_message("====================== MINER STATISTICS ======================\n");
    for(int i = 0; i < num_miners_global; i++){
        log_message("STATISTICS: Number of valid blocks submitted by miner %d: %d\n", i+1, stats.num_valid_blocks[i]);    
        log_message("STATISTICS: Number of invalid blocks submitted by miner %d: %d\n", i+1, stats.num_invalid_blocks[i]);  
        log_message("STATISTICS: Average time to verify a transaction: %d\n", stats.avg_time_to_verify_transaction);
        log_message("STATISTICS: Credits of miner %d: %d\n", i+1, stats.credits_of_each_miner[i]);
    }
    log_message("===============================================================\n");
    log_message("====================== GLOBAL STATISTICS ======================\n");
    log_message("STATISTICS: Total number of blocks validated (both valid and invalid): %d\n", stats.total_number_of_blocks_validated);
    log_message("STATISTICS: Total number of blocks in the blockchain: %d\n", stats.total_number_of_blocks_in_the_blockchain);
    log_message("=================================================================\n");
}

void initialize_statistics(){
    stats.num_valid_blocks = calloc(num_miners_global, sizeof(int));
    stats.num_invalid_blocks = calloc(num_miners_global, sizeof(int));
    stats.tx_timestamp = calloc(num_transactions_per_block_global, sizeof(time_t));
    stats.avg_time_to_verify_transaction = 0;
    stats.credits_of_each_miner = calloc(num_miners_global, sizeof(int));
    stats.total_number_of_blocks_validated = 0;
    stats.total_number_of_blocks_in_the_blockchain = 0;
}

void free_statistics(){
    free(stats.num_valid_blocks);
    free(stats.num_invalid_blocks);
    free(stats.tx_timestamp);
}

void update_statistics(MessageToStatistics message){
    if(message.valid_block == 1){
        stats.num_valid_blocks[message.miner_id]++;
        stats.total_number_of_blocks_in_the_blockchain++;
        stats.credits_of_each_miner[message.miner_id] += message.credits;
    } else {
        stats.num_invalid_blocks[message.miner_id]++;
    }
    stats.total_number_of_blocks_validated++;

    // Calculate the average time to verify a transaction
    for(int i = 0; i < num_transactions_per_block_global; i++){
        stats.avg_time_to_verify_transaction += message.block_timestamp - message.tx_timestamp[i];   
    }   
    stats.avg_time_to_verify_transaction /= num_transactions_per_block_global;
}

void statistics_process(){
    // Print the statistics when SIGUSR1 is received
    signal(SIGUSR1, print_statistics);
    // Receive messages from validator process through the message queue
    MessageToStatistics message;
    message.tx_timestamp = calloc(num_transactions_per_block_global, sizeof(time_t));

    while(1){
        // Receive a message from the message queue
        receive_message(msqid, &message);
        // Calculate the statistics
        update_statistics(message);
    }

    // Free the message
    free(message.tx_timestamp);
}

void create_statistics_process(){
    pid_t statistics_id = fork();
    if(statistics_id == 0){
        // Child process
        #if DEBUG
        debug_message("Creating statistics process\n");
        #endif
        // Initialize the statistics
        initialize_statistics();
        statistics_process();
        exit(0);
    } else if (statistics_id < 0) {
        perror("Failed to create statistics process");
    } else {
        // Parent process
        statistics_pid = statistics_id;
        log_message("CONTROLLER: Created statistics process with PID %d\n", statistics_pid);
    }
}

void create_miner_process(int num_miners){
    pid_t miner_id = fork();
    if(miner_id == 0){
        #if DEBUG
        debug_message("Creating miner process\n");
        #endif
        // Child process
        miner_process(num_miners);
        if(wait_for_miner_threads() == 1){
            #if DEBUG
            log_message("Miner process completed\n");
            #endif
        }
        exit(0);
    } else if (miner_id < 0) {
        perror("Failed to create miner process");
    } else {
        // Parent process
        miner_process_pid = miner_id;
        log_message("CONTROLLER: Created miner process with PID %d\n", miner_process_pid);
    }
}

int create_message_queue(){
    // Create the message queue
    msqid = msgget(IPC_PRIVATE, 0777| IPC_CREAT);
    if(msqid == -1){
        perror("CONTROLLER: Failed to create message queue");
        exit(1);
    } else {
        log_message("CONTROLLER: Message queue created successfully with ID %d\n", msqid);
    }
    return msqid;
}

MessageToStatistics* prepare_message(int miner_id, int valid_block, int credits, 
                                  time_t block_timestamp, time_t tx_timestamp[]) {
    MessageToStatistics* message = malloc(sizeof(MessageToStatistics));
    
    message->mtype = (long)block_timestamp;
    message->miner_id = miner_id;
    message->valid_block = valid_block;
    message->credits = credits;
    message->block_timestamp = block_timestamp;
    message->num_timestamps = num_transactions_per_block_global;
    
    // Allocate memory for timestamps
    message->tx_timestamp = malloc(num_transactions_per_block_global * sizeof(time_t));
    
    for(int i = 0; i < num_transactions_per_block_global; i++) {
        message->tx_timestamp[i] = tx_timestamp[i];
    }
    print_message(message);
    return message;
}

void print_message(MessageToStatistics* message){
    log_message("Message: %ld\n", message->mtype);
    log_message("Miner ID: %d\n", message->miner_id);
    log_message("Valid block: %d\n", message->valid_block);
    log_message("Credits: %d\n", message->credits);
    log_message("Block timestamp: %ld\n", message->block_timestamp);
    for(int i = 0; i < num_transactions_per_block_global; i++){
        log_message("Transaction timestamp: %ld\n", message->tx_timestamp[i]);
    }
}

void send_message(int msqid, MessageToStatistics *message) {
    // Send the message struct first
    if(msgsnd(msqid, message, MSG_SIZE, 0) == -1) {
        perror("CONTROLLER: Failed to send message");
        return;
    }
    
    // Then immediately send the timestamp array in a separate message
    // with the same mtype for identification
    if(msgsnd(msqid, message->tx_timestamp, 
              message->num_timestamps * sizeof(time_t), 0) == -1) {
        perror("CONTROLLER: Failed to send timestamp array");
    }
}

void receive_message(int msqid, MessageToStatistics *message) {
    // Receive the message struct first
    if(msgrcv(msqid, message, MSG_SIZE, -1, 0) == -1) {
        perror("CONTROLLER: Failed to receive message");
        return;
    }
    
    // Allocate memory for the timestamp array
    message->tx_timestamp = malloc(message->num_timestamps * sizeof(time_t));
    
    // Receive the timestamp array in a separate message
    if(msgrcv(msqid, message->tx_timestamp, 
              message->num_timestamps * sizeof(time_t), message->mtype, 0) == -1) {
        perror("CONTROLLER: Failed to receive timestamp array");
        free(message->tx_timestamp);
        message->tx_timestamp = NULL;
    }
}

// Clean up function for freeing memory
void free_message(MessageToStatistics *message) {
    if(message) {
        if(message->tx_timestamp) {
            free(message->tx_timestamp);
        }
        free(message);
    }
}

void print_process_status() {
    log_message("\nCONTROLLER: Current Process Status:\n");
    log_message("Validator Process (PID: %d): ", validator_pid);
    if (kill(validator_pid, 0) == 0) {
        log_message("Running\n");
    } else {
        log_message("Not running\n");
    }
    
    log_message("Statistics Process (PID: %d): ", statistics_pid);
    if (kill(statistics_pid, 0) == 0) {
        log_message("Running\n");
    } else {
        log_message("Not running\n");
    }
    
    log_message("Miner Process (PID: %d): ", miner_process_pid);
    if (kill(miner_process_pid, 0) == 0) {
        log_message("Running\n");
    } else {
        log_message("Not running\n");
    }
    log_message("\n");
}

void terminate_processes() {
    log_message("CONTROLLER: Beginning process termination...\n");
    #if DEBUG
    print_process_status();  // Print status before termination
    #endif
    
    // Send SIGTERM to specific processes first
    if (validator_pid > 0) {
        log_message("CONTROLLER: Sending SIGTERM to validator (PID: %d)\n", validator_pid);
        kill(validator_pid, SIGTERM);
    }
    if (statistics_pid > 0) {
        log_message("CONTROLLER: Sending SIGTERM to statistics (PID: %d)\n", statistics_pid);
        kill(statistics_pid, SIGTERM);
    }
    if (miner_process_pid > 0) {
        // Wait for all miner threads to finish
        wait_for_miner_threads();
        log_message("CONTROLLER: Sending SIGTERM to miner process (PID: %d)\n", miner_process_pid);
        kill(miner_process_pid, SIGTERM);
    }
    
    // Wait for processes to terminate
    pid_t wpid;
    int status;
    int timeout = 5;  // 5 seconds timeout
    time_t start_time = time(NULL);
    
    while ((wpid = waitpid(-1, &status, WNOHANG)) >= 0) {
        if (wpid > 0) {
            if (WIFEXITED(status)) {
                log_message("CONTROLLER: Process %d terminated normally with status %d\n", 
                           wpid, WEXITSTATUS(status));
            } else if (WIFSIGNALED(status)) {
                log_message("CONTROLLER: Process %d killed by signal %d\n", 
                           wpid, WTERMSIG(status));
            }
        }
        
        // Check timeout
        if (time(NULL) - start_time > timeout) {
            log_message("CONTROLLER: Timeout waiting for processes to terminate. Forcing termination.\n");
            // Force kill any remaining processes
            if (kill(validator_pid, 0) == 0) kill(validator_pid, SIGKILL);
            if (kill(statistics_pid, 0) == 0) kill(statistics_pid, SIGKILL);
            if (kill(miner_process_pid, 0) == 0) kill(miner_process_pid, SIGKILL);
            break;
        }
        
        usleep(100000);  // Sleep for 100ms between checks
    }
    #if DEBUG
    print_process_status();  // Print final status
    log_message("CONTROLLER: Process termination completed\n");
    #endif
}

int main(int argc, char *argv[]) {
    // Initialize the logger
    logger_init("DEIChain_log.txt");
    
    log_message("CONTROLLER: DEIChain Controller starting up\n");
    
    // Read config and set up
    Config config = read_config();
    setup_shared_memory(config);
    create_message_queue();
    
    // Set up signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGUSR1, signal_handler);
    
    log_message("CONTROLLER: Signal handlers installed. Send SIGUSR1 to print statistics (kill -SIGUSR1 %d)\n", getpid());
    
    // Create threads and processes
    create_miner_process(config.num_miners);
    create_validator_process();
    create_statistics_process();
    
    // Main loop - wait for termination signal
    while (running_miner_threads) {
        // Check if we need to print statistics
        if (print_stats_flag) {
            print_statistics();  
            print_stats_flag = 0;  // Reset the flag
        }
        
        sleep(1);
    }
    
    log_message("CONTROLLER: Beginning shutdown sequence...\n");
    
    // Terminate the miner process
    

    // First terminate all processes
    terminate_processes();
    
    // Then cleanup shared memory
    cleanup_shared_memory();
    
    // Finally cleanup message queue
    cleanup_message_queue();

    log_message("CONTROLLER: All resources cleaned up, exiting\n");
    
    // Close the logger
    logger_close();
    
    return 0;
}

