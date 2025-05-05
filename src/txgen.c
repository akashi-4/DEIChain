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
    int result = -1;

    // Wait for semaphore
    if (sem_wait(tx_pool_sem) == -1) {
        perror("TXGEN: Failed to lock semaphore");
        return -1;
    }

    // Critical section
    if (transaction_pool != NULL) {
        for (int i = 0; i < transaction_pool->size; i++) {
            if (transaction_pool->entries[i].empty) {
                transaction_pool->entries[i].empty = 0;
                transaction_pool->entries[i].age = 0;
                transaction_pool->entries[i].t = tx;
                transaction_pool->transactions_pending++;
                result = i;
                log_message("TXGEN: Added transaction %d to pool at position %d\n", tx.tx_id, i);
                break;
            }
        }
    }

    // Release semaphore
    if (sem_post(tx_pool_sem) == -1) {
        perror("TXGEN: Failed to unlock semaphore");
        return -1;
    }

    return result;
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
        tx.tx_id = getpid() * 1000 + num_transactions; // Make ID unique across processes
        tx.reward = reward;
        tx.value = (double)(rand() % 1000) / 3.0;
        tx.tx_timestamp = time(NULL);

        // Try to add the transaction to the pool
        int result = add_transaction_to_pool(tx);
        if (result >= 0) {
            log_message("TXGEN: Transaction %d generated with reward %d and value %.2f\n", 
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






