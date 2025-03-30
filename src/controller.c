#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <pthread.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <semaphore.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/errno.h>

#include "common.h"
#include "logger.h"  // Include the logger header

#define CONFIG_FILE "config.cfg"
#define DEBUG 1        // Set to 1 to enable debug messages
#define CONSOLE_DEBUG 1 // Set to 1 to print debug messages to console
#define LOG_DEBUG 1    // Set to 1 to write debug messages to log file

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

// Global variables for shared memory
int tx_pool_shmid = -1;
int blockchain_shmid = -1;
TransactionPool *transaction_pool = NULL;
BlockchainLedger *blockchain_ledger = NULL;

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
    // Size of pool and ledger
    size_t pool_size = sizeof(TransactionPool) + config.tx_pool_size * sizeof(TransactionEntry);
    size_t ledger_size = sizeof(BlockchainLedger);

    // Create the shared memory segments
    tx_pool_shmid = shmget(IPC_PRIVATE, pool_size, IPC_CREAT | 0664);
    blockchain_shmid = shmget(IPC_PRIVATE, ledger_size, IPC_CREAT | 0664);

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
    //...

    // Initialize the blockchain ledger
    //...
}

// Function to clean up shared memory
void cleanup_shared_memory() {
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

// Thread function
void *miner_thread(void *arg) {
    int id = *((int*)arg);
    log_message("Miner thread %d started\n", id);
    
    // Thread main loop - just sleep until termination
    while (running_miner_threads) {
        sleep(1);
    }
    
    log_message("Miner thread %d shutting down\n", id);
    return NULL;
}

void print_system_statistics(){
    log_message("===DEIChain Statistics===\n");
    log_message("Transaction Pool Status:\n");
    log_message("Miner Statistics:\n");
    log_message("Validator Statistics:\n");
    log_message("Blockchain Statistics:\n");
}
// Signal handler
void signal_handler(int signum) {
    if (signum == SIGINT) {
        log_message("\nReceived SIGINT, shutting down...\n");
        running_miner_threads = 0;  // Set flag to terminate threads
    } else if (signum == SIGUSR1) {
        log_message("\nReceived SIGUSR1, printing statistics...\n");
        print_stats_flag = 1;  // Set flag to print statistics
    }
}

// Create miner threads but don't wait for them
void create_miner_threads(int num_miners) {
    // Store the number of miners globally
    num_miners_global = num_miners;
    
    // Allocate memory for thread IDs and handles
    miner_threads = (pthread_t *)malloc(num_miners * sizeof(pthread_t));
    thread_ids = (int *)malloc(num_miners * sizeof(int));
    
    // Create the threads
    for (int i = 0; i < num_miners; i++) {
        thread_ids[i] = i + 1;  // Store thread ID
        
        debug_message("Creating miner thread %d\n", i + 1);
        
        // Create the Miner threads
        pthread_create(&miner_threads[i], NULL, miner_thread, &thread_ids[i]);
    }
    
    // Return immediately without waiting
    log_message("All miner threads created\n");
}

// Function to wait for miner threads to complete (call this at the end)
int wait_for_miner_threads() {
    if (miner_threads == NULL) return 0;
    
    log_message("Waiting for all miner threads to finish...\n");
    
    // Wait for all Miner threads to complete
    for (int i = 0; i < num_miners_global; i++) {
        debug_message("Joining miner thread %d\n", i + 1);
        pthread_join(miner_threads[i], NULL);
    }
    
    // Free the memory allocated
    free(miner_threads);
    free(thread_ids);
    miner_threads = NULL;
    thread_ids = NULL;
    
    return 1;
}

void create_validator_process(){
    // Create the Validator process
    pid_t validator_id = fork();
    if(validator_id == 0){
        debug_message("Creating validator process\n");
        // Child process
        //validator_process();
        exit(0);  // Exit after validator process completes
    } else if (validator_id < 0) {
        // Error handling
        perror("Failed to create validator process");
    } else {
        // Parent process
        debug_message("Created validator process with PID %d\n", validator_id);
    }
}

void create_statistics_process(){
    // Create the Statistics process
    pid_t statistics_id = fork();
    if(statistics_id == 0){
        debug_message("Creating statistics process\n");
        // Child process
        //statistics_process();
        exit(0);  // Exit after statistics process completes
    } else if (statistics_id < 0) {
        // Error handling
        perror("Failed to create statistics process");
    } else {
        // Parent process
        debug_message("Created statistics process with PID %d\n", statistics_id);
    }
}

int main(int argc, char *argv[]) {
    // Initialize the logger
    logger_init("DEIChain_log.txt");
    
    log_message("DEIChain Controller starting up\n");
    
    // Read config and set up
    Config config = read_config();
    setup_shared_memory(config);
    
    // Set up signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGUSR1, signal_handler);
    
    log_message("Signal handlers installed. Send SIGUSR1 to print statistics (kill -SIGUSR1 %d)\n", getpid());
    
    // Create threads and processes
    create_miner_threads(config.num_miners);
    create_validator_process();
    create_statistics_process();
    
    // Main loop - wait for termination signal
    while (running_miner_threads) {
        // Check if we need to print statistics
        if (print_stats_flag) {
            print_system_statistics();
            print_stats_flag = 0;  // Reset the flag
        }
        
        sleep(1);
    }
    
    // Clean up
    if (wait_for_miner_threads() == 1) {
        #ifdef DEBUG
        log_message("All miner threads terminated\n");
        #endif
    }
    cleanup_shared_memory();

    log_message("All resources cleaned up, exiting\n");
    
    // Close the logger
    logger_close();
    
    return 0;
}

