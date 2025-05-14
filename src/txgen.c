/* 
    João Victor Furukawa - 2021238987
    Gladys Maquena - 2022242385
*/

#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/wait.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/errno.h>
#include <semaphore.h>
#include "logger.h"
#include "txgen.h"

// ./txgen <reward> <sleep_time>
// Sleep time is in milliseconds
int running = 1;
int num_transactions = 0;
TransactionPool *transaction_pool = NULL;
sem_t *tx_pool_sem = NULL;

void cleanup_resources() {
    // Detach from shared memory if attached
    if (transaction_pool != NULL && transaction_pool != (void *)-1) {
        shmdt(transaction_pool);
        transaction_pool = NULL;
    }

    // Close semaphore if opened
    if (tx_pool_sem != NULL) {
        sem_close(tx_pool_sem);
        tx_pool_sem = NULL;
    }
}

void signal_handler_txgen(int signum) {
    if (signum == SIGINT) {
        log_message("TXGEN: Received SIGINT, shutting down...\n");
        running = 0;
        cleanup_resources();
        logger_close();
        exit(0);
    }
}

// Function to access the transaction pool shared memory
TransactionPool* access_transaction_pool() {
    // Generate the same key as in controller
    key_t tx_pool_key = ftok(TX_POOL_KEY_PATH, TX_POOL_KEY_ID);
    if (tx_pool_key == -1) {
        perror("TXGEN: Failed to generate transaction pool key");
        return NULL;
    }

    // Get the shared memory segment
    int shmid = shmget(tx_pool_key, 0, 0664); // Size 0 means we're accessing existing segment
    if (shmid == -1) {
        perror("TXGEN: Failed to access transaction pool shared memory");
        return NULL;
    }

    // Attach to the shared memory
    TransactionPool *pool = (TransactionPool *)shmat(shmid, NULL, 0);
    if (pool == (TransactionPool *)-1) {
        perror("TXGEN: Failed to attach to transaction pool");
        return NULL;
    }

    return pool;
}

// Function to add a transaction to the pool with semaphore protection
int add_transaction_to_pool(Transaction tx) {
    // Try to acquire the semaphore - this will block if no slots are available
    if (sem_wait(tx_pool_sem) == -1) {
        perror("TXGEN: Failed to lock semaphore");
        return -1;
    }

    // Critical section
    int result = -1;
    if (transaction_pool != NULL) {
        // Find the first empty slot
        for (int i = 0; i < transaction_pool->size; i++) {
            if (transaction_pool->entries[i].empty) {
                transaction_pool->entries[i].empty = 0;
                transaction_pool->entries[i].age = 0;
                
                // Lock mutex to safely increment the global tx_id counter
                pthread_mutex_lock(&transaction_pool->mutex);
                tx.tx_id = transaction_pool->next_tx_id++;
                pthread_mutex_unlock(&transaction_pool->mutex);
                
                transaction_pool->entries[i].t = tx;
                transaction_pool->transactions_pending++;
                result = i;
                #if DEBUG
                debug_message("TXGEN: Added transaction %d to pool at position %d\n", tx.tx_id, i);
                #endif
                // If we now have enough transactions for a block, signal the condition variable
                if (transaction_pool->transactions_pending >= transaction_pool->num_transactions_per_block) {
                    pthread_mutex_lock(&transaction_pool->mutex);
                    pthread_cond_signal(&transaction_pool->enough_tx);
                    pthread_mutex_unlock(&transaction_pool->mutex);
                }
                break;
            }
        }
    }

    // If we couldn't add the transaction, release the semaphore
    if (result == -1) {
        sem_post(tx_pool_sem);
    }

    return result;
}

// Function to remove a transaction from the pool
void remove_transaction_from_pool(int position) {
    if (position >= 0 && position < transaction_pool->size) {
        transaction_pool->entries[position].empty = 1;
        transaction_pool->transactions_pending--;
        // Release the semaphore to indicate a slot is now available
        sem_post(tx_pool_sem);
    }
}

int main(int argc, char *argv[]) {
    int keep_printing = 1;
    if (argc != 3) {
        fprintf(stderr, "Please use the following format: %s <reward> <sleep_time>\n", argv[0]);
        exit(1);
    }
    
    int reward = atoi(argv[1]);
    int sleep_time = atoi(argv[2]);

    if (reward < 1 || reward > 3 || sleep_time < 200 || sleep_time > 3000) {
        fprintf(stderr, "Invalid reward value, must be between 1 and 3\n");
        fprintf(stderr, "Invalid sleep time, must be between 200ms and 3000ms\n");
        exit(1);
    }

    // Initialize the logger
    logger_init("DEIChain_log.txt");
    log_message("TXGEN: Starting with reward=%d, sleep_time=%dms\n", reward, sleep_time);

    // Setup signal handler
    signal(SIGINT, signal_handler_txgen);

    // Access the transaction pool
    transaction_pool = access_transaction_pool();
    if (transaction_pool == NULL) {
        log_message("TXGEN: Failed to access transaction pool\n");
        exit(1);
    }

    // Open the semaphore
    tx_pool_sem = sem_open(TX_POOL_SEM, 0);
    if (tx_pool_sem == SEM_FAILED) {
        perror("TXGEN: Failed to open semaphore");
        cleanup_resources();
        exit(1);
    }

    // Create a random number generator
    srand(time(NULL));

    // Main loop
    while (running) {
        // Generate a transaction
        Transaction tx;
        memset(&tx, 0, sizeof(Transaction));  // Initialize all fields to 0
        
        // tx_id will be assigned by the transaction pool
        tx.tx_id = num_transactions;   // temporary ID, will be assigned by pool
        tx.reward = reward;  // This is already validated in main()
        tx.value = (rand() % 1000) / 3;  // Value between 0 and 333
        tx.tx_timestamp = time(NULL);

        // Validate transaction values before adding to pool
        if (tx.reward < 1 || tx.reward > 3 || tx.value < 0 || tx.value > 333) {
            log_message("TXGEN: Invalid transaction values - ID: %d, Reward: %d, Value: %d\n",
                       tx.tx_id, tx.reward, tx.value);
            usleep(sleep_time * 1000);
            continue;
        }

        // Try to add the transaction to the pool
        int result = add_transaction_to_pool(tx);
        if (result >= 0) {
            log_message("TXGEN: TX-%d generated with reward %d, value %d\n",
                       tx.tx_id, tx.reward, tx.value);
            num_transactions++;
            keep_printing = 1;
        } else {
            if(keep_printing){
                log_message("TXGEN: Transaction pool is full, waiting...\n");
            }
            keep_printing = 0;
        }

        // Sleep for the specified time
        usleep(sleep_time * 1000);
    }

    // Cleanup
    cleanup_resources();
    logger_close();

    return 0;
}






