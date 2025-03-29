#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <string.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <errno.h>
#include <signal.h>

#define DEBUG

// Structure for a transaction
typedef struct {
    int transaction_id;  // PID + incremental number
    int reward;          // 1-3 initially, can increase with aging
    int sender_id;       // PID of the transaction generator
    int receiver_id;     // Random number
    int value;           // Quantity associated with the transaction
    time_t timestamp;    // Time of creation
} Transaction;

// Structure for transaction pool entry
typedef struct {
    int empty;           // 1 if empty, 0 if filled
    int age;             // Age counter, starts at 0
    Transaction tx;      // The transaction itself
} TransactionPoolEntry;

// Structure for the transaction pool in shared memory
typedef struct {
    int current_block_id;                // Current block ID
    int tx_pool_size;                    // Size of the transaction pool
    TransactionPoolEntry transactions[]; // Variable-sized array
} TransactionPool;

// Global variables
int shmid = -1;
TransactionPool *pool = NULL;
int semid = -1;
int tx_counter = 0;

// Function prototypes
void cleanup();
void handle_signal(int sig);

// Set up signal handlers and initialize resources
void setup() {
    // Register signal handlers
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    
    // Set cleanup function to be called at exit
    atexit(cleanup);
    
    // Seed random number generator
    srand(time(NULL) ^ getpid());
}

// Clean up resources
void cleanup() {
    if (pool != NULL) {
        // Detach from shared memory
        if (shmdt(pool) == -1) {
            perror("shmdt");
        }
        
        #ifdef DEBUG
        printf("TxGen[%d]: Detached from shared memory\n", getpid());
        #endif
    }
    
    #ifdef DEBUG
    printf("TxGen[%d]: Exiting...\n", getpid());
    #endif
}

// Signal handler
void handle_signal(int sig) {
    #ifdef DEBUG
    printf("TxGen[%d]: Received signal %d\n", getpid(), sig);
    #endif
    
    exit(0);
}

// Function to attach to the transaction pool
int attach_to_transaction_pool() {
    key_t key;
    
    // Generate the same key as the Controller process
    // Using ftok with a common file path
    if ((key = ftok("/tmp/deichain", 'T')) == -1) {
        perror("ftok");
        return -1;
    }
    
    // Get existing shared memory segment
    if ((shmid = shmget(key, 0, 0)) == -1) {
        perror("shmget");
        return -1;
    }
    
    // Attach to the shared memory segment
    if ((pool = (TransactionPool *)shmat(shmid, NULL, 0)) == (TransactionPool *)-1) {
        perror("shmat");
        return -1;
    }
    
    #ifdef DEBUG
    printf("TxGen[%d]: Attached to transaction pool (size: %d)\n", 
           getpid(), pool->tx_pool_size);
    #endif
    
    return 0;
}

// Function to attach to semaphores
int attach_to_semaphores() {
    key_t key;
    
    // Generate the same key as the Controller process
    if ((key = ftok("/tmp/deichain", 'S')) == -1) {
        perror("ftok");
        return -1;
    }
    
    // Get existing semaphore set
    if ((semid = semget(key, 0, 0)) == -1) {
        perror("semget");
        return -1;
    }
    
    #ifdef DEBUG
    printf("TxGen[%d]: Attached to semaphores\n", getpid());
    #endif
    
    return 0;
}

// Semaphore operations
int sem_wait(int sem_num) {
    struct sembuf op;
    op.sem_num = sem_num;
    op.sem_op = -1;
    op.sem_flg = 0;
    
    return semop(semid, &op, 1);
}

int sem_signal(int sem_num) {
    struct sembuf op;
    op.sem_num = sem_num;
    op.sem_op = 1;
    op.sem_flg = 0;
    
    return semop(semid, &op, 1);
}

// Generate and add a transaction to the pool
void generate_transaction(int reward) {
    int i;
    int added = 0;
    
    // Create a new transaction
    Transaction tx;
    tx.transaction_id = (getpid() << 16) | (tx_counter++);
    tx.reward = reward;
    tx.sender_id = getpid();
    tx.receiver_id = rand() % 1000;  // Random receiver
    tx.value = rand() % 100 + 1;     // Random value between 1 and 100
    tx.timestamp = time(NULL);
    
    // Wait for access to the transaction pool
    if (sem_wait(0) == -1) {
        perror("sem_wait");
        return;
    }
    
    // Find an empty slot in the transaction pool
    for (i = 0; i < pool->tx_pool_size; i++) {
        if (pool->transactions[i].empty) {
            // Found an empty slot, add the transaction
            pool->transactions[i].empty = 0;
            pool->transactions[i].age = 0;
            pool->transactions[i].tx = tx;
            added = 1;
            
            #ifdef DEBUG
            printf("TxGen[%d]: Added transaction ID %d with reward %d to pool slot %d\n", 
                   getpid(), tx.transaction_id, tx.reward, i);
            #endif
            
            break;
        }
    }
    
    // Release access to the transaction pool
    if (sem_signal(0) == -1) {
        perror("sem_signal");
    }
    
    if (!added) {
        #ifdef DEBUG
        printf("TxGen[%d]: Transaction pool is full, couldn't add transaction\n", getpid());
        #endif
    }
}

int main(int argc, char *argv[]) {
    int reward;
    int sleep_time;
    
    // Check command line arguments
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <reward> <sleep time>\n", argv[0]);
        fprintf(stderr, "  reward: 1 to 3\n");
        fprintf(stderr, "  sleep time (ms): 200 to 3000\n");
        return EXIT_FAILURE;
    }
    
    // Parse reward
    reward = atoi(argv[1]);
    if (reward < 1 || reward > 3) {
        fprintf(stderr, "Error: reward must be between 1 and 3\n");
        return EXIT_FAILURE;
    }
    
    // Parse sleep time
    sleep_time = atoi(argv[2]);
    if (sleep_time < 200 || sleep_time > 3000) {
        fprintf(stderr, "Error: sleep time must be between 200 and 3000 ms\n");
        return EXIT_FAILURE;
    }
    
    #ifdef DEBUG
    printf("TxGen[%d]: Starting with reward=%d, sleep_time=%d ms\n", 
           getpid(), reward, sleep_time);
    #endif
    
    // Set up signal handlers and initialize resources
    setup();
    
    // Attach to the transaction pool
    if (attach_to_transaction_pool() != 0) {
        fprintf(stderr, "Failed to attach to transaction pool\n");
        return EXIT_FAILURE;
    }
    
    // Attach to semaphores
    if (attach_to_semaphores() != 0) {
        fprintf(stderr, "Failed to attach to semaphores\n");
        return EXIT_FAILURE;
    }
    
    // Main loop: generate transactions at specified intervals
    while (1) {
        // Generate a transaction with the specified reward
        generate_transaction(reward);
        
        // Sleep for the specified time
        usleep(sleep_time * 1000);  // Convert milliseconds to microseconds
    }
    
    return EXIT_SUCCESS;  // Will never reach here due to infinite loop
}