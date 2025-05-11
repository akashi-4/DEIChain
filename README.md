# Blockchain Simulation Project

## Overview

This project simulates a blockchain network with multiple components working together to create, validate, and maintain a blockchain. The system consists of a Controller process that manages the overall system, Miner processes that create blocks, a Validator process that validates blocks, a Statistics process that tracks system metrics, and Transaction Generator processes that create transactions.

## System Components

### Controller Process
- Reads configuration from `config.cfg`
- Initializes shared memory segments for Transaction Pool and Blockchain Ledger
- Creates and manages all other processes (Miners, Validator, Statistics)
- Handles system-wide signals (SIGINT, SIGUSR1)
- Manages system shutdown and cleanup
- Maintains the blockchain ledger in shared memory

### Transaction Generator (TxGen)
- Creates transactions with specified reward (1-3) and sleep time (200-3000ms)
- Writes transactions to the shared Transaction Pool
- Implements aging mechanism for transactions
- Handles its own cleanup on shutdown

### Miner Process
- Creates multiple miner threads (as specified in config)
- Each thread:
  - Waits for enough transactions in the pool
  - Selects random transactions
  - Creates a new block
  - Performs proof-of-work
  - Sends valid blocks to the Validator

### Validator Process
- Receives blocks from miners through a named pipe
- Validates blocks by:
  - Verifying proof-of-work
  - Checking transaction validity
  - Ensuring proper block linking
- Adds valid blocks to the blockchain
- Removes validated transactions from the pool
- Maintains a ledger log file

### Statistics Process
- Tracks system metrics:
  - Number of valid/invalid blocks per miner
  - Average transaction verification time
  - Miner credits
  - Total blocks validated
  - Total blocks in blockchain
- Responds to SIGUSR1 signal to print statistics

## Data Structures

### Transaction
```c
typedef struct {
    int tx_id;           // Unique transaction ID
    int reward;          // Reward value (1-3)
    int value;           // Transaction value (0-333)
    time_t tx_timestamp; // Creation timestamp
} Transaction;
```

### Block
```c
typedef struct {
    int txb_id;                    // Block ID
    char prev_hash[HASH_SIZE];     // Hash of previous block
    time_t txb_timestamp;          // Block creation time
    int nonce;                     // Proof-of-work nonce
    Transaction transactions[];     // Array of transactions
} Block;
```

### BlockchainLedger
```c
typedef struct {
    int num_blocks;     // Current number of blocks
    Block blocks[];     // Array of blocks
} BlockchainLedger;
```

### TransactionPool
```c
typedef struct {
    int size;                      // Pool size
    int transactions_pending;      // Current pending transactions
    int num_transactions_per_block;// Required transactions per block
    pthread_mutex_t mutex;         // Mutex for synchronization
    pthread_cond_t enough_tx;      // Condition variable
    TransactionEntry entries[];    // Transaction entries
} TransactionPool;
```

## Synchronization Mechanisms

### Semaphores
- `TX_POOL_SEM`: Controls access to transaction pool slots
- `BLOCKCHAIN_LEDGER_SEM`: Protects blockchain ledger access

### Mutexes
- `transaction_pool->mutex`: Protects transaction pool operations
- `validator_message_mutex`: Protects validator message handling
- `pipe_mutex`: Protects named pipe access

### Condition Variables
- `transaction_pool->enough_tx`: Signals when enough transactions are available

## Proof of Work

The system implements a proof-of-work mechanism with three difficulty levels:
- EASY (reward 1): Hash must start with "0000" followed by 0-b
- NORMAL (reward 2): Hash must start with "00000"
- HARD (reward 3): Hash must start with "00000" followed by 0-b

## Configuration

The system is configured through `config.cfg`:
```
NUM_MINERS = 2              # Number of miner threads
TX_POOL_SIZE = 50          # Size of transaction pool
TRANSACTIONS_PER_BLOCK = 5  # Transactions per block
BLOCKCHAIN_BLOCKS = 50000  # Maximum number of blocks
```

## Logging

The system maintains two log files:
- `DEIChain_log.txt`: General system logs
- `blockchain_ledger.txt`: Detailed blockchain ledger

## Building and Running

1. Build the project:
```bash
make
```

2. Start the controller:
```bash
./controller
```

3. Start transaction generators:
```bash
./txgen <reward> <sleep_time>
```

4. View statistics:
```bash
kill -SIGUSR1 <controller_pid>
```

5. Shutdown:
```bash
kill -SIGINT <controller_pid>
```
