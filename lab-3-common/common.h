//////////////////////////////////////////////////////////////////////////////////////
// common.h - FIT3143 Lab 3 (Tasks 1 and 2): code shared by every version
// ----------------------------------------------------------------------------------
// Configuration, message layouts, charging-port simulation, the base station's
// nearest-node search, communication-delay statistics and MPI start-up.
// Every version uses exactly this logic, so versions differ ONLY in how they
// pass messages - which keeps the Task 3 comparisons fair.
//////////////////////////////////////////////////////////////////////////////////////
#ifndef WSN_COMMON_H
#define WSN_COMMON_H

#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <mpi.h>
#include <omp.h>

#define NUM_DIMS 2
#define NUM_NEIGHBOURS 4
#define TIME_LEN 32
#define LOG_PATH_LEN 256
#define MAX_PORTS 256              // One OpenMP thread per port, so keep this modest
#define MAX_CANDIDATES 8           // At most 8 nodes are two hops away in a 2D grid

#define TOP 0
#define BOTTOM 1
#define LEFT 2
#define RIGHT 3

#define SCENARIO_RANDOM 0
#define SCENARIO_DEMO 1
#define SCENARIO_FULL 2
#define SCENARIO_CORNER 3

#define ROLE_INVALID -1
#define ROLE_NODE 0
#define ROLE_BASE 1

// Point-to-point message tags. Neighbour messages travel on the Cartesian
// communicator; base-station messages travel on MPI_COMM_WORLD.
#define TAG_NBR_REQ 100
#define TAG_NBR_RESP 101
#define TAG_REPORT 200
#define TAG_ADVICE 201
#define TAG_TERM 300
#define TAG_CLOCK_SYNC 400
#define CLOCK_SYNC_ROUNDS 8

typedef struct
{
    int rows;
    int cols;
    int ports;
    int threshold;
    int rounds;
    int interval_ms;
    int window_ms;
    int poll_us;
    int arrival_percent;
    int departure_percent;
    int scenario;
    int seed;
    char log_path[LOG_PATH_LEN];
    char csv_path[LOG_PATH_LEN];   // Empty = no CSV output
} Config;

typedef struct
{
    int rank;
    int coords[2];
    int *port_status;              // 0 = free, 1 = occupied; shared by OpenMP threads
    uint32_t *random_state;        // Independent random state for each port
    int neighbours[NUM_NEIGHBOURS];
    int free_ports;
    int port_threads;              // OpenMP threads that actually ran the last update
} ChargingNode;

// Status record (V1) / congestion report (all versions) sent to the base station.
typedef struct
{
    int rank;
    int row;
    int col;
    int free_ports;
    int occupied_ports;
    int port_threads;
    int neighbours[NUM_NEIGHBOURS];
    int neighbour_free[NUM_NEIGHBOURS];
    int requested[NUM_NEIGHBOURS];
    int incoming_requests;
    int neighbour_messages_sent;
    int neighbour_messages_received;
    int neighbour_bytes_sent;
    int neighbour_bytes_received;
    int redirect_rank;
    int congested;
    int alert;
    double neighbour_exchange_seconds;
    double sent_at;                // MPI_Wtime() when sent, for delay measurement
    char timestamp[TIME_LEN];
} NodeReport;

// Point-to-point message bodies (V2 onwards). Each carries its send time.
typedef struct
{
    double sent_at;                // Requester is identified by the MPI status source
} NbrRequest;

typedef struct
{
    int free_ports;
    double sent_at;
} NbrResponse;

typedef struct
{
    int count;                     // 0 = no nodes available nearby
    int nodes[MAX_CANDIDATES];
    int recent_neighbour_reports;
    double sent_at;
} Advice;

typedef struct
{
    double sent_at;
} Termination;

// Communication delays recorded for Task 3. One-way delays compare timestamps
// from two processes, so every timestamp uses global_time(): MPI_Wtime() plus
// this process's estimated offset from the base station's clock (Open MPI's
// MPI_Wtime() starts from a per-process origin, so raw values do not agree even
// on one machine). Round-trip delays (RTT) use one process's clock only.
enum DelayType
{
    DELAY_NBR_REQ,                 // neighbour request, one-way (at the neighbour)
    DELAY_NBR_RESP,                // neighbour response, one-way (at the requester)
    DELAY_NBR_RTT,                 // exchange start -> ALL neighbours replied (requester)
    DELAY_REPORT,                  // congestion report, one-way (at the base station)
    DELAY_ADVICE,                  // base-station advice, one-way (at the node)
    DELAY_REPORT_RTT,              // report sent -> advice received (node)
    DELAY_TERM,                    // termination message, one-way (at the node)
    NUM_DELAY_TYPES
};

typedef struct
{
    long long count[NUM_DELAY_TYPES];
    double total[NUM_DELAY_TYPES];
    double max[NUM_DELAY_TYPES];
    long long messages_sent;       // Logical messages (per destination)
    long long bytes_sent;
    long long cycles;
    long long redirects;
    long long alerts;
} CommStats;

extern const char *DELAY_NAMES[NUM_DELAY_TYPES];

// Start-up, configuration and utilities
int wsn_init(int *argc, char ***argv, const char *version, Config *cfg,
             MPI_Comm *cart_comm, int *base_rank);
double global_time(void);
void sleep_milliseconds(int milliseconds);
void sleep_microseconds(int microseconds);
void current_timestamp(char *buffer, size_t capacity);

// Charging node (Task 1)
void initialise_node(ChargingNode *node, const Config *cfg, MPI_Comm cart_comm);
void free_node(ChargingNode *node);
void print_node_topology(const ChargingNode *node, const Config *cfg);
void update_charging_ports(ChargingNode *node, const Config *cfg, int round);
int choose_redirect(const Config *cfg, const int *neighbours, const int *neighbour_free);

// Base station (Task 2)
int is_recent(double event_time, double now, const Config *cfg);
void record_report(const NodeReport *report, double now, double *last_report_time,
                   double *last_congested_time);
int find_alternative_nodes(const Config *cfg, const NodeReport *report,
                           const double *last_report_time,
                           const double *last_congested_time,
                           double now, int *results, int *recent_count);
void record_neighbours(FILE *log, const NodeReport *report);
FILE *open_base_log(const Config *cfg, const char *version);

// Communication statistics (Task 3)
void stats_record(CommStats *stats, int type, double delay_seconds);
void stats_count_message(CommStats *stats, int bytes);
void gather_stats(const CommStats *mine, int base_rank, CommStats *total);
void write_summary(FILE *log, const Config *cfg, const char *version,
                   double simulation_seconds, const CommStats *total,
                   long long reports_received, long long recommendations);
void append_csv(const Config *cfg, const char *version, double simulation_seconds,
                const CommStats *total);

#endif
