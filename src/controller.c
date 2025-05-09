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
#include <time.h>
#include <openssl/conf.h>
#include <openssl/evp.h>
#include <openssl/crypto.h>
#include "common.h"
#include "logger.h"  // Include the logger header
#include "controller.h"
#include "pow.h"

#define CONFIG_FILE "config.cfg"
#define DEBUG 0        // Set to 1 to enable debug messages
#define MSG_SIZE (sizeof(MessageToStatistics) - sizeof(long))
#define VALIDATOR_PIPE_NAME "/tmp/validator_pipe"
#define MAX_SLEEP 10

#define min(a, b) ((a) < (b) ? (a) : (b))
// Booting the system, reading the configuration file, validating the data in the
// file, and applying the read configurations
// Creation of processes Miner, Validator, and Statistics 
// Setting up two shared memory segments (for Transaction Pool and Blockchain Ledger)
// Begin implementation for SIGINT signal capture (preliminary)

// Global flag for miner thread termination
volatile sig_atomic_t running_miner_threads = 1;
volatile sig_atomic_t running_validator_process = 1;
volatile sig_atomic_t print_stats_flag = 0;

// Global variables to manage threads
pthread_t *miner_threads = NULL;
pthread_mutex_t pipe_mutex = PTHREAD_MUTEX_INITIALIZER;
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

pthread_mutex_t validator_message_mutex = PTHREAD_MUTEX_INITIALIZER;

// Global flag to prevent multiple signal handler executions
volatile sig_atomic_t shutdown_in_progress = 0;
// Store main process PID for signal handling
pid_t main_process_pid = 0;

// Signal handler
void signal_handler(int signum) {
    // Only handle SIGINT in the main controller process
    if (signum == SIGINT) {
        // Check if we're the main controller process
        if (getpid() != main_process_pid) {
            // If not the main process, just exit
            exit(0);
        }
        
        // Prevent re-entry by checking the global flag
        if (shutdown_in_progress) {
            return;  // Silent return to prevent multiple log messages
        }
        
        // Set the global flag first thing
        shutdown_in_progress = 1;
        
        log_message("CONTROLLER: Received SIGINT, shutting down...\n");
        
        // Set global flags to signal threads to stop
        running_miner_threads = 0;
        
        // Wake up all waiting threads by modifying the condition they're waiting on
        if (transaction_pool) {
            pthread_mutex_lock(&transaction_pool->mutex);
            // Artificially set transactions_pending to trigger the wake-up condition
            transaction_pool->transactions_pending = transaction_pool->num_transactions_per_block;
            // Broadcast to wake up ALL waiting threads
            pthread_cond_broadcast(&transaction_pool->enough_tx);
            pthread_mutex_unlock(&transaction_pool->mutex);
        }

        // Wait for all miner threads to finish
        wait_for_miner_threads();

        running_validator_process = 0;
        // Explicitly initiate process termination
        terminate_processes();
        
        // Exit directly if SIGINT
        cleanup_all_resources();
        log_message("CONTROLLER: Emergency shutdown complete\n");
        logger_close();
        exit(0);  // Force exit
    } else if (signum == SIGUSR1) {
        // For SIGUSR1, handle only in main process
        if (getpid() != main_process_pid) {
            return;
        }
        
        // For SIGUSR1, we still allow multiple calls
        log_message("CONTROLLER: Received SIGUSR1, printing statistics...\n");
        print_stats_flag = 1;  // Set flag to print statistics
    } else {
        // For any other signal, handle like SIGINT but only in main process
        if (getpid() != main_process_pid) {
            exit(0);
        }
        
        if (shutdown_in_progress) {
            return;
        }
        
        // Set the global flag
        shutdown_in_progress = 1;
        
        log_message("CONTROLLER: Received signal %d, shutting down...\n", signum);
        running_miner_threads = 0;  // Signal threads to stop
        running_validator_process = 0;  // Signal validator process to stop

        // Wake up all waiting threads by modifying the condition they're waiting on
        if (transaction_pool) {
            pthread_mutex_lock(&transaction_pool->mutex);
            // Artificially set transactions_pending to trigger the wake-up condition
            transaction_pool->transactions_pending = transaction_pool->num_transactions_per_block;
            // Broadcast to wake up ALL waiting threads
            pthread_cond_broadcast(&transaction_pool->enough_tx);
            pthread_mutex_unlock(&transaction_pool->mutex);
        }
        
        // Explicitly initiate process termination
        terminate_processes();
        
        // Exit directly for other termination signals
        cleanup_all_resources();
        log_message("CONTROLLER: Emergency shutdown complete\n");
        logger_close();
        exit(0);  // Force exit
    }
}

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
void setup(Config config) {
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

    // Create the named pipe
    if(mkfifo(VALIDATOR_PIPE_NAME, 0666) == -1){
        if (errno != EEXIST) {  // Only error out if it's not because the pipe already exists
            perror("Failed to create named pipe");
            exit(1);
        }
        log_message("CONTROLLER: Named pipe already exists\n");
    } else {
        log_message("CONTROLLER: Created named pipe\n");
    }
}

// Function to clean up shared memory
void cleanup_shared_memory() {
    log_message("CONTROLLER: Cleaning up shared memory...\n");
    
    // Detach from shared memory segments
    if (transaction_pool != NULL && transaction_pool != (TransactionPool *)-1) {
        if (shmdt(transaction_pool) == -1) {
            perror("Failed to detach transaction pool shared memory");
        } else {
            #ifdef DEBUG
            log_message("Detached from transaction pool shared memory\n");
            #endif
        }
        transaction_pool = NULL;
    }
    
    if (blockchain_ledger != NULL && blockchain_ledger != (BlockchainLedger *)-1) {
        if (shmdt(blockchain_ledger) == -1) {
            perror("Failed to detach blockchain ledger shared memory");
        } else {
            #ifdef DEBUG
            log_message("Detached from blockchain ledger shared memory\n");
            #endif
        }
        blockchain_ledger = NULL;
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

void cleanup_semaphores() {
    log_message("CONTROLLER: Cleaning up semaphores...\n");
    
    if (tx_pool_sem != NULL && tx_pool_sem != SEM_FAILED) {
        if (sem_close(tx_pool_sem) == -1) {
            perror("Failed to close transaction pool semaphore");
        } else {
            log_message("Closed transaction pool semaphore\n");
        }
        
        if (sem_unlink(TX_POOL_SEM) == -1) {
            perror("Failed to unlink transaction pool semaphore");
        } else {
            log_message("Unlinked transaction pool semaphore\n");
        }
        tx_pool_sem = NULL;
    }
}

void cleanup_named_pipes() {
    log_message("CONTROLLER: Cleaning up named pipes...\n");
    
    // Close and unlink the validator pipe
    if (unlink(VALIDATOR_PIPE_NAME) == -1) {
        if (errno != ENOENT) { // Don't report error if pipe doesn't exist
            perror("Failed to unlink validator pipe");
        }
    } else {
        log_message("Unlinked validator pipe\n");
    }
}

void cleanup_message_queues() {
    log_message("CONTROLLER: Cleaning up message queues...\n");
    
    if (msqid != -1) {
        if (msgctl(msqid, IPC_RMID, NULL) == -1) {
            perror("Failed to remove message queue");
        } else {
            log_message("Removed message queue\n");
            msqid = -1;
        }
    }
}

void cleanup_mutexes() {
    log_message("CONTROLLER: Cleaning up mutexes...\n");
    
    // Destroy the pipe mutex
    if (pthread_mutex_destroy(&pipe_mutex) != 0) {
        perror("Failed to destroy pipe mutex");
    } else {
        log_message("Destroyed pipe mutex\n");
    }
    
    // Destroy the validator message mutex
    if (pthread_mutex_destroy(&validator_message_mutex) != 0) {
        perror("Failed to destroy validator message mutex");
    } else {
        log_message("Destroyed validator message mutex\n");
    }
    
    // Destroy transaction pool mutex and condition variable if they exist
    if (transaction_pool != NULL) {
        if (pthread_mutex_destroy(&transaction_pool->mutex) != 0) {
            perror("Failed to destroy transaction pool mutex");
        }
        if (pthread_cond_destroy(&transaction_pool->enough_tx) != 0) {
            perror("Failed to destroy transaction pool condition variable");
        }
    }
}

void cleanup_all_resources() {
    // Only do cleanup in the main controller process
    if (getpid() != main_process_pid) {
        return;
    }
    
    // Prevent multiple cleanups
    static int cleanup_done = 0;
    if (cleanup_done) {
        return;
    }
    cleanup_done = 1;
    
    log_message("CONTROLLER: Beginning cleanup of all resources...\n");
    
    // First terminate all processes (existing function)
    terminate_processes();
    
    // Then cleanup all IPC mechanisms in order
    cleanup_message_queues();  // Message queues first as they might be in use
    cleanup_named_pipes();     // Named pipes next
    cleanup_semaphores();      // Semaphores before shared memory
    cleanup_shared_memory();   // Shared memory last as other cleanups might need it
    cleanup_mutexes();         // Clean up synchronization primitives
    
    // Finally, free any remaining allocated memory
    free_statistics();
    
    log_message("CONTROLLER: All resources cleaned up\n");
}

// Function to create a block with transactions
Block* create_block(int miner_id, Transaction* selected_tx) {
    if (!selected_tx) {
        log_message("MINER: No transactions provided to create_block\n");
        return NULL;
    }

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

    // Copy transactions and validate them
    log_message("MINER: Copying %d transactions to new block\n", num_transactions_per_block_global);
    for (int i = 0; i < num_transactions_per_block_global; i++) {
        block->transactions[i] = selected_tx[i];
        log_message("MINER: Transaction %d - ID: %d, Reward: %d\n", 
                   i, selected_tx[i].tx_id, selected_tx[i].reward);
    }

    return block;
}

// Thread function
void *miner_thread(void *arg) {
    int id = *((int*)arg);
    log_message("MINER: Thread %d started\n", id);
    int printin = 1;

    // Adjust sleep based on mining success/failure to avoid spamming the validator
    int sleep_duration = 1;
    while (running_miner_threads) {
        pthread_mutex_lock(&transaction_pool->mutex);
        while (running_miner_threads && 
               transaction_pool->transactions_pending < transaction_pool->num_transactions_per_block) {
            pthread_cond_wait(&transaction_pool->enough_tx, &transaction_pool->mutex);
        }
        
        // Check if we were woken up for shutdown
        if (!running_miner_threads) {
            log_message("MINER: Thread %d woken up for shutdown\n", id);
            pthread_mutex_unlock(&transaction_pool->mutex);
            break;
        }
        pthread_mutex_unlock(&transaction_pool->mutex);

        // Try to get random transactions
        Transaction* selected_tx = get_random_transactions(num_transactions_per_block_global);
        if (selected_tx != NULL) {
            printin = 1;
            log_message("MINER: Thread %d selected %d transactions\n", id, num_transactions_per_block_global);
            
            // Check again if we should be shutting down
            if (!running_miner_threads) {
                log_message("MINER: Thread %d stopping after selecting transactions\n", id);
                free(selected_tx);
                break;
            }
            
            // Create a new block
            Block* block = create_block(id, selected_tx);
            if (!block) {
                free(selected_tx);
                continue;
            }

            // Check again if we should be shutting down
            if (!running_miner_threads) {
                log_message("MINER: Thread %d stopping after creating block\n", id);
                free(block);
                free(selected_tx);
                break;
            }

            // Try to mine the block
            PoWResult result = proof_of_work(block, num_transactions_per_block_global);
            
            // Check again if we should be shutting down
            if (!running_miner_threads) {
                log_message("MINER: Thread %d stopping after mining attempt\n", id);
                free(block);
                free(selected_tx);
                break;
            }
            
            if (!result.error) {
                log_message("MINER: Thread %d successfully mined block %d with hash %s (nonce: %d)\n", 
                          id, block->txb_id, result.hash, block->nonce);
                // TODO: Submit block to validator
                // Send via named pipe to validator process, send the block and miner id.
                send_block(block, id);
            } else {
                log_message("MINER: Thread %d failed to mine block %d\n", id, block->txb_id);
            }

            free(block);  // Free the block after we're done with it
            free(selected_tx);  // Free the selected transactions
            if (result.error) {
                sleep_duration = min(sleep_duration * 2, MAX_SLEEP); // Back off on failure
            } else {
                sleep_duration = 1; // Reset on success
            }
            
            // Only sleep if we're not shutting down
            if (running_miner_threads) {
                sleep(sleep_duration);
            }
        } else if(printin == 1){
            printin = 0;
            log_message("MINER: Thread %d - Not enough transactions in pool, waiting...\n", id);
        }
        
        // Check one more time if we should be shutting down
        if (!running_miner_threads) {
            log_message("MINER: Thread %d detected shutdown signal\n", id);
            break;
        }
    }
    
    log_message("MINER: Thread %d shutting down cleanly\n", id);
    return NULL;
}

void send_block(Block* block, int miner_id) {
    if (!block) {
        log_message("MINER: Null block pointer in send_block\n");
        return;
    }

    // We need to allocate and populate a complete message, 
    // including space for all the transactions
    
    // Calculate size for the complete block with all transactions
    size_t block_with_transactions_size = sizeof(Block) + num_transactions_per_block_global * sizeof(Transaction);
    
    // Allocate a buffer large enough for the miner_id plus the entire block with transactions
    size_t total_message_size = sizeof(int) + block_with_transactions_size;
    void* message_buffer = malloc(total_message_size);
    
    if (!message_buffer) {
        log_message("MINER: Failed to allocate memory for message buffer\n");
        return;
    }
    
    // Initialize buffer
    memset(message_buffer, 0, total_message_size);
    
    // First part: miner_id
    int* miner_id_ptr = (int*)message_buffer;
    *miner_id_ptr = miner_id;
    
    // Second part: block with transactions
    void* block_ptr = (void*)(miner_id_ptr + 1);
    
    // Copy the block header (everything up to but not including the flexible array)
    memcpy(block_ptr, block, sizeof(Block));
    
    // Copy the transactions right after the block header
    Transaction* dest_tx = (Transaction*)((char*)block_ptr + sizeof(Block));
    
    // Log the transactions being sent for debugging
    log_message("MINER: Sending block with %d transactions:\n", num_transactions_per_block_global);
    for (int i = 0; i < num_transactions_per_block_global; i++) {
        log_message("MINER: Transaction %d before copy - ID: %d, Reward: %d\n", 
                   i, block->transactions[i].tx_id, block->transactions[i].reward);
        
        memcpy(&dest_tx[i], &block->transactions[i], sizeof(Transaction));
    }
    
    // Open the named pipe
    int fd = open(VALIDATOR_PIPE_NAME, O_WRONLY);
    if (fd == -1) {
        perror("MINER: Failed to open named pipe");
        free(message_buffer);
        return;
    }
    
    pthread_mutex_lock(&pipe_mutex);
    
    // Send the complete message
    if (write(fd, message_buffer, total_message_size) == -1) {
        perror("MINER: Failed to send block to validator");
    } else {
        log_message("MINER: Successfully sent block with %d transactions to validator\n", 
                   num_transactions_per_block_global);
        
        // Log sent transactions
        for (int i = 0; i < num_transactions_per_block_global; i++) {
            log_message("MINER: Sent transaction %d - ID: %d, Reward: %d\n",
                       i, block->transactions[i].tx_id, block->transactions[i].reward);
        }
    }
    
    pthread_mutex_unlock(&pipe_mutex);
    close(fd);
    free(message_buffer);
}

int receive_block(MessageToValidator* out_message) {
    if (!out_message) {
        log_message("VALIDATOR: Null out_message pointer in receive_block\n");
        return 0;
    }
    
    int success = 0;
    
    // Calculate the expected message size
    size_t block_with_transactions_size = sizeof(Block) + num_transactions_per_block_global * sizeof(Transaction);
    size_t total_message_size = sizeof(int) + block_with_transactions_size;
    
    // Allocate a buffer large enough for the entire message
    void* message_buffer = malloc(total_message_size);
    if (!message_buffer) {
        log_message("VALIDATOR: Failed to allocate memory for message buffer\n");
        return 0;
    }
    
    // Initialize buffer
    memset(message_buffer, 0, total_message_size);
    
    // Open the named pipe
    int fd = open(VALIDATOR_PIPE_NAME, O_RDONLY);
    if (fd == -1) {
        perror("VALIDATOR: Failed to open named pipe");
        free(message_buffer);
        return 0;
    }

    // Read the complete message
    if (read(fd, message_buffer, total_message_size) == -1) {
        perror("VALIDATOR: Failed to receive block");
    } else {
        // First part: miner_id
        int* miner_id_ptr = (int*)message_buffer;
        out_message->miner_id = *miner_id_ptr;
        
        // Second part: block with transactions
        void* block_ptr = (void*)(miner_id_ptr + 1);
        
        // Copy the block header
        memcpy(&out_message->block, block_ptr, sizeof(Block));
        
        // Copy the transactions
        Transaction* src_tx = (Transaction*)((char*)block_ptr + sizeof(Block));
        for (int i = 0; i < num_transactions_per_block_global; i++) {
            memcpy(&out_message->block.transactions[i], &src_tx[i], sizeof(Transaction));
            
            // Verify the transaction was copied correctly
            if (i < 5) { // Only log first 5 to avoid spamming
                log_message("VALIDATOR: Copied transaction %d - ID: %d, Reward: %d\n",
                          i, out_message->block.transactions[i].tx_id, 
                          out_message->block.transactions[i].reward);
            }
        }
        
        log_message("VALIDATOR: Successfully received block %d from miner %d with nonce %d\n", 
                   out_message->block.txb_id, out_message->miner_id, out_message->block.nonce);
        
        // Compute the hash directly to confirm
        char hash[HASH_SIZE];
        compute_sha256(&out_message->block, hash, num_transactions_per_block_global);
        log_message("VALIDATOR: Block hash after receive: %s\n", hash);
        
        success = 1;
    }
    
    close(fd);
    free(message_buffer);
    return success;
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
    if (miner_threads == NULL) {
        log_message("CONTROLLER: No miner threads to wait for\n");
        return 0;
    }
    
    log_message("CONTROLLER: Waiting for %d miner threads to finish...\n", num_miners_global);
    
    // Print the status of each thread
    for (int i = 0; i < num_miners_global; i++) {
        log_message("CONTROLLER: Waiting for miner thread %d to complete\n", i + 1);
        
        // Join each thread with a timeout to avoid hanging
        struct timespec timeout;
        clock_gettime(CLOCK_REALTIME, &timeout);
        timeout.tv_sec += 3; // 3 second timeout
        
        int join_result = pthread_join(miner_threads[i], NULL);
        if (join_result == 0) {
            log_message("CONTROLLER: Miner thread %d completed successfully\n", i + 1);
        } else if (join_result == ETIMEDOUT) {
            log_message("CONTROLLER: Timeout waiting for miner thread %d, will proceed anyway\n", i + 1);
        } else {
            log_message("CONTROLLER: Error joining miner thread %d: %s\n", i + 1, strerror(join_result));
        }
    }
    
    // Free the memory allocated
    free(miner_threads);
    free(thread_ids);
    miner_threads = NULL;
    thread_ids = NULL;

    log_message("CONTROLLER: All miner threads have finished or timed out\n");
    return 1;
}

void create_validator_process(){
    // Create the Validator process
    pid_t validator_id = fork();
    if(validator_id == 0){
        #if DEBUG
        debug_message("Creating validator process\n");
        #endif
        
        // Set up signal handlers in child process
        signal(SIGINT, signal_handler);  // Will just exit for child processes
        signal(SIGTERM, signal_handler); // Will just exit for child processes
        
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

// Function to check if a transaction is still in the pool
int is_transaction_in_pool(Transaction* tx) {
    for (int i = 0; i < transaction_pool->size; i++) {
        if (!transaction_pool->entries[i].empty && 
            transaction_pool->entries[i].t.tx_id == tx->tx_id) {
            return 1;  // Found in pool
        }
    }
    return 0;  // Not found in pool
}

// Function to validate a block
int validate_block(Block* block) {
    if (!block) {
        log_message("VALIDATOR: Invalid block pointer\n");
        return 0;
    }

    log_message("VALIDATOR: Starting validation of block from miner %d\n", block->txb_id);

    // 1. Recheck the block's PoW
    log_message("VALIDATOR: Starting PoW verification for block %d with nonce %d\n", 
                block->txb_id, block->nonce);

    // Log block details before verification
    char current_hash[HASH_SIZE];
    compute_sha256(block, current_hash, num_transactions_per_block_global);
    log_message("VALIDATOR: Block hash before verification: %s (nonce: %d)\n", current_hash, block->nonce);
    
    int max_reward = get_max_transaction_reward(block, num_transactions_per_block_global);
    log_message("VALIDATOR: Max reward in block: %d\n", max_reward);

    if (!verify_nonce(block, num_transactions_per_block_global)) {
        log_message("VALIDATOR: Invalid proof of work - verification failed for nonce %d\n", block->nonce);
        return 0;
    }
    
    log_message("VALIDATOR: Proof of work verification successful with nonce %d\n", block->nonce);

    // 2. Check that it correctly references the latest accepted block
    if (blockchain_ledger->num_blocks > 0) {
        Block* prev_block = &blockchain_ledger->blocks[blockchain_ledger->num_blocks - 1];
        char expected_prev_hash[HASH_SIZE];
        compute_sha256(prev_block, expected_prev_hash, num_transactions_per_block_global);
        
        if (strcmp(block->prev_hash, expected_prev_hash) != 0) {
            log_message("VALIDATOR: Block doesn't reference latest blockchain block\n");
            return 0;
        }
    } else if (strcmp(block->prev_hash, INITIAL_HASH) != 0) {
        log_message("VALIDATOR: First block doesn't have correct initial hash\n");
        return 0;
    }

    // 3. Check that the transactions are still in the pool
    pthread_mutex_lock(&transaction_pool->mutex);
    for (int i = 0; i < num_transactions_per_block_global; i++) {
        if (!is_transaction_in_pool(&block->transactions[i])) {
            log_message("VALIDATOR: Transaction %d no longer in pool\n", 
                       block->transactions[i].tx_id);
            pthread_mutex_unlock(&transaction_pool->mutex);
            return 0;
        }
    }
    pthread_mutex_unlock(&transaction_pool->mutex);

    log_message("VALIDATOR: Block from miner %d is valid\n", block->txb_id);
    return 1;
}

// Helper function to calculate miner credits based on transaction rewards
int calculate_miner_credits(Block* block) {
    int total_credits = 0;
    for (int i = 0; i < num_transactions_per_block_global; i++) {
        total_credits += block->transactions[i].reward;
    }
    return total_credits;
}

void validator_process() {
    // Calculate size needed for the block with transactions
    size_t message_with_block_size = sizeof(MessageToValidator) + num_transactions_per_block_global * sizeof(Transaction);
    
    // Allocate memory for message and processing_message
    MessageToValidator *message = malloc(message_with_block_size);
    MessageToValidator *processing_message = malloc(message_with_block_size);
    
    if (!message || !processing_message) {
        log_message("VALIDATOR: Failed to allocate memory for messages\n");
        if (message) free(message);
        if (processing_message) free(processing_message);
        return;
    }
    
    // Initialize memory
    memset(message, 0, message_with_block_size);
    memset(processing_message, 0, message_with_block_size);
    
    while(running_validator_process) {
        pthread_mutex_lock(&validator_message_mutex);
        // Receive the block from the miner
        if(receive_block(message)) {
            // Make a complete copy of the message including all transactions
            memcpy(processing_message, message, message_with_block_size);
            pthread_mutex_unlock(&validator_message_mutex);

            log_message("VALIDATOR: Processing block %d with nonce %d\n", 
                       processing_message->block.txb_id, processing_message->block.nonce);
            
            // Log all transactions in the processing message for debug
            for (int i = 0; i < num_transactions_per_block_global; i++) {
                log_message("VALIDATOR: Processing transaction %d - ID: %d, Reward: %d\n",
                          i, processing_message->block.transactions[i].tx_id, 
                          processing_message->block.transactions[i].reward);
            }

            // Validate the block
            if (validate_block(&processing_message->block)) {
                log_message("VALIDATOR: Block validated successfully\n");
                // Calculate credits for the miner
                int credits = calculate_miner_credits(&processing_message->block);
                
                // Remove validated transactions from pool
                remove_validated_transactions(&processing_message->block);
                
                // Add block to blockchain
                //add_block_to_blockchain(&processing_message->block);
            } else {
                log_message("VALIDATOR: Block validation failed\n");
                // Send invalid block statistics
                //send_invalid_block_statistics(&processing_message->block);
            }
        } else {
            pthread_mutex_unlock(&validator_message_mutex);
        }
    }
    
    // Free allocated memory
    free(message);
    free(processing_message);
    
    log_message("VALIDATOR: Process shutting down\n");
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
        
        // Set up signal handlers in child process
        signal(SIGINT, signal_handler);  // Will just exit for child processes
        signal(SIGTERM, signal_handler); // Will just exit for child processes
        
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
        
        // Set up signal handlers in child process
        signal(SIGINT, signal_handler);  // Will just exit for child processes
        signal(SIGTERM, signal_handler); // Will just exit for child processes
        
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
    // Prevent multiple concurrent terminations
    static int termination_in_progress = 0;
    
    if (termination_in_progress) {
        return;  // Silent return to prevent multiple terminations
    }
    
    termination_in_progress = 1;  // Set the flag
    
    log_message("CONTROLLER: Beginning process termination...\n");
    
    #if DEBUG
    print_process_status();  // Print status before termination
    #endif
    
    // First send SIGTERM to all child processes
    int termination_sent = 0;
    
    if (validator_pid > 0 && kill(validator_pid, 0) == 0) {
        log_message("CONTROLLER: Sending SIGTERM to validator (PID: %d)\n", validator_pid);
        kill(validator_pid, SIGTERM);
        termination_sent = 1;
    }
    
    if (statistics_pid > 0 && kill(statistics_pid, 0) == 0) {
        log_message("CONTROLLER: Sending SIGTERM to statistics (PID: %d)\n", statistics_pid);
        kill(statistics_pid, SIGTERM);
        termination_sent = 1;
    }
    
    if (miner_process_pid > 0 && kill(miner_process_pid, 0) == 0) {
        // First try to wait for all miner threads to finish
        wait_for_miner_threads();
        log_message("CONTROLLER: Sending SIGTERM to miner process (PID: %d)\n", miner_process_pid);
        kill(miner_process_pid, SIGTERM);
        termination_sent = 1;
    }
    
    // If no processes to terminate, return early
    if (!termination_sent) {
        log_message("CONTROLLER: No active processes to terminate\n");
        termination_in_progress = 0;  // Reset the flag before returning
        return;
    }
    
    // Wait for processes to terminate with timeout
    int timeout = 5;  // 5 seconds timeout
    time_t start_time = time(NULL);
    pid_t wpid;
    int status;
    int remaining_processes = 0;
    
    // Wait a bit for processes to terminate on their own
    do {
        remaining_processes = 0;
        
        if (validator_pid > 0 && kill(validator_pid, 0) == 0) 
            remaining_processes++;
            
        if (statistics_pid > 0 && kill(statistics_pid, 0) == 0) 
            remaining_processes++;
            
        if (miner_process_pid > 0 && kill(miner_process_pid, 0) == 0) 
            remaining_processes++;
        
        // Check for any terminated children
        while ((wpid = waitpid(-1, &status, WNOHANG)) > 0) {
            if (WIFEXITED(status)) {
                log_message("CONTROLLER: Process %d terminated normally with status %d\n", 
                           wpid, WEXITSTATUS(status));
            } else if (WIFSIGNALED(status)) {
                log_message("CONTROLLER: Process %d killed by signal %d\n", 
                           wpid, WTERMSIG(status));
            }
            
            // Update PIDs if this process terminated
            if (wpid == validator_pid) validator_pid = -1;
            if (wpid == statistics_pid) statistics_pid = -1;
            if (wpid == miner_process_pid) miner_process_pid = -1;
        }
        
        // If no more processes or timeout reached, exit the loop
        if (remaining_processes == 0 || time(NULL) - start_time > timeout)
            break;
            
        usleep(100000);  // Sleep for 100ms between checks
    } while (1);
    
    // If timeout reached and processes still exist, force kill them
    if (remaining_processes > 0) {
        log_message("CONTROLLER: Timeout waiting for processes to terminate. Forcing termination.\n");
        
        if (validator_pid > 0 && kill(validator_pid, 0) == 0) {
            log_message("CONTROLLER: Force killing validator (PID: %d)\n", validator_pid);
            kill(validator_pid, SIGKILL);
        }
        
        if (statistics_pid > 0 && kill(statistics_pid, 0) == 0) {
            log_message("CONTROLLER: Force killing statistics (PID: %d)\n", statistics_pid);
            kill(statistics_pid, SIGKILL);
        }
        
        if (miner_process_pid > 0 && kill(miner_process_pid, 0) == 0) {
            log_message("CONTROLLER: Force killing miner process (PID: %d)\n", miner_process_pid);
            kill(miner_process_pid, SIGKILL);
        }
        
        // Wait one final time for any killed processes
        usleep(500000);  // 500ms wait
        while (waitpid(-1, NULL, WNOHANG) > 0);
    }
    
    #if DEBUG
    print_process_status();  // Print final status
    #endif
    
    log_message("CONTROLLER: Process termination completed\n");
    termination_in_progress = 0;  // Reset the flag at the end
}

// Function to remove transactions from pool after block validation
void remove_validated_transactions(Block* block) {
    if (!block) return;

    pthread_mutex_lock(&transaction_pool->mutex);
    int removed_count = 0;
    
    // For each transaction in the validated block
    for (int i = 0; i < num_transactions_per_block_global; i++) {
        Transaction* validated_tx = &block->transactions[i];
        
        // Search for this transaction in the pool by tx_id
        for (int j = 0; j < transaction_pool->size; j++) {
            if (!transaction_pool->entries[j].empty && 
                transaction_pool->entries[j].t.tx_id == validated_tx->tx_id) {
                
                // Log the transaction being removed
                log_message("Removing transaction: ID=%d, Value=%.2f, Reward=%d\n",
                          validated_tx->tx_id,
                          validated_tx->value,
                          validated_tx->reward);
                
                // Mark the entry as empty
                transaction_pool->entries[j].empty = 1;
                transaction_pool->entries[j].age = 0;
                transaction_pool->transactions_pending--;
                removed_count++;
                
                // Release a slot in the semaphore
                if (sem_post(tx_pool_sem) == -1) {
                    log_message("Error releasing semaphore slot: %s\n", strerror(errno));
                }
                
                break; // Found and removed this transaction, move to next
            }
        }
    }
    
    pthread_mutex_unlock(&transaction_pool->mutex);
    
    // Log summary
    log_message("Removed %d/%d transactions from pool for block %d\n", 
                removed_count, num_transactions_per_block_global, block->txb_id);
    
    if (removed_count < num_transactions_per_block_global) {
        log_message("Warning: Could not find all transactions in pool for block %d. Some transactions might have been removed by another process.\n", 
                   block->txb_id);
    }
}

int main(int argc, char *argv[]) {
    // Store the main process PID
    main_process_pid = getpid();
    
    // Initialize the logger
    logger_init("DEIChain_log.txt");
    
    OpenSSL_add_all_algorithms();

    log_message("CONTROLLER: DEIChain Controller starting up\n");
    
    // Read config and set up
    Config config = read_config();
    setup(config);
    create_message_queue();
    
    // Set up signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGUSR1, signal_handler);
    signal(SIGTERM, signal_handler);  // Also handle SIGTERM for clean shutdown
    
    log_message("CONTROLLER: Signal handlers installed. Send SIGUSR1 to print statistics (kill -SIGUSR1 %d)\n", getpid());
    
    // Create threads and processes
    create_miner_process(config.num_miners);
    create_validator_process();
    create_statistics_process();
    
    // Main loop - wait for termination signal
    int shutdown_attempts = 0;
    while (running_miner_threads) {
        // Check if we need to print statistics
        if (print_stats_flag) {
            print_statistics();  
            print_stats_flag = 0;  // Reset the flag
        }
        
        // Check if child processes are still running
        if (miner_process_pid > 0 && kill(miner_process_pid, 0) != 0) {
            log_message("CONTROLLER: Miner process ended unexpectedly, initiating shutdown\n");
            running_miner_threads = 0;  // Signal to exit the loop
        }
        
        if (validator_pid > 0 && kill(validator_pid, 0) != 0) {
            log_message("CONTROLLER: Validator process ended unexpectedly, initiating shutdown\n");
            running_miner_threads = 0;  // Signal to exit the loop
        }
        
        // Periodic status check for debugging
        shutdown_attempts++;
        if (shutdown_attempts % 10 == 0 && !running_miner_threads) {
            log_message("CONTROLLER: Waiting for processes to terminate... (attempt %d)\n", shutdown_attempts);
            terminate_processes();  // Try terminating processes again
        }
        
        sleep(1);
    }
    
    log_message("CONTROLLER: Beginning shutdown sequence...\n");
    
    // Single call to cleanup everything
    cleanup_all_resources();
    
    log_message("CONTROLLER: Shutdown complete\n");
    logger_close();
    
    return 0;
}

