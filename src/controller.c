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
#define BLOCKCHAIN_LEDGER_SEM "/blockchain_ledger_sem"
#define AGE_MULTIPLIER 50

#define min(a, b) ((a) < (b) ? (a) : (b))

volatile sig_atomic_t running_miner_threads = 1;
volatile sig_atomic_t running_validator_process = 1;
volatile sig_atomic_t running_statistics_process = 1;
volatile sig_atomic_t running = 1;
volatile sig_atomic_t print_stats_flag = 0;


pthread_t *miner_threads = NULL;
pthread_mutex_t pipe_mutex = PTHREAD_MUTEX_INITIALIZER;
int *thread_ids = NULL;
int num_miners_global = 0;
int num_transactions_per_block_global = 0;
int msqid = -1;

int tx_pool_shmid = -1;
int blockchain_shmid = -1;
TransactionPool *transaction_pool = NULL;
BlockchainLedger *blockchain_ledger = NULL;
sem_t *tx_pool_sem = NULL;
sem_t *blockchain_ledger_sem = NULL;

pid_t validator_pid = -1;
pid_t statistics_pid = -1;
pid_t miner_process_pid = -1;

pthread_mutex_t validator_message_mutex = PTHREAD_MUTEX_INITIALIZER;

volatile sig_atomic_t shutdown_in_progress = 0;
pid_t main_process_pid = 0;

int max_blocks_global = 0;

// Add this new global variable at the top with other globals
volatile sig_atomic_t miner_shutdown_requested = 0;

// Add this with other global variables at the top
pthread_mutex_t block_id_mutex = PTHREAD_MUTEX_INITIALIZER;
volatile int next_block_id = 1;  // Start from 1

// Add at the top with other global variables
volatile sig_atomic_t print_stats_requested = 0;

void signal_handler(int signum) {
    if (getpid() != main_process_pid) {
        exit(0);
    }

    if (signum == SIGINT || signum == SIGTERM) {
        // Prevent re-entry by checking the global flag
        if (shutdown_in_progress) {
            return;  // Silent return to prevent multiple log messages
        }
        
        // Set the global flag first thing
        shutdown_in_progress = 1;
        
        log_message("CONTROLLER: Received signal %d, initiating ordered shutdown...\n", signum);
        
        // First send SIGUSR1 to print statistics
        if (statistics_pid > 0) {
            kill(statistics_pid, SIGUSR1);
            // Give it enough time to print statistics
            sleep(2);  // Wait 2 seconds for statistics to print
        }
        
        // Then use the ordered shutdown handler for SIGINT/SIGTERM
        controller_sigusr2_handler(SIGUSR2);
        
        // Instead of cleaning up here, set running to 0 to trigger main's cleanup
        running = 0;
        
    } else if (signum == SIGUSR1) {
        log_message("CONTROLLER: Received SIGUSR1, printing statistics...\n");
        if (statistics_pid > 0) {
            kill(statistics_pid, SIGUSR1);
        }
    }
}

// Modify the miner signal handler to be minimal
void miner_signal_handler(int signum) {
    if (signum == SIGUSR2) {
        log_message("MINER: Received shutdown signal\n");
        running_miner_threads = 0;  // Signal threads to finish current work
        miner_shutdown_requested = 1;  // Set shutdown flag
    }
}

// Add this new signal handler for the validator process
void validator_signal_handler(int signum) {
    if (signum == SIGUSR2) {
        log_message("VALIDATOR: Received graceful shutdown signal\n");
        running_validator_process = 0;  // Signal to finish current work
        
        // Cleanup and exit after current work is done
        log_message("VALIDATOR: Process shutting down gracefully\n");
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
        #if DEBUG
        debug_message("Invalid configuration values\n");
        #endif
        exit(1);
    }
    
    // Store the number of miners globally and the number of transactions per block
    num_miners_global = config.num_miners;
    num_transactions_per_block_global = config.transactions_per_block;


    #if DEBUG
    debug_message("Configuration read successfully\n");
    debug_message("NUM_MINERS = %d\n", config.num_miners);
    debug_message("TX_POOL_SIZE = %d\n", config.tx_pool_size);
    debug_message("TRANSACTIONS_PER_BLOCK = %d\n", config.transactions_per_block);
    debug_message("BLOCKCHAIN_BLOCKS = %d\n", config.blockchain_blocks);
    #endif

    return config;
}

void setup(Config config) {
    // Store max blocks globally
    max_blocks_global = config.blockchain_blocks;
    
    // Create and initialize the semaphore first
    // Unlink any existing semaphore
    sem_unlink(TX_POOL_SEM);
    tx_pool_sem = sem_open(TX_POOL_SEM, O_CREAT, 0644, config.tx_pool_size);
    if (tx_pool_sem == SEM_FAILED) {
        perror("CONTROLLER: Failed to create semaphore");
        exit(1);
    }

    // Create and initialize the blockchain ledger semaphore
    sem_unlink(BLOCKCHAIN_LEDGER_SEM);
    blockchain_ledger_sem = sem_open(BLOCKCHAIN_LEDGER_SEM, O_CREAT, 0644, 1);
    if (blockchain_ledger_sem == SEM_FAILED) {
        perror("CONTROLLER: Failed to create blockchain ledger semaphore");
        exit(1);
    }

    #if DEBUG
    debug_message("CONTROLLER: Transaction pool semaphore created with count %d\n", config.tx_pool_size);
    debug_message("CONTROLLER: Blockchain ledger semaphore created with count %d\n", 1);
    #endif

    // Size of pool and ledger
    size_t pool_size = sizeof(TransactionPool) + config.tx_pool_size * sizeof(TransactionEntry);
    
    // Calculate total size needed for blockchain ledger
    // Each block needs space for its transactions array
    size_t block_size = sizeof(Block) + config.transactions_per_block * sizeof(Transaction);
    size_t ledger_size = sizeof(BlockchainLedger) + config.blockchain_blocks * block_size;

    #if DEBUG
    debug_message("CONTROLLER: Allocating blockchain ledger with size %zu bytes\n", ledger_size);
    debug_message("CONTROLLER: Each block requires %zu bytes\n", block_size);
    #endif

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

    #if DEBUG
    debug_message("Transaction pool shared memory ID: %d\n", tx_pool_shmid);
    debug_message("Blockchain ledger shared memory ID: %d\n", blockchain_shmid);
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

    #if DEBUG
    debug_message("Transaction pool attached at address: %p\n", transaction_pool);
    debug_message("Blockchain ledger attached at address: %p\n", blockchain_ledger);
    #endif

    // Initialize the transaction pool
    transaction_pool->size = config.tx_pool_size;
    transaction_pool->transactions_pending = 0;
    transaction_pool->num_transactions_per_block = config.transactions_per_block;
    transaction_pool->next_tx_id = 1;  // Start transaction IDs from 1
    
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

    // Initialize all transaction pool entries
    for(int i = 0; i < config.tx_pool_size; i++) {
        transaction_pool->entries[i].empty = 1;
        transaction_pool->entries[i].age = 0;
        memset(&transaction_pool->entries[i].t, 0, sizeof(Transaction));
    }

    // Initialize the blockchain ledger
    sem_wait(blockchain_ledger_sem);
    blockchain_ledger->num_blocks = 0;
    
    // Initialize all blocks in the ledger to zero
    for (int i = 0; i < config.blockchain_blocks; i++) {
        Block* block = &blockchain_ledger->blocks[i];
        memset(block, 0, block_size);
        // Initialize transactions array for each block
        for (int j = 0; j < config.transactions_per_block; j++) {
            memset(&block->transactions[j], 0, sizeof(Transaction));
        }
    }
    
    // Set initial hash for first block
    strncpy(blockchain_ledger->blocks[0].prev_hash, INITIAL_HASH, HASH_SIZE);
    sem_post(blockchain_ledger_sem);

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

    // Initialize the ledger log file
    initialize_ledger_log();
}

void cleanup_shared_memory() {
    #if DEBUG
    debug_message("CONTROLLER: Cleaning up shared memory...\n");
    #endif
    
    // Detach from shared memory segments
    if (transaction_pool != NULL && transaction_pool != (TransactionPool *)-1) {
        if (shmdt(transaction_pool) == -1) {
            perror("Failed to detach transaction pool shared memory");
        } else {
            #if DEBUG
            debug_message("Detached from transaction pool shared memory\n");
            #endif
        }
        transaction_pool = NULL;
    }
    
    if (blockchain_ledger != NULL && blockchain_ledger != (BlockchainLedger *)-1) {
        if (shmdt(blockchain_ledger) == -1) {
            perror("Failed to detach blockchain ledger shared memory");
        } else {
            #if DEBUG
            debug_message("Detached from blockchain ledger shared memory\n");
            #endif
        }
        blockchain_ledger = NULL;
    }
    
    // Remove shared memory segments
    if (tx_pool_shmid != -1) {
        if (shmctl(tx_pool_shmid, IPC_RMID, NULL) == -1) {
            perror("Failed to remove transaction pool shared memory");
        } else {
            #if DEBUG
            debug_message("Removed transaction pool shared memory\n");
            #endif
            tx_pool_shmid = -1;
        }
    }
    
    if (blockchain_shmid != -1) {
        if (shmctl(blockchain_shmid, IPC_RMID, NULL) == -1) {
            perror("Failed to remove blockchain ledger shared memory");
        } else {
            #if DEBUG
            debug_message("Removed blockchain ledger shared memory\n");
            #endif
            blockchain_shmid = -1;
        }
    }
}

void cleanup_semaphores() {
    #if DEBUG
    debug_message("CONTROLLER: Cleaning up semaphores...\n");
    #endif
    
    if (tx_pool_sem != NULL && tx_pool_sem != SEM_FAILED) {
        if (sem_close(tx_pool_sem) == -1) {
            perror("Failed to close transaction pool semaphore");
        } else {
            #if DEBUG
            debug_message("Closed transaction pool semaphore\n");
            #endif
        }
        
        if (sem_unlink(TX_POOL_SEM) == -1) {
            perror("Failed to unlink transaction pool semaphore");
        } else {
            #if DEBUG
            debug_message("Unlinked transaction pool semaphore\n");
            #endif
        }
        tx_pool_sem = NULL;
    }
    if (blockchain_ledger_sem != NULL && blockchain_ledger_sem != SEM_FAILED) {
        if (sem_close(blockchain_ledger_sem) == -1) {
            perror("Failed to close blockchain ledger semaphore");
        } else {
            #if DEBUG
            debug_message("Closed blockchain ledger semaphore\n");
            #endif
        }
        
        if (sem_unlink(BLOCKCHAIN_LEDGER_SEM) == -1) {
            perror("Failed to unlink blockchain ledger semaphore");
        } else {
            #if DEBUG
            debug_message("Unlinked blockchain ledger semaphore\n");
            #endif
        }
        blockchain_ledger_sem = NULL;
    }
}

void cleanup_named_pipes() {
    #if DEBUG
    debug_message("CONTROLLER: Cleaning up named pipes...\n");
    #endif
    
    // Close and unlink the validator pipe
    if (unlink(VALIDATOR_PIPE_NAME) == -1) {
        if (errno != ENOENT) { // Don't report error if pipe doesn't exist
            perror("Failed to unlink validator pipe");
        }
    } else {
        #if DEBUG
        debug_message("Unlinked validator pipe\n");
        #endif
    }
}

void cleanup_message_queues() {
    #if DEBUG
    debug_message("CONTROLLER: Cleaning up message queues...\n");
    #endif
    
    if (msqid != -1) {
        if (msgctl(msqid, IPC_RMID, NULL) == -1) {
            perror("Failed to remove message queue");
        } else {
            #if DEBUG
            debug_message("Removed message queue\n");
            #endif
            msqid = -1;
        }
    }
}

void cleanup_mutexes() {
    #if DEBUG
    debug_message("CONTROLLER: Cleaning up mutexes...\n");
    #endif
    
    // Destroy the pipe mutex
    if (pthread_mutex_destroy(&pipe_mutex) != 0) {
        perror("Failed to destroy pipe mutex");
    } else {
        #if DEBUG
        debug_message("Destroyed pipe mutex\n");
        #endif
    }
    
    // Destroy the validator message mutex
    if (pthread_mutex_destroy(&validator_message_mutex) != 0) {
        perror("Failed to destroy validator message mutex");
    } else {
        #if DEBUG
        debug_message("Destroyed validator message mutex\n");
        #endif
    }
    
    // Destroy the block ID mutex
    if (pthread_mutex_destroy(&block_id_mutex) != 0) {
        perror("Failed to destroy block ID mutex");
    } else {
        #if DEBUG
        debug_message("Destroyed block ID mutex\n");
        #endif
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
    
    // First signal all processes to stop
    running_miner_threads = 0;
    running_validator_process = 0;
    running_statistics_process = 0;
    
    // Then cleanup all IPC mechanisms in order
    cleanup_message_queues();  // Message queues first as they might be in use
    cleanup_named_pipes();     // Named pipes next
    cleanup_semaphores();      // Semaphores before shared memory
    cleanup_shared_memory();   // Shared memory last as other cleanups might need it
    cleanup_mutexes();         // Clean up synchronization primitives
    
    // Remove the ledger file
    if (remove("blockchain_ledger.txt") == 0) {
        log_message("CONTROLLER: Removed blockchain ledger file\n");
    }
    
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
    pthread_mutex_lock(&block_id_mutex);
    block->txb_id = next_block_id++;
    pthread_mutex_unlock(&block_id_mutex);
    
    block->txb_timestamp = time(NULL);
    block->nonce = 0;

    sem_wait(blockchain_ledger_sem);
    // Copy the initial hash for the first block, or the previous block's hash otherwise
    if (blockchain_ledger->num_blocks == 0) {
        strncpy(block->prev_hash, INITIAL_HASH, HASH_SIZE);
    } else {
        Block* prev_block = &blockchain_ledger->blocks[blockchain_ledger->num_blocks - 1];
        char prev_hash[HASH_SIZE];
        compute_sha256(prev_block, prev_hash, num_transactions_per_block_global);
        strncpy(block->prev_hash, prev_hash, HASH_SIZE);
    }
    sem_post(blockchain_ledger_sem);

    // Copy transactions and validate them
    #if DEBUG
    debug_message("MINER: Copying %d transactions to new block\n", num_transactions_per_block_global);
    #endif
    for (int i = 0; i < num_transactions_per_block_global; i++) {
        block->transactions[i] = selected_tx[i];
        #if DEBUG
        debug_message("MINER: Transaction %d - ID: %d, Reward: %d\n", 
                   i, selected_tx[i].tx_id, selected_tx[i].reward);
        #endif
    }

    return block;
}

// Thread function
void *miner_thread(void *arg) {
    int id = *((int*)arg);
    log_message("MINER: Thread %d started\n", id);
    int printin = 1;
    const int MAX_ERRORS = 3;  // Maximum number of consecutive errors before shutdown
    int error_count = 0;

    // Adjust sleep based on mining success/failure to avoid spamming the validator
    int sleep_duration = 1;
    while (running_miner_threads && error_count < MAX_ERRORS) {
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
            error_count = 0;  // Reset error count on successful transaction selection
            
            // Create a new block
            Block* block = create_block(id, selected_tx);
            if (!block) {
                free(selected_tx);
                error_count++;
                log_message("MINER %d: Failed to create block, error count: %d/%d\n", 
                          id, error_count, MAX_ERRORS);
                continue;
            }

            log_message("MINER: Thread %d mining block %d\n", id, block->txb_id);
            // Try to mine the block
            PoWResult result = proof_of_work(block, num_transactions_per_block_global);
            
            if (!result.error) {
                sem_wait(blockchain_ledger_sem);
                log_message("MINER %d: Successfully mined block %d\n", 
                          id, blockchain_ledger->num_blocks + 1);
                sem_post(blockchain_ledger_sem);

                #if DEBUG
                debug_message("MINER %d: Sending block to validator\n", id);
                #endif
                send_block(block, id);
            } else {
                sem_wait(blockchain_ledger_sem);
                log_message("MINER %d: Failed to mine block %d\n", id, blockchain_ledger->num_blocks + 1);
                sem_post(blockchain_ledger_sem);
                error_count++;
                log_message("MINER %d: Mining failed, error count: %d/%d\n", 
                          id, error_count, MAX_ERRORS);
            }

            free(block);
            free(selected_tx);

            if (result.error) {
                sleep_duration = min(sleep_duration * 2, MAX_SLEEP);
            } else {
                sleep_duration = 1;
                error_count = 0;  // Reset error count on successful mining
            }
            
            if (running_miner_threads) {
                sleep(sleep_duration);
            }
        } else if(printin == 1) {
            printin = 0;
            log_message("MINER %d: Not enough transactions in pool, waiting...\n", id);
            error_count++;
            log_message("MINER %d: Failed to get transactions, error count: %d/%d\n", 
                      id, error_count, MAX_ERRORS);
        }
    }
    
    if (error_count >= MAX_ERRORS) {
        log_message("MINER %d: Thread shutting down due to too many errors (%d)\n", id, error_count);
    } else {
        log_message("MINER %d: Thread shutting down cleanly\n", id);
    }
    return NULL;
}

void send_block(Block* block, int miner_id) {
    // During shutdown, allow a few retries for sending the block
    const int MAX_RETRIES = 3;
    int retry_count = 0;

    if (!block) {
        log_message("MINER %d: Null block pointer in send_block\n", miner_id);
        return;
    }

    // Calculate size for the complete block with transactions
    size_t block_with_transactions_size = sizeof(Block) + num_transactions_per_block_global * sizeof(Transaction);
    size_t total_message_size = sizeof(int) + block_with_transactions_size;
    void* message_buffer = malloc(total_message_size);
    
    if (!message_buffer) {
        log_message("MINER %d: Failed to allocate memory for message buffer\n", miner_id);
        return;
    }
    
    // Initialize buffer
    memset(message_buffer, 0, total_message_size);
    
    // First part: miner_id
    int* miner_id_ptr = (int*)message_buffer;
    *miner_id_ptr = miner_id;
    
    // Second part: block with transactions
    void* block_ptr = (void*)(miner_id_ptr + 1);
    memcpy(block_ptr, block, sizeof(Block));
    Transaction* dest_tx = (Transaction*)((char*)block_ptr + sizeof(Block));
    
    for (int i = 0; i < num_transactions_per_block_global; i++) {
        memcpy(&dest_tx[i], &block->transactions[i], sizeof(Transaction));
    }

    while (retry_count < MAX_RETRIES) {
        // Open the named pipe
        int fd = open(VALIDATOR_PIPE_NAME, O_WRONLY | O_NONBLOCK);
        if (fd == -1) {
            if (errno == ENXIO && !running_miner_threads) {
                // Pipe is not being read and we're shutting down
                log_message("MINER %d: Validator not reading pipe during shutdown, attempt %d/%d\n", 
                          miner_id, retry_count + 1, MAX_RETRIES);
            } else {
                log_message("MINER %d: Failed to open pipe: %s, attempt %d/%d\n", 
                          miner_id, strerror(errno), retry_count + 1, MAX_RETRIES);
            }
            retry_count++;
            if (retry_count < MAX_RETRIES) {
                sleep(1);  // Wait a bit before retrying
                continue;
            }
            break;
        }

        pthread_mutex_lock(&pipe_mutex);
        if (write(fd, message_buffer, total_message_size) == -1) {
            pthread_mutex_unlock(&pipe_mutex);
            close(fd);
            log_message("MINER %d: Failed to write to pipe: %s, attempt %d/%d\n", 
                      miner_id, strerror(errno), retry_count + 1, MAX_RETRIES);
            retry_count++;
            if (retry_count < MAX_RETRIES) {
                sleep(1);
                continue;
            }
            break;
        }
        pthread_mutex_unlock(&pipe_mutex);
        close(fd);
        
        log_message("MINER %d: Successfully sent block after %d attempts\n", miner_id, retry_count + 1);
        free(message_buffer);
        return;
    }

    log_message("MINER %d: Failed to send block after %d attempts, giving up\n", miner_id, MAX_RETRIES);
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
                #if DEBUG
                debug_message("VALIDATOR: Copied transaction %d - ID: %d, Reward: %d\n",
                          i, out_message->block.transactions[i].tx_id, 
                          out_message->block.transactions[i].reward);
                #endif
            }
        }
        
        #if DEBUG
        debug_message("VALIDATOR: Successfully received block %d from miner %d with nonce %d\n", 
                   out_message->block.txb_id, out_message->miner_id, out_message->block.nonce);
        #endif
        
        // Compute the hash directly to confirm
        char hash[HASH_SIZE];
        compute_sha256(&out_message->block, hash, num_transactions_per_block_global);
        #if DEBUG
        debug_message("VALIDATOR: Block hash after receive: %s\n", hash);
        #endif
        
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

    #if DEBUG
    log_message("MINER: Found %d available transactions\n", available_transactions);
    #endif

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

        // Copy the transaction and validate its values
        Transaction* src_tx = &transaction_pool->entries[pool_idx].t;
        selected_transactions[i] = *src_tx;
        
        #if DEBUG
        debug_message("MINER: Copied transaction %d - ID: %d, Reward: %d, Value: %d\n",
                   i, selected_transactions[i].tx_id, selected_transactions[i].reward, selected_transactions[i].value);
        #endif
        
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

// Modify the miner_process function to handle the shutdown
void miner_process(int num_miners) {
    // Set up signal handler first
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = miner_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGUSR2, &sa, NULL) == -1) {
        log_message("MINER: Failed to set up signal handler\n");
        return;
    }
    
    log_message("MINER: Signal handler installed\n");
    
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
    
    if (!miner_threads || !thread_ids) {
        log_message("MINER: Failed to allocate memory for threads\n");
        if (miner_threads) free(miner_threads);
        if (thread_ids) free(thread_ids);
        sem_close(tx_pool_sem);
        shmdt(transaction_pool);
        return;
    }
    
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
    
    // Use a condition variable to wait for shutdown signal
    pthread_mutex_t shutdown_mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t shutdown_cond = PTHREAD_COND_INITIALIZER;
    
    pthread_mutex_lock(&shutdown_mutex);
    while (!miner_shutdown_requested) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 1; // Wait for 1 second at a time
        pthread_cond_timedwait(&shutdown_cond, &shutdown_mutex, &ts);
    }
    pthread_mutex_unlock(&shutdown_mutex);
    
    log_message("MINER: Beginning shutdown sequence\n");
    
    // Once shutdown is requested, wake up any waiting threads
    if (transaction_pool != NULL) {
        pthread_mutex_lock(&transaction_pool->mutex);
        pthread_cond_broadcast(&transaction_pool->enough_tx);
        pthread_mutex_unlock(&transaction_pool->mutex);
    }
    
    // Wait for threads to complete
    for (int i = 0; i < num_miners; i++) {
        log_message("MINER: Waiting for thread %d to complete\n", i + 1);
        if (pthread_join(miner_threads[i], NULL) == 0) {
            log_message("MINER: Thread %d completed successfully\n", i + 1);
        } else {
            log_message("MINER: Error joining thread %d\n", i + 1);
        }
    }
    
    // Cleanup
    pthread_mutex_destroy(&shutdown_mutex);
    pthread_cond_destroy(&shutdown_cond);
    free(miner_threads);
    free(thread_ids);
    if (tx_pool_sem != NULL) {
        sem_close(tx_pool_sem);
    }
    if (transaction_pool != NULL) {
        shmdt(transaction_pool);
    }
    
    log_message("MINER: Process shutting down\n");
}

void create_validator_process(){
    pid_t validator_id = fork();
    if(validator_id == 0) {
        // Install signal handler in child process
        signal(SIGUSR2, validator_signal_handler);
        
        #if DEBUG
        debug_message("Creating validator process\n");
        #endif
        validator_process();
        exit(0);
    } else if (validator_id < 0) {
        perror("Failed to create validator process");
    } else {
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

// Function to age transactions in the pool and update rewards
void age_transactions_in_pool() {
    pthread_mutex_lock(&transaction_pool->mutex);
    
    for (int i = 0; i < transaction_pool->size; i++) {
        if (!transaction_pool->entries[i].empty) {
            // Increment age
            transaction_pool->entries[i].age++;
            
            // Check if we need to increase reward (age mod 50 == 0)
            if (transaction_pool->entries[i].age % AGE_MULTIPLIER == 0) {
                transaction_pool->entries[i].t.reward++;
                #if DEBUG
                debug_message("VALIDATOR: Increased reward for transaction %d (age: %d, new reward: %d)\n",
                          transaction_pool->entries[i].t.tx_id,
                          transaction_pool->entries[i].age,
                          transaction_pool->entries[i].t.reward);
                #endif
            }
        }
    }
    
    pthread_mutex_unlock(&transaction_pool->mutex);
}

// Function to validate a block
int validate_block(Block* block, int miner_id) {
    if (!block) {
        #if DEBUG
        debug_message("VALIDATOR: Invalid block pointer\n");
        #endif
        return 0;
    }

    #if DEBUG
    debug_message("VALIDATOR: Starting validation of block from miner %d\n", miner_id);
    #endif

    // Age all transactions in the pool before validation
    age_transactions_in_pool();

    // 1. Recheck the block's PoW
    #if DEBUG
    debug_message("VALIDATOR: Starting PoW verification for block %d with nonce %d\n", 
                block->txb_id, block->nonce);
    #endif

    // Log block details before verification
    char current_hash[HASH_SIZE];
    compute_sha256(block, current_hash, num_transactions_per_block_global);
    #if DEBUG
    debug_message("VALIDATOR: Block hash before verification: %s (nonce: %d)\n", current_hash, block->nonce);
    #endif
    
    
    #if DEBUG
    int max_reward = get_max_transaction_reward(block, num_transactions_per_block_global);
    debug_message("VALIDATOR: Max reward in block: %d\n", max_reward);
    #endif

    if (!verify_nonce(block, num_transactions_per_block_global)) {
        #if DEBUG
        debug_message("VALIDATOR: Invalid proof of work - verification failed for nonce %d\n", block->nonce);
        #endif
        return 0;
    }
    
    #if DEBUG
    debug_message("VALIDATOR: Proof of work verification successful with nonce %d\n", block->nonce);
    #endif

    sem_wait(blockchain_ledger_sem);
    // 2. Check that it correctly references the latest accepted block
    if (blockchain_ledger->num_blocks > 0) {
        Block* prev_block = &blockchain_ledger->blocks[blockchain_ledger->num_blocks - 1];
        char expected_prev_hash[HASH_SIZE];
        compute_sha256(prev_block, expected_prev_hash, num_transactions_per_block_global);
        #if DEBUG
        debug_message("VALIDATOR: Checking block reference - Current: %s, Expected: %s\n",
                   block->prev_hash, expected_prev_hash);
        #endif
        
        if (strcmp(block->prev_hash, expected_prev_hash) != 0) {
            log_message("VALIDATOR: Block doesn't reference latest blockchain block\n");
            sem_post(blockchain_ledger_sem);
            return 0;
        }
    } else if (strcmp(block->prev_hash, INITIAL_HASH) != 0) {
        log_message("VALIDATOR: First block doesn't have correct initial hash\n");
        sem_post(blockchain_ledger_sem);
        return 0;
    }
    sem_post(blockchain_ledger_sem);

    // 3. Check that the transactions are still in the pool
    #if DEBUG
    debug_message("VALIDATOR: Verifying transactions are still in pool\n");
    #endif
    pthread_mutex_lock(&transaction_pool->mutex);
    for (int i = 0; i < num_transactions_per_block_global; i++) {
        Transaction* tx = &block->transactions[i];
        #if DEBUG
        debug_message("VALIDATOR: Checking transaction %d - ID: %d, Reward: %d, Value: %d\n",
                   i+1, tx->tx_id, tx->reward, tx->value);
        #endif
        
        if (!is_transaction_in_pool(tx)) {
            log_message("VALIDATOR: Transaction %d no longer in pool\n", tx->tx_id);
            pthread_mutex_unlock(&transaction_pool->mutex);
            return 0;
        }
    }
    pthread_mutex_unlock(&transaction_pool->mutex);

    #if DEBUG
    debug_message("DEBUG: Block %d hash: %s\n", block->txb_id, current_hash);
    #endif
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
    
    log_message("VALIDATOR: Process started, waiting for blocks...\n");
    
    while(running_validator_process) {
        // Check if we should stop before processing more blocks
        if (!running_validator_process) {
            break;
        }

        pthread_mutex_lock(&validator_message_mutex);
        // Receive the block from the miner
        if(receive_block(message)) {
            // Check if we should stop before processing this block
            if (!running_validator_process) {
                pthread_mutex_unlock(&validator_message_mutex);
                break;
            }

            log_message("VALIDATOR: Received block from miner %d\n", message->miner_id);
            
            // Make a complete copy of the message including all transactions
            memcpy(processing_message, message, message_with_block_size);
            pthread_mutex_unlock(&validator_message_mutex);

            #if DEBUG
            debug_message("VALIDATOR: Processing block %d with nonce %d\n", 
                       processing_message->block.txb_id, processing_message->block.nonce);
            // Log all transactions in the processing message for debug
            for (int i = 0; i < num_transactions_per_block_global; i++) {
                Transaction* tx = &processing_message->block.transactions[i];
                
                debug_message("VALIDATOR: Transaction %d - ID: %d, Reward: %d, Value: %d\n",
                          i+1, tx->tx_id, tx->reward, tx->value);
            }
            #endif

            // Validate the block
            if (validate_block(&processing_message->block, processing_message->miner_id)) {
                log_message("VALIDATOR: Block %d from %d validated successfully\n", processing_message->block.txb_id, processing_message->miner_id);
                
                // Remove validated transactions from pool
                #if DEBUG
                debug_message("VALIDATOR: Removing validated transactions from pool\n");
                #endif
                remove_validated_transactions(&processing_message->block);
                
                // Add block to blockchain
                #if DEBUG
                debug_message("VALIDATOR: Adding block %d to blockchain\n", processing_message->block.txb_id);
                #endif
                add_block_to_blockchain(&processing_message->block);
                
                #if DEBUG
                debug_message("VALIDATOR: Block %d processing complete\n", processing_message->block.txb_id);
                #endif

                // Send message to statistics process for valid block
                MessageToStatistics* message2;
                message2 = prepare_message(processing_message->miner_id, 1, 
                                        calculate_miner_credits(&processing_message->block), 
                                        processing_message->block.txb_timestamp, 
                                        processing_message->block.transactions);
                send_message(msqid, message2);
                free(message2);
            } else {    
                log_message("VALIDATOR: Block %d from %d validation failed\n", processing_message->block.txb_id, processing_message->miner_id);
                
                // Send message to statistics process for invalid block
                MessageToStatistics* message2;
                message2 = prepare_message(processing_message->miner_id, 0, 
                                        0, // No credits for invalid block
                                        processing_message->block.txb_timestamp, 
                                        processing_message->block.transactions);
                send_message(msqid, message2);
                free(message2);
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

void add_block_to_blockchain(Block* block) {
    if (!block) {
        log_message("VALIDATOR: Invalid block pointer in add_block_to_blockchain\n");
        return;
    }

    #if DEBUG
    debug_message("VALIDATOR: Starting to add block %d to blockchain\n", block->txb_id);
    debug_message("VALIDATOR: Block details before adding - Nonce: %d\n Hash: %s\n", block->nonce, block->prev_hash);
    #endif
    
    // Log transactions before adding to blockchain
    #if DEBUG
    debug_message("VALIDATOR: Transactions in block before adding to blockchain:\n");
    for (int i = 0; i < num_transactions_per_block_global; i++) {
        Transaction* tx = &block->transactions[i];
        debug_message("VALIDATOR: Transaction %d - ID: %d, Reward: %d, Value: %d\n",
                   i+1, tx->tx_id, tx->reward, tx->value);
    }
    #endif

    // Wait for semaphore to access shared memory
    if (sem_wait(blockchain_ledger_sem) == -1) {
        perror("VALIDATOR: Failed to acquire blockchain semaphore");
        return;
    }

    // Check if we've reached the maximum number of blocks
    if (blockchain_ledger->num_blocks >= max_blocks_global) {
        log_message("VALIDATOR: Maximum number of blocks reached (%d). Initiating shutdown.\n", max_blocks_global);
        sem_post(blockchain_ledger_sem);
        running_validator_process = 0;
        return;
    }

    #if DEBUG
    debug_message("VALIDATOR: Acquired blockchain semaphore, current block count: %d\n", 
                blockchain_ledger->num_blocks);
    #endif

    // Get pointer to the destination block in the ledger
    Block* dest_block = &blockchain_ledger->blocks[blockchain_ledger->num_blocks];
    
    // Copy the block header (everything except the transactions array)
    memcpy(dest_block, block, sizeof(Block));
    
    // Copy each transaction individually
    for (int i = 0; i < num_transactions_per_block_global; i++) {
        memcpy(&dest_block->transactions[i], &block->transactions[i], sizeof(Transaction));
        #if DEBUG
        debug_message("VALIDATOR: Copied transaction %d - ID: %d, Reward: %d, Value: %d\n",
                   i+1, dest_block->transactions[i].tx_id, 
                   dest_block->transactions[i].reward,
                   dest_block->transactions[i].value);
        #endif
    }

    // Increment block count after successful copy
    blockchain_ledger->num_blocks++;

    // Append the new block to the ledger log file
    append_block_to_ledger_log(dest_block);

    log_message("VALIDATOR: Successfully added block %d to blockchain\n", 
                block->txb_id);

    // Check if we've reached the maximum number of blocks
    if (blockchain_ledger->num_blocks >= max_blocks_global) {
        log_message("VALIDATOR: Maximum number of blocks reached (%d). Initiating shutdown.\n", max_blocks_global);
        running_validator_process = 0;
    }
    // Release semaphore
    if (sem_post(blockchain_ledger_sem) == -1) {
        perror("VALIDATOR: Failed to release blockchain semaphore");
    }
}

void initialize_ledger_log() {
    // Remove existing ledger file if it exists
    remove("blockchain_ledger.txt");
    
    // Create a new ledger file
    FILE* ledger_file = fopen("blockchain_ledger.txt", "w");
    if (ledger_file) {
        fprintf(ledger_file, "=================== Blockchain Ledger ===================\n");
        fclose(ledger_file);
        log_message("CONTROLLER: Created new blockchain ledger file\n");
    } else {
        log_message("CONTROLLER: Failed to create blockchain ledger file\n");
    }
}

void append_block_to_ledger_log(Block* block) {
    FILE* ledger_file = fopen("blockchain_ledger.txt", "a");
    if (!ledger_file) {
        log_message("Error: Failed to open ledger log file\n");
        return;
    }

    char block_time[26];
    char tx_time[26];
    
    // Convert block timestamp to readable format
    struct tm* block_tm = localtime(&block->txb_timestamp);
    if (block_tm == NULL) {
        strcpy(block_time, "Invalid timestamp");
    } else {
        strftime(block_time, sizeof(block_time), "%Y-%m-%d %H:%M:%S", block_tm);
    }
    
    fprintf(ledger_file, "||----  Block %03d ----||  \n", blockchain_ledger->num_blocks);
    fprintf(ledger_file, "Block ID: BLOCK-%d \n", block->txb_id);
    fprintf(ledger_file, "Previous Hash:\n%s\n", block->prev_hash);
    fprintf(ledger_file, "Block Timestamp: %s \n", block_time);
    fprintf(ledger_file, "Nonce: %d \n", block->nonce);
    fprintf(ledger_file, "Transactions: \n");
    
    for (int j = 0; j < num_transactions_per_block_global; j++) {
        Transaction* tx = &block->transactions[j];
        if (tx == NULL) {
            fprintf(ledger_file, "  [%d] Invalid transaction\n", j+1);
            continue;
        }

        // Convert transaction timestamp to readable format
        struct tm* tx_tm = localtime(&tx->tx_timestamp);
        if (tx_tm == NULL) {
            strcpy(tx_time, "Invalid timestamp");
        } else {
            strftime(tx_time, sizeof(tx_time), "%Y-%m-%d %H:%M:%S", tx_tm);
        }

        fprintf(ledger_file, "  [%d] ID: TX-%d | Reward: %d | Value: %d | Timestamp: %s \n",
                j+1, tx->tx_id, tx->reward, tx->value, tx_time);
    }
    fprintf(ledger_file, "||-----------------------------------------|| \n");
    
    fclose(ledger_file);
}

void dump_ledger() {
    // Read and print the contents of the ledger log file
    FILE* ledger_file = fopen("blockchain_ledger.txt", "r");
    if (!ledger_file) {
        log_message("Error: Failed to open ledger log file\n");
        return;
    }

    char line[256];
    while (fgets(line, sizeof(line), ledger_file)) {
        log_message("%s", line);
    }
    
    fclose(ledger_file);
}

void initialize_statistics(Statistics *stats){
    if (!stats) return;

    stats->num_valid_blocks = calloc(num_miners_global, sizeof(int));
    stats->num_invalid_blocks = calloc(num_miners_global, sizeof(int));
    stats->credits_of_each_miner = calloc(num_miners_global, sizeof(int));
    
    if (!stats->num_valid_blocks || !stats->num_invalid_blocks || !stats->credits_of_each_miner) {
        log_message("STATISTICS: Failed to allocate memory for statistics arrays\n");
        if (stats->num_valid_blocks) free(stats->num_valid_blocks);
        if (stats->num_invalid_blocks) free(stats->num_invalid_blocks);
        if (stats->credits_of_each_miner) free(stats->credits_of_each_miner);
        exit(1);
    }

    stats->avg_time_to_verify_transaction = 0;
    stats->total_number_of_blocks_validated = 0;
    stats->total_number_of_blocks_in_the_blockchain = 0;
    stats->tx_timestamp = NULL;

    #if DEBUG
    debug_message("STATISTICS: Initialized statistics for %d miners\n", num_miners_global);
    #endif
}

void free_statistics(Statistics *stats){
    if (!stats) return;

    if (stats->num_valid_blocks) {
        free(stats->num_valid_blocks);
        stats->num_valid_blocks = NULL;
    }
    if (stats->num_invalid_blocks) {
        free(stats->num_invalid_blocks);
        stats->num_invalid_blocks = NULL;
    }
    if (stats->credits_of_each_miner) {
        free(stats->credits_of_each_miner);
        stats->credits_of_each_miner = NULL;
    }
}

void update_statistics(Statistics *stats, MessageToStatistics *message){
    if (!stats || !message) return;

    if(message->valid_block == 1){
        stats->num_valid_blocks[message->miner_id - 1]++;
        stats->total_number_of_blocks_in_the_blockchain++;
        stats->credits_of_each_miner[message->miner_id - 1] += message->credits;
    } else {
        stats->num_invalid_blocks[message->miner_id - 1]++;
    }
    stats->total_number_of_blocks_validated++;

    for(int i = 0; i < message->num_timestamps; i++){
        stats->avg_time_to_verify_transaction += message->block_timestamp - message->tx_timestamp[i];   
    }   
    stats->avg_time_to_verify_transaction /= message->num_timestamps;
}

void print_statistics(Statistics *stats){
    if (!stats || !stats->num_valid_blocks || !stats->num_invalid_blocks || !stats->credits_of_each_miner) {
        log_message("STATISTICS: Error - Statistics not properly initialized\n");
        return;
    }

    char buffer[4096]; 
    char *ptr = buffer;
    int remaining_buffer_size = sizeof(buffer);
    int written;

    written = snprintf(ptr, remaining_buffer_size, "STATISTICS: SIGUSR1 RECEIVED\n");
    ptr += written;
    remaining_buffer_size -= written;

    written = snprintf(ptr, remaining_buffer_size, "====================== MINER STATISTICS ======================\n");
    ptr += written;
    remaining_buffer_size -= written;
    
    for(int i = 0; i < num_miners_global; i++){
        written = snprintf(ptr, remaining_buffer_size, "STATISTICS: Miner %d:\n", i + 1);
        ptr += written;
        remaining_buffer_size -= written;

        written = snprintf(ptr, remaining_buffer_size, "  - Valid blocks: %d\n", stats->num_valid_blocks[i]);    
        ptr += written;
        remaining_buffer_size -= written;

        written = snprintf(ptr, remaining_buffer_size, "  - Invalid blocks: %d\n", stats->num_invalid_blocks[i]);
        ptr += written;
        remaining_buffer_size -= written;

        written = snprintf(ptr, remaining_buffer_size, "  - Credits: %d\n", stats->credits_of_each_miner[i]);
        ptr += written;
        remaining_buffer_size -= written;
    }

    if (stats->total_number_of_blocks_validated > 0) {
        written = snprintf(ptr, remaining_buffer_size, "STATISTICS: Average time to verify a transaction: %.2f seconds\n", 
                   stats->avg_time_to_verify_transaction);
        ptr += written;
        remaining_buffer_size -= written;
    } else {
        written = snprintf(ptr, remaining_buffer_size, "STATISTICS: No blocks validated yet\n");
        ptr += written;
        remaining_buffer_size -= written;
    }

    written = snprintf(ptr, remaining_buffer_size, "===============================================================\n");
    ptr += written;
    remaining_buffer_size -= written;

    written = snprintf(ptr, remaining_buffer_size, "====================== GLOBAL STATISTICS ======================\n");
    ptr += written;
    remaining_buffer_size -= written;

    written = snprintf(ptr, remaining_buffer_size, "STATISTICS: Total blocks validated (valid + invalid): %d\n", 
                stats->total_number_of_blocks_validated);
    ptr += written;
    remaining_buffer_size -= written;

    written = snprintf(ptr, remaining_buffer_size, "STATISTICS: Total blocks in blockchain: %d\n", 
                stats->total_number_of_blocks_in_the_blockchain);
    ptr += written;
    remaining_buffer_size -= written;
                
    written = snprintf(ptr, remaining_buffer_size, "=================================================================\n");
    // ptr += written; // Not needed for the last write, as we don't use ptr further
    // remaining_buffer_size -= written;

    log_message("%s", buffer);
}

// Create a signal handler context structure to hold the stats pointer
typedef struct {
    Statistics *stats;
} SignalContext;

// Global signal context for the statistics process
static SignalContext signal_ctx;

void statistics_signal_handler(int signum) {
    if (signum == SIGUSR1) {
        // For direct signal, print immediately
        if (signal_ctx.stats) {
            print_statistics(signal_ctx.stats);
        }
    } else if (signum == SIGUSR2) {
        log_message("STATISTICS: Received graceful shutdown signal\n");
        // Print one final time before shutting down
        if (signal_ctx.stats) {
            print_statistics(signal_ctx.stats);
        }
        running_statistics_process = 0;
    }
}

void statistics_process(){
    log_message("STATISTICS: Process starting up\n");

    // Create local statistics structure
    Statistics local_stats = {0};  // Initialize to zero
    initialize_statistics(&local_stats);

    // Set up signal context to access stats in signal handler
    signal_ctx.stats = &local_stats;

    // Set up signal handlers
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = statistics_signal_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, NULL);
    sigaction(SIGUSR2, &sa, NULL);

    // Allocate message with space for timestamps
    size_t msg_size = sizeof(MessageToStatistics) + 
                     num_transactions_per_block_global * sizeof(time_t);
    MessageToStatistics *message = malloc(msg_size);
    
    if (!message) {
        log_message("STATISTICS: Failed to allocate memory for message\n");
        free_statistics(&local_stats);
        return;
    }

    debug_message("STATISTICS: Entering main processing loop\n");
    while(running_statistics_process) {
        // Check if statistics print was requested
        if (print_stats_requested) {
            print_statistics(&local_stats);
            print_stats_requested = 0;  // Reset the flag
        }

        // Non-blocking receive to allow checking print flag
        if (msgrcv(msqid, message, msg_size - sizeof(long), 0, IPC_NOWAIT) != -1) {
            log_message("STATISTICS: Processing message from miner %d (valid=%d)\n", 
                       message->miner_id, message->valid_block);
            update_statistics(&local_stats, message);
            log_message("STATISTICS: Successfully updated statistics\n");
        }

        // Small sleep to prevent busy waiting
        usleep(100000);  // 100ms sleep
    }

    // Print final statistics before shutting down
    print_statistics(&local_stats);
    
    // Cleanup
    free(message);
    free_statistics(&local_stats);
    log_message("STATISTICS: Process shutting down\n");
}

void create_statistics_process() {
    pid_t statistics_id = fork();
    
    if(statistics_id == 0) {
        // The signal handlers will be set up in statistics_process()
        #if DEBUG
        debug_message("Creating statistics process\n");
        #endif
        
        statistics_process();
        exit(0);
    } else if (statistics_id < 0) {
        perror("Failed to create statistics process");
    } else {
        statistics_pid = statistics_id;
        log_message("CONTROLLER: Created statistics process with PID %d\n", statistics_pid);
        log_message("CONTROLLER: Send SIGUSR1 to print statistics(kill -SIGUSR1 %d)\n", statistics_pid);
    }
}

void create_miner_process(int num_miners) {
    pid_t miner_id = fork();
    if(miner_id == 0) {
        // Install signal handler in child process
        signal(SIGUSR2, miner_signal_handler);
        
        #if DEBUG
        debug_message("Creating miner process\n");
        #endif
        // Child process
        miner_process(num_miners);
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
                                   time_t block_timestamp, Transaction* transactions) {
    // Calculate total size needed for the message including the timestamp array
    size_t msg_size = sizeof(MessageToStatistics) + num_transactions_per_block_global * sizeof(time_t);
    
    MessageToStatistics* message = malloc(msg_size);
    if (!message) {
        log_message("VALIDATOR: Failed to allocate memory for message\n");
        return NULL;
    }

    // Initialize the message
    message->mtype = 1;  // Use a constant message type
    message->miner_id = miner_id;
    message->valid_block = valid_block;
    message->credits = credits;
    message->block_timestamp = block_timestamp;
    message->num_timestamps = num_transactions_per_block_global;

    // Copy timestamps directly into the flexible array member
    for(int i = 0; i < num_transactions_per_block_global; i++) {
        message->tx_timestamp[i] = transactions[i].tx_timestamp;
    }

    return message;
}

void print_message(MessageToStatistics* message){
    // Allocate a buffer for the base message plus space for timestamps
    // Each timestamp might need ~40 chars ("STATISTICS: Transaction XX timestamp: XXXXXXXXXX\n")
    char* buffer = malloc(1024 + message->num_timestamps * 64);
    char* current = buffer;
    
    // Write the header portion
    current += sprintf(current, 
        "STATISTICS: ========= MESSAGE DETAILS =========\n"
        "STATISTICS: Message Type: %ld\n"
        "STATISTICS: Miner ID: %d\n"
        "STATISTICS: Valid block: %s\n"
        "STATISTICS: Credits: %d\n"
        "STATISTICS: Block timestamp: %ld\n"
        "STATISTICS: Number of transaction timestamps: %d\n",
        message->mtype,
        message->miner_id,
        message->valid_block ? "Yes" : "No",
        message->credits,
        message->block_timestamp,
        message->num_timestamps);
    
    // Add each timestamp
    for(int i = 0; i < message->num_timestamps; i++){
        current += sprintf(current, "STATISTICS: Transaction %d timestamp: %ld\n", 
                         i+1, message->tx_timestamp[i]);
    }
    
    // Add the footer
    sprintf(current, "STATISTICS: ================================\n");
    
    // Log the complete message
    log_message("%s", buffer);
    
    // Clean up
    free(buffer);
}

void send_message(int msqid, MessageToStatistics *message) {
    if (!message) {
        log_message("VALIDATOR: Cannot send null message\n");
        return;
    }

    // Calculate total message size including timestamps
    size_t total_size = sizeof(MessageToStatistics) + 
                       message->num_timestamps * sizeof(time_t);

    #if DEBUG
    debug_message("VALIDATOR: Sending message to queue %d (size: %zu bytes)\n", 
                 msqid, total_size);
    #endif

    // Send the entire message in one go
    if(msgsnd(msqid, message, total_size - sizeof(long), 0) == -1) {
        perror("VALIDATOR: Failed to send message");
        return;
    }

    #if DEBUG
    debug_message("VALIDATOR: Successfully sent message from miner %d\n", 
                 message->miner_id);
    #endif
}

void receive_message(int msqid, MessageToStatistics *message) {
    if (!message) {
        log_message("STATISTICS: Cannot receive into null message\n");
        return;
    }

    // Calculate maximum possible message size
    size_t max_msg_size = sizeof(MessageToStatistics) + 
                         num_transactions_per_block_global * sizeof(time_t);

    #if DEBUG
    debug_message("STATISTICS: Waiting for message (max size: %zu bytes)\n", 
                 max_msg_size);
    #endif

    // Receive the entire message in one go
    ssize_t bytes_received = msgrcv(msqid, message, max_msg_size - sizeof(long), 0, 0);
    
    if(bytes_received == -1) {
        perror("STATISTICS: Failed to receive message");
        return;
    }

    #if DEBUG
    debug_message("STATISTICS: Received message from miner %d (%zd bytes)\n", 
                 message->miner_id, bytes_received);
    #endif

    log_message("STATISTICS: Message received from miner %d\n", message->miner_id);
    #if DEBUG
    print_message(message);
    #endif
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
                #if DEBUG
                debug_message("Removing transaction: ID=%d, Value=%d, Reward=%d\n",
                          validated_tx->tx_id,
                          validated_tx->value,
                          validated_tx->reward);
                #endif
                
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
    #if DEBUG
    debug_message("Removed %d/%d transactions from pool for block %d\n", 
                removed_count, num_transactions_per_block_global, block->txb_id);
    #endif
    
    if (removed_count < num_transactions_per_block_global) {
        #if DEBUG
        debug_message("Warning: Could not find all transactions in pool for block %d. Some transactions might have been removed by another process.\n", 
                   block->txb_id);
        #endif
    }
}

void controller_sigusr2_handler(int signum) {
    if (signum != SIGUSR2 || getpid() != main_process_pid) {
        return;
    }

    log_message("CONTROLLER: Starting ordered shutdown sequence...\n");

    // Phase 1: Stop miners first and wait for all threads to finish
    if (miner_process_pid > 0) {
        log_message("CONTROLLER: Phase 1 - Stopping miner process...\n");
        // Call miner handler directly for the miner process
        miner_signal_handler(SIGUSR2);
        
        // Wait for miner process to finish
        int status;
        waitpid(miner_process_pid, &status, 0);
        if (WIFEXITED(status)) {
            log_message("CONTROLLER: Miner process exited with status %d\n", WEXITSTATUS(status));
        } else if (WIFSIGNALED(status)) {
            log_message("CONTROLLER: Miner process terminated by signal %d\n", WTERMSIG(status));
        }
        log_message("CONTROLLER: Phase 1 complete - Miners stopped\n");
    }

    // Phase 2: Stop validator and statistics
    if (validator_pid > 0) {
        log_message("CONTROLLER: Phase 2 - Stopping validator process...\n");
        // Call validator handler directly
        validator_signal_handler(SIGUSR2);
        
        // Wait for validator to finish
        int status;
        waitpid(validator_pid, &status, 0);
        if (WIFEXITED(status)) {
            log_message("CONTROLLER: Validator process exited with status %d\n", WEXITSTATUS(status));
        }
    }

    // Phase 3: Print final statistics and stop statistics process
    if (statistics_pid > 0) {
        log_message("CONTROLLER: Phase 3 - Requesting final statistics...\n");
        
        // First send SIGUSR1 to print stats
        kill(statistics_pid, SIGUSR1);
        
        // Give it a moment to print
        sleep(1);  // Wait 1 second
        
        log_message("CONTROLLER: Phase 3 - Stopping statistics process...\n");
        
        // Then send shutdown signal
        kill(statistics_pid, SIGUSR2);
        
        // Wait for statistics to finish
        int status;
        waitpid(statistics_pid, &status, 0);
        if (WIFEXITED(status)) {
            log_message("CONTROLLER: Statistics process exited with status %d\n", WEXITSTATUS(status));
        }
    }
    
    log_message("CONTROLLER: All processes stopped\n");

    // Signal main loop to exit
    running = 0;
}

int main(int argc, char *argv[]) {
    // Store the main process PID
    main_process_pid = getpid();
    
    // Initialize the logger
    logger_init("DEIChain_log.log");
    
    OpenSSL_add_all_algorithms();

    log_message("CONTROLLER: DEIChain Controller starting up\n");
    
    // Read config and set up
    Config config = read_config();
    setup(config);
    create_message_queue();
    
    // Set up signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGUSR1, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGUSR2, controller_sigusr2_handler);  // Add the ordered shutdown handler
    
    // Create threads and processes
    create_miner_process(config.num_miners);
    create_validator_process();
    create_statistics_process();
    
    // Main loop - wait for termination signal or max blocks
    while (running) {
        // Check if we've reached the maximum number of blocks
        sem_wait(blockchain_ledger_sem);
        if (blockchain_ledger->num_blocks >= max_blocks_global) {
            log_message("CONTROLLER: Maximum number of blocks reached (%d). Initiating ordered shutdown.\n", max_blocks_global);
            sem_post(blockchain_ledger_sem);
            
            // Use the ordered shutdown handler
            controller_sigusr2_handler(SIGUSR2);
            break;
        }
        sem_post(blockchain_ledger_sem);
        sleep(1);
    }

    // Dump the ledger before cleanup
    dump_ledger();
    
    // Single call to cleanup everything
    cleanup_all_resources();
    
    log_message("CONTROLLER: Shutdown complete\n");
    logger_close();
    
    return 0;
}


