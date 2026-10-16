//////////////////////////////////////////////////////////////////////////////////////
// lab3_wsn.c - FIT3143 Lab 3 (Tasks 1 and 2)
// ----------------------------------------------------------------------------------
// Simulates an EV charging wireless sensor network with a 2D MPI Cartesian topology.
// One MPI process represents each charging station, and one extra MPI process
// represents the base station. OpenMP threads update charging ports in parallel.
//
// Based on the procedural C structure used in FIT3143 Week 11 MPI examples.
// Compile: mpicc -std=c11 -O2 -Wall -Wextra -fopenmp lab3_wsn.c -o lab3_wsn
// Example: mpirun -np 10 ./lab3_wsn 3 3 --ports 6 --rounds 5 --scenario demo
//////////////////////////////////////////////////////////////////////////////////////

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
#define CMD_STOP 0
#define CMD_RUN 1
#define TAG_ADVICE_COUNT 300
#define TAG_ADVICE_NODES 301

#define TOP 0
#define BOTTOM 1
#define LEFT 2
#define RIGHT 3

#define SCENARIO_RANDOM 0
#define SCENARIO_DEMO 1
#define SCENARIO_FULL 2

typedef struct
{
    int rows;
    int cols;
    int ports;
    int threshold;
    int rounds;
    int interval_ms;
    int window_sec;
    int arrival_percent;
    int departure_percent;
    int scenario;
    int seed;
    char log_path[LOG_PATH_LEN];
} Config;

typedef struct
{
    int rank;
    int coords[2];
    int *port_status;              // 0 = free, 1 = occupied; shared by OpenMP threads
    uint32_t *random_state;        // Independent random state for each port
    int neighbours[NUM_NEIGHBOURS];
    int free_ports;
} ChargingNode;

typedef struct
{
    int rank;
    int row;
    int col;
    int free_ports;
    int occupied_ports;
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
    char timestamp[TIME_LEN];
} NodeReport;

typedef struct
{
    long long total_alerts;
    long long total_neighbour_messages;
    long long total_snapshot_messages;
    long long total_advice_messages;
    long long total_recommendations;
    double start_time;
} BaseMetrics;

/*
 * Function: print_usage
 * Purpose : Prints the command-line options and an example of launching the program.
 *           The required first two arguments are the Cartesian grid dimensions.
 */
void print_usage(const char *program)
{
    fprintf(stderr,
        "Usage: %s ROWS COLS [options]\n"
        "  --ports K              Number of ports at each station (default: 6)\n"
        "  --threshold T          Congested when free ports < T (default: 1)\n"
        "  --rounds R             Number of simulation rounds (default: 10)\n"
        "  --interval-ms MS       Delay between rounds in ms (default: 1000)\n"
        "  --window-sec S         Recent-alert lookup window in seconds (default: 30)\n"
        "  --arrival P            Free-to-occupied probability, percent (default: 45)\n"
        "  --departure P          Occupied-to-free probability, percent (default: 35)\n"
        "  --scenario NAME        random | demo | full (default: random)\n"
        "  --seed N               Random seed (default: 12345)\n"
        "  --log PATH             Base-station output file (default: base_station.log)\n"
        "Example: mpirun -np 10 %s 3 3 --ports 6 --rounds 5 --scenario demo\n",
        program, program);
}

/*
 * Function: parse_integer
 * Purpose : Converts a string to an integer, rejecting non-numeric input,
 *           integer overflow, and numbers outside the requested range.
 * Returns : 1 when valid; 0 otherwise.
 */
int parse_integer(const char *text, int minimum, int maximum, int *value)
{
    char *end = NULL;
    long number;
    errno = 0;
    number = strtol(text, &end, 10);
    if (errno != 0 || text[0] == '\0' || *end != '\0' ||
        number < minimum || number > maximum)
        return 0;
    *value = (int)number;
    return 1;
}

/*
 * Function: parse_config
 * Purpose : Sets default simulation options, then reads and validates optional
 *           command-line parameters. Only the base-station rank calls this.
 * Returns : 1 for valid arguments; 0 for invalid arguments.
 */
int parse_config(int argc, char *argv[], Config *cfg)
{
    int i;
    memset(cfg, 0, sizeof(*cfg));
    cfg->ports = 6;
    cfg->threshold = 1;
    cfg->rounds = 10;
    cfg->interval_ms = 1000;
    cfg->window_sec = 30;
    cfg->arrival_percent = 45;
    cfg->departure_percent = 35;
    cfg->seed = 12345;
    cfg->scenario = SCENARIO_RANDOM;
    snprintf(cfg->log_path, LOG_PATH_LEN, "base_station.log");

    if (argc < 3 || !parse_integer(argv[1], 1, 10000, &cfg->rows) ||
        !parse_integer(argv[2], 1, 10000, &cfg->cols))
        return 0;

    for (i = 3; i < argc; i += 2)
    {
        const char *option = argv[i];
        const char *value;
        if (i + 1 >= argc)
            return 0;
        value = argv[i + 1];

        if (strcmp(option, "--ports") == 0)
        {
            if (!parse_integer(value, 1, 10000, &cfg->ports)) return 0;
        }
        else if (strcmp(option, "--threshold") == 0)
        {
            if (!parse_integer(value, 1, 10000, &cfg->threshold)) return 0;
        }
        else if (strcmp(option, "--rounds") == 0)
        {
            if (!parse_integer(value, 1, 100000, &cfg->rounds)) return 0;
        }
        else if (strcmp(option, "--interval-ms") == 0)
        {
            if (!parse_integer(value, 0, 3600000, &cfg->interval_ms)) return 0;
        }
        else if (strcmp(option, "--window-sec") == 0)
        {
            if (!parse_integer(value, 1, 86400, &cfg->window_sec)) return 0;
        }
        else if (strcmp(option, "--arrival") == 0)
        {
            if (!parse_integer(value, 0, 100, &cfg->arrival_percent)) return 0;
        }
        else if (strcmp(option, "--departure") == 0)
        {
            if (!parse_integer(value, 0, 100, &cfg->departure_percent)) return 0;
        }
        else if (strcmp(option, "--seed") == 0)
        {
            if (!parse_integer(value, 0, INT_MAX, &cfg->seed)) return 0;
        }
        else if (strcmp(option, "--scenario") == 0)
        {
            if (strcmp(value, "random") == 0) cfg->scenario = SCENARIO_RANDOM;
            else if (strcmp(value, "demo") == 0) cfg->scenario = SCENARIO_DEMO;
            else if (strcmp(value, "full") == 0) cfg->scenario = SCENARIO_FULL;
            else return 0;
        }
        else if (strcmp(option, "--log") == 0)
        {
            if (strlen(value) >= LOG_PATH_LEN || value[0] == '\0') return 0;
            snprintf(cfg->log_path, LOG_PATH_LEN, "%s", value);
        }
        else return 0;
    }

    if (cfg->threshold > cfg->ports) return 0;
    if ((long long)cfg->rows * cfg->cols > INT_MAX - 1) return 0;
    return 1;
}

/*
 * Function: sleep_milliseconds
 * Purpose : Pauses the base station between simulation rounds so port updates
 *           occur periodically. Retries if a signal interrupts nanosleep().
 */
void sleep_milliseconds(int milliseconds)
{
    struct timespec delay;
    delay.tv_sec = milliseconds / 1000;
    delay.tv_nsec = (long)(milliseconds % 1000) * 1000000L;
    while (nanosleep(&delay, &delay) == -1 && errno == EINTR) { }
}

/*
 * Function: current_timestamp
 * Purpose : Formats the local wall-clock date and time of a congestion event.
 */
void current_timestamp(char *buffer, size_t capacity)
{
    time_t now = time(NULL);
    struct tm local_time;
    localtime_r(&now, &local_time);
    strftime(buffer, capacity, "%Y-%m-%d %H:%M:%S", &local_time);
}

/*
 * Function: next_random
 * Purpose : Generates a small pseudo-random value for a single charging port.
 *           Each port owns its own state, avoiding rand() races between threads.
 */
uint32_t next_random(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

/*
 * Function: initialise_node
 * Purpose : Allocates the port-status shared array and per-port random states;
 *           also obtains the node's coordinates and immediate Cartesian neighbours.
 */
void initialise_node(ChargingNode *node, const Config *cfg, MPI_Comm cart_comm)
{
    int i;
    memset(node, 0, sizeof(*node));
    MPI_Comm_rank(cart_comm, &node->rank);
    MPI_Cart_coords(cart_comm, node->rank, NUM_DIMS, node->coords);
    MPI_Cart_shift(cart_comm, 0, 1, &node->neighbours[TOP], &node->neighbours[BOTTOM]);
    MPI_Cart_shift(cart_comm, 1, 1, &node->neighbours[LEFT], &node->neighbours[RIGHT]);

    node->port_status = (int *)calloc((size_t)cfg->ports, sizeof(int));
    node->random_state = (uint32_t *)calloc((size_t)cfg->ports, sizeof(uint32_t));
    if (node->port_status == NULL || node->random_state == NULL)
    {
        fprintf(stderr, "[Node %d] Could not allocate charging-port arrays.\n", node->rank);
        MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
    }

    for (i = 0; i < cfg->ports; i++)
    {
        uint32_t s = (uint32_t)cfg->seed ^
                     ((uint32_t)(node->rank + 1) * UINT32_C(747796405)) ^
                     ((uint32_t)(i + 1) * UINT32_C(2891336453));
        node->random_state[i] = (s == 0) ? UINT32_C(1) : s;
        node->port_status[i] = ((next_random(&node->random_state[i]) % 100U) < 60U);
    }
}

/*
 * Function: update_charging_ports
 * Purpose : Uses OpenMP to simulate arrivals/departures at all charging ports.
 *           Each loop iteration updates one unique entry in the shared array.
 *           The reduction safely counts free ports after the updates complete.
 */
void update_charging_ports(ChargingNode *node, const Config *cfg, int round)
{
    int free_count = 0;
    int number_of_threads = omp_get_max_threads();
    int row = node->coords[0], col = node->coords[1];
    int distance_from_centre = abs(row - cfg->rows / 2) + abs(col - cfg->cols / 2);
    if (number_of_threads > cfg->ports) number_of_threads = cfg->ports;

    #pragma omp parallel for num_threads(number_of_threads) reduction(+:free_count) schedule(static)
    for (int port = 0; port < cfg->ports; port++)
    {
        if (cfg->scenario == SCENARIO_FULL)
        {
            node->port_status[port] = 1;
        }
        else if (cfg->scenario == SCENARIO_DEMO)
        {
            // Middle + direct neighbours are full; farther nodes have spare ports.
            // Rotate the free port(s) to demonstrate the shared array changing.
            if (distance_from_centre <= 1)
                node->port_status[port] = 1;
            else
                node->port_status[port] = ((port + round) % cfg->ports >= cfg->threshold);
        }
        else
        {
            int change = (int)(next_random(&node->random_state[port]) % 100U);
            if (node->port_status[port] == 0 && change < cfg->arrival_percent)
                node->port_status[port] = 1; // An EV arrives.
            else if (node->port_status[port] == 1 && change < cfg->departure_percent)
                node->port_status[port] = 0; // An EV leaves.
        }
        if (node->port_status[port] == 0) free_count++;
    }
    node->free_ports = free_count;
}

/*
 * Function: exchange_neighbour_availability
 * Purpose : Sends an availability-request flag to each immediate MPI neighbour,
 *           then replies with the current free-port count to requesting nodes.
 *           Neighbour collectives avoid blocking point-to-point deadlocks.
 *           A request flag of 0 means this node is not currently congested.
 *           The report records neighbour ranks, results, logical message counts,
 *           transferred payload sizes, and communication time.
 */
void exchange_neighbour_availability(ChargingNode *node, const Config *cfg,
                                     MPI_Comm cart_comm, NodeReport *report)
{
    int requests_out[NUM_NEIGHBOURS] = {0};
    int requests_in[NUM_NEIGHBOURS] = {0};
    int replies_out[NUM_NEIGHBOURS] = {-1, -1, -1, -1};
    int replies_in[NUM_NEIGHBOURS] = {-1, -1, -1, -1};
    int i, available_rank = MPI_PROC_NULL, best_availability = -1;
    double start;

    memset(report, 0, sizeof(*report));
    report->rank = node->rank;
    report->row = node->coords[0];
    report->col = node->coords[1];
    report->free_ports = node->free_ports;
    report->occupied_ports = cfg->ports - node->free_ports;
    report->congested = (node->free_ports < cfg->threshold);
    report->redirect_rank = -1;
    current_timestamp(report->timestamp, TIME_LEN);

    for (i = 0; i < NUM_NEIGHBOURS; i++)
    {
        report->neighbours[i] = node->neighbours[i];
        report->neighbour_free[i] = -1;
        requests_out[i] = (report->congested && node->neighbours[i] != MPI_PROC_NULL);
        report->requested[i] = requests_out[i];
        if (node->neighbours[i] != MPI_PROC_NULL)
        {
            // Each valid neighbour receives one request flag and one response slot.
            report->neighbour_messages_sent += 2;
            report->neighbour_messages_received += 2;
        }
    }

    start = MPI_Wtime();
    // MPI Cartesian neighbour order: top, bottom, left, right.
    MPI_Neighbor_alltoall(requests_out, 1, MPI_INT, requests_in, 1, MPI_INT, cart_comm);

    for (i = 0; i < NUM_NEIGHBOURS; i++)
    {
        if (requests_in[i] != 0 && node->neighbours[i] != MPI_PROC_NULL)
        {
            report->incoming_requests++;
            replies_out[i] = node->free_ports;
        }
    }
    MPI_Neighbor_alltoall(replies_out, 1, MPI_INT, replies_in, 1, MPI_INT, cart_comm);
    report->neighbour_exchange_seconds = MPI_Wtime() - start;
    report->neighbour_bytes_sent = report->neighbour_messages_sent * (int)sizeof(int);
    report->neighbour_bytes_received = report->neighbour_messages_received * (int)sizeof(int);

    if (!report->congested) return;

    for (i = 0; i < NUM_NEIGHBOURS; i++)
    {
        if (node->neighbours[i] == MPI_PROC_NULL) continue;
        report->neighbour_free[i] = replies_in[i];
        // Prefer a non-congested neighbour with the most free ports.
        if (replies_in[i] >= cfg->threshold &&
            (replies_in[i] > best_availability ||
             (replies_in[i] == best_availability &&
              (available_rank == MPI_PROC_NULL || node->neighbours[i] < available_rank))))
        {
            available_rank = node->neighbours[i];
            best_availability = replies_in[i];
        }
    }

    if (available_rank != MPI_PROC_NULL)
        report->redirect_rank = available_rank;
    else
    {
        report->alert = 1; // No immediate neighbour has sufficient available ports.
        // Record the discovery time after receiving all neighbours' replies.
        current_timestamp(report->timestamp, TIME_LEN);
    }
}

/*
 * Function: receive_base_advice
 * Purpose : Receives a variable-length list of equally nearest alternatives from
 *           the base station after this node reports a congestion incident.
 *           Prints the recommendation that would be forwarded to incoming EVs.
 */
void receive_base_advice(const ChargingNode *node, int base_rank, int round)
{
    int count, i;
    int *ranks = NULL;
    MPI_Recv(&count, 1, MPI_INT, base_rank, TAG_ADVICE_COUNT,
             MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    if (count < 0)
    {
        fprintf(stderr, "[Node %d] Received invalid advice count.\n", node->rank);
        MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
    }
    if (count > 0)
    {
        ranks = (int *)malloc((size_t)count * sizeof(int));
        if (ranks == NULL) MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
        MPI_Recv(ranks, count, MPI_INT, base_rank, TAG_ADVICE_NODES,
                 MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    }

    printf("[Round %d][Node %d] Congestion alert -> base station. ", round, node->rank);
    if (count == 0)
        printf("No available nearby nodes.\n");
    else
    {
        printf("Nearest available node(s) for EVs:");
        for (i = 0; i < count; i++) printf(" %d", ranks[i]);
        printf("\n");
    }
    fflush(stdout);
    free(ranks);
}

/*
 * Function: run_charging_node
 * Purpose : Runs Task 1. On each base-station command, updates ports in OpenMP,
 *           exchanges MPI messages with Cartesian neighbours, and contributes
 *           the latest snapshot or congestion alert to the base station.
 *           Waits for advice only when an alert was actually sent.
 */
void run_charging_node(const Config *cfg, MPI_Comm cart_comm, int base_rank)
{
    ChargingNode node;
    NodeReport report;
    int command, round = 0;
    initialise_node(&node, cfg, cart_comm);

    while (1)
    {
        // All world ranks participate in the base station's RUN/STOP broadcast.
        MPI_Bcast(&command, 1, MPI_INT, base_rank, MPI_COMM_WORLD);
        if (command == CMD_STOP) break;
        round++;

        update_charging_ports(&node, cfg, round);
        exchange_neighbour_availability(&node, cfg, cart_comm, &report);

        // One fixed-size collective snapshot per node per round keeps the base
        // station up to date without many separate point-to-point heartbeats.
        MPI_Gather(&report, (int)sizeof(report), MPI_BYTE, NULL, (int)sizeof(report),
                   MPI_BYTE, base_rank, MPI_COMM_WORLD);

        if (report.redirect_rank >= 0)
        {
            printf("[Round %d][Node %d] %d free ports; redirect EVs to neighbour %d.\n",
                   round, node.rank, node.free_ports, report.redirect_rank);
            fflush(stdout);
        }
        if (report.alert)
            receive_base_advice(&node, base_rank, round);
    }

    free(node.port_status);
    free(node.random_state);
    printf("[Node %d] Graceful shutdown complete.\n", node.rank);
    fflush(stdout);
}

/*
 * Function: record_neighbours
 * Purpose : Writes neighbour ranks and reported free-port availability to the
 *           base-station log; used for explaining congestion incidents.
 */
void record_neighbours(FILE *log, const NodeReport *report)
{
    static const char *names[NUM_NEIGHBOURS] = {"top", "bottom", "left", "right"};
    int i;
    for (i = 0; i < NUM_NEIGHBOURS; i++)
    {
        if (report->neighbours[i] == MPI_PROC_NULL) continue;
        fprintf(log, "    %-6s node=%d free_ports=%d request=%d\n",
                names[i], report->neighbours[i], report->neighbour_free[i],
                report->requested[i]);
    }
}

/*
 * Function: find_alternative_nodes
 * Purpose : Implements Task 2's recent-report time-window rule. If ALL direct
 *           neighbours sent congestion alerts recently, it reports no nearby
 *           candidates. Otherwise, it scans the latest charging-node snapshots,
 *           selects the minimum Manhattan distance, and includes EVERY tied
 *           available node. The supplied result array needs one slot per node.
 * Returns : Number of suggested nodes, possibly zero.
 */
int find_alternative_nodes(const Config *cfg, const NodeReport *report,
                           const NodeReport *snapshots, const double *last_alert_time,
                           double now, int node_count, int *results, int *recent_count)
{
    int i, min_distance = INT_MAX, result_count = 0, neighbours = 0;
    *recent_count = 0;

    for (i = 0; i < NUM_NEIGHBOURS; i++)
    {
        int rank = report->neighbours[i];
        if (rank == MPI_PROC_NULL) continue;
        neighbours++;
        if (last_alert_time[rank] >= 0.0 &&
            (now - last_alert_time[rank]) <= (double)cfg->window_sec)
            (*recent_count)++;
    }

    // The specification's rule: every adjacent node having reported recently
    // means the base station returns "no nodes available nearby".
    if (neighbours > 0 && *recent_count == neighbours)
        return 0;

    for (i = 0; i < node_count; i++)
    {
        int distance;
        if (i == report->rank || snapshots[i].free_ports < cfg->threshold)
            continue;
        distance = abs(report->row - snapshots[i].row) +
                   abs(report->col - snapshots[i].col);
        if (distance < min_distance)
        {
            min_distance = distance;
            result_count = 0;
        }
        if (distance == min_distance)
            results[result_count++] = i;
    }
    return result_count;
}

/*
 * Function: process_round_reports
 * Purpose : Logs all charging-node snapshots, tracks congestion alerts and their
 *           timestamps, finds alternative nodes, and sends MPI advice replies.
 *           All MPI calls are performed by the base station's main thread.
 */
void process_round_reports(const Config *cfg, int round, int node_count,
                           const NodeReport *snapshots, double *last_alert_time,
                           FILE *log, BaseMetrics *metrics,
                           int *recommendations)
{
    int i, j;
    double now = MPI_Wtime();
    int round_alerts = 0;
    long long round_neighbour_messages = 0;

    // Make all incident reports from this round visible before giving advice.
    for (i = 0; i < node_count; i++)
    {
        if (snapshots[i].alert)
        {
            last_alert_time[i] = now;
            round_alerts++;
        }
        round_neighbour_messages += snapshots[i].neighbour_messages_sent;
    }
    metrics->total_alerts += round_alerts;
    metrics->total_neighbour_messages += round_neighbour_messages;
    metrics->total_snapshot_messages += node_count;

    fprintf(log, "\n================ ROUND %d ================\n", round);
    fprintf(log, "simulation_elapsed_sec=%.6f alerts=%d neighbour_message_slots=%lld\n",
            now - metrics->start_time, round_alerts, round_neighbour_messages);

    for (i = 0; i < node_count; i++)
    {
        const NodeReport *r = &snapshots[i];
        fprintf(log, "NODE rank=%d coord=(%d,%d) time=%s free=%d occupied=%d "
                     "congested=%d alert=%d redirect_neighbour=%d "
                     "incoming_requests=%d neighbour_messages_sent=%d "
                     "neighbour_messages_received=%d neighbour_bytes_sent=%d "
                     "neighbour_bytes_received=%d exchange_time_sec=%.9f\n",
                r->rank, r->row, r->col, r->timestamp, r->free_ports, r->occupied_ports,
                r->congested, r->alert, r->redirect_rank, r->incoming_requests,
                r->neighbour_messages_sent, r->neighbour_messages_received,
                r->neighbour_bytes_sent, r->neighbour_bytes_received,
                r->neighbour_exchange_seconds);

        if (!r->alert) continue;

        // Every congestion report contains its timestamp, identity, availability,
        // direct-neighbour data, number of exchanges, and payload byte counts.
        fprintf(log, "  ALERT incident_time=%s sender_rank=%d sender_coord=(%d,%d) "
                     "sender_free_ports=%d\n",
                r->timestamp, r->rank, r->row, r->col, r->free_ports);
        record_neighbours(log, r);
    }

    for (i = 0; i < node_count; i++)
    {
        const NodeReport *r = &snapshots[i];
        int candidate_count, recent_count;
        if (!r->alert) continue;
        candidate_count = find_alternative_nodes(cfg, r, snapshots, last_alert_time,
                                                  now, node_count, recommendations,
                                                  &recent_count);
        fprintf(log, "  ADVICE to_node=%d recent_neighbour_alerts=%d count=%d nearest_nodes=",
                r->rank, recent_count, candidate_count);
        if (candidate_count == 0) fprintf(log, "none");
        for (j = 0; j < candidate_count; j++)
            fprintf(log, "%s%d", (j ? "," : ""), recommendations[j]);
        fprintf(log, "\n");

        // The receiver is waiting after MPI_Gather(), so these sends complete
        // without a message cycle or a persistent unmatched MPI request.
        MPI_Send(&candidate_count, 1, MPI_INT, r->rank,
                 TAG_ADVICE_COUNT, MPI_COMM_WORLD);
        metrics->total_advice_messages++;
        if (candidate_count > 0)
        {
            MPI_Send(recommendations, candidate_count, MPI_INT, r->rank,
                     TAG_ADVICE_NODES, MPI_COMM_WORLD);
            metrics->total_advice_messages++;
            metrics->total_recommendations += candidate_count;
        }
    }
    fflush(log);
    printf("[Base] Round %d: %d congestion alert(s), %lld neighbour message slots.\n",
           round, round_alerts, round_neighbour_messages);
    fflush(stdout);
}

/*
 * Function: run_base_station
 * Purpose : Runs Task 2 using exactly one MPI process and one MPI-calling thread.
 *           Starts each simulation round, gathers node snapshots and reports,
 *           responds with nearest alternatives, logs performance metrics, and
 *           finally broadcasts a graceful termination command to every node.
 */
void run_base_station(const Config *cfg, int base_rank)
{
    int node_count = cfg->rows * cfg->cols;
    int round, command;
    FILE *log = fopen(cfg->log_path, "w");
    NodeReport *snapshots = NULL;
    double *last_alert_time = NULL;
    int *recommendations = NULL;
    BaseMetrics metrics;
    memset(&metrics, 0, sizeof(metrics));

    if (log == NULL)
    {
        fprintf(stderr, "[Base] Could not open log file '%s'.\n", cfg->log_path);
        MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
    }
    // MPI_Gather includes the base station as its final sender, so reserve N+1 slots.
    snapshots = (NodeReport *)calloc((size_t)node_count + 1U, sizeof(NodeReport));
    last_alert_time = (double *)malloc((size_t)node_count * sizeof(double));
    recommendations = (int *)malloc((size_t)node_count * sizeof(int));
    if (snapshots == NULL || last_alert_time == NULL || recommendations == NULL)
    {
        fprintf(stderr, "[Base] Memory allocation error.\n");
        MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
    }
    for (int i = 0; i < node_count; i++) last_alert_time[i] = -1.0;

    metrics.start_time = MPI_Wtime();
    fprintf(log, "FIT3143 Lab 3 - Base Station Log\n");
    fprintf(log, "grid=%dx%d ports=%d threshold=%d rounds=%d interval_ms=%d "
                 "window_sec=%d scenario=%d seed=%d\n",
            cfg->rows, cfg->cols, cfg->ports, cfg->threshold, cfg->rounds,
            cfg->interval_ms, cfg->window_sec, cfg->scenario, cfg->seed);
    fprintf(log, "Note: neighbour_message_slots counts logical per-neighbour integer "
                 "payloads, not MPI's internal collective packets.\n");

    for (round = 1; round <= cfg->rounds; round++)
    {
        command = CMD_RUN;
        MPI_Bcast(&command, 1, MPI_INT, base_rank, MPI_COMM_WORLD);

        // Root also participates in the collective with its own dummy send buffer.
        NodeReport empty_report;
        memset(&empty_report, 0, sizeof(empty_report));
        MPI_Gather(&empty_report, (int)sizeof(empty_report), MPI_BYTE,
                   snapshots, (int)sizeof(empty_report), MPI_BYTE,
                   base_rank, MPI_COMM_WORLD);
        process_round_reports(cfg, round, node_count, snapshots,
                              last_alert_time, log, &metrics,
                              recommendations);

        if (round < cfg->rounds) sleep_milliseconds(cfg->interval_ms);
    }

    // A final explicit MPI control broadcast signals every charging process to exit.
    command = CMD_STOP;
    MPI_Bcast(&command, 1, MPI_INT, base_rank, MPI_COMM_WORLD);

    fprintf(log, "\n================ FINAL SUMMARY ================\n");
    fprintf(log, "simulation_time_sec=%.6f\n", MPI_Wtime() - metrics.start_time);
    fprintf(log, "total_alerts=%lld\n", metrics.total_alerts);
    fprintf(log, "total_neighbour_message_slots=%lld\n", metrics.total_neighbour_messages);
    fprintf(log, "total_snapshot_payloads=%lld\n", metrics.total_snapshot_messages);
    fprintf(log, "total_advice_messages=%lld\n", metrics.total_advice_messages);
    fprintf(log, "total_recommended_nodes=%lld\n", metrics.total_recommendations);
    fprintf(log, "total_logical_messages_excluding_internal_collective_protocol=%lld\n",
            metrics.total_neighbour_messages + metrics.total_snapshot_messages +
            metrics.total_advice_messages + (long long)node_count * (cfg->rounds + 1));
    fprintf(log, "graceful_shutdown=initiated\n");
    fflush(log);
    fclose(log);

    printf("[Base] Finished %d rounds. Total alerts: %lld. Log: %s\n",
           cfg->rounds, metrics.total_alerts, cfg->log_path);
    fflush(stdout);
    free(recommendations);
    free(last_alert_time);
    free(snapshots);
}

/*
 * Function: main
 * Purpose : Initialises MPI with FUNNELED thread support, validates that the
 *           number of processes equals (rows * columns + 1), creates a separate
 *           Cartesian communicator for charging nodes, launches Task 1 or Task 2
 *           by MPI rank, then releases communicators and finalises MPI cleanly.
 */
int main(int argc, char *argv[])
{
    int world_rank, world_size, base_rank, provided;
    int valid = 0;
    int dims[NUM_DIMS], wrap_around[NUM_DIMS] = {0, 0};
    Config cfg;
    MPI_Comm nodes_comm = MPI_COMM_NULL;
    MPI_Comm cart_comm = MPI_COMM_NULL;

    MPI_Init_thread(&argc, &argv, MPI_THREAD_FUNNELED, &provided);
    MPI_Comm_rank(MPI_COMM_WORLD, &world_rank);
    MPI_Comm_size(MPI_COMM_WORLD, &world_size);
    base_rank = world_size - 1;

    if (provided < MPI_THREAD_FUNNELED)
    {
        if (world_rank == base_rank)
            fprintf(stderr, "MPI implementation does not support MPI_THREAD_FUNNELED.\n");
        MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
    }

    if (world_rank == base_rank)
    {
        valid = parse_config(argc, argv, &cfg);
        if (valid && ((long long)cfg.rows * cfg.cols + 1 != world_size))
        {
            fprintf(stderr, "Expected %lld MPI processes, but found %d.\n",
                    (long long)cfg.rows * cfg.cols + 1, world_size);
            valid = 0;
        }
        if (!valid) print_usage(argv[0]);
    }
    MPI_Bcast(&valid, 1, MPI_INT, base_rank, MPI_COMM_WORLD);
    if (!valid)
    {
        MPI_Finalize();
        return EXIT_FAILURE;
    }
    MPI_Bcast(&cfg, (int)sizeof(cfg), MPI_BYTE, base_rank, MPI_COMM_WORLD);

    // Separate charging-node ranks from the base-station rank. Only charging
    // ranks are permitted in the Cartesian grid communicator.
    MPI_Comm_split(MPI_COMM_WORLD,
                   (world_rank == base_rank ? MPI_UNDEFINED : 0),
                   world_rank, &nodes_comm);

    if (world_rank == base_rank)
    {
        printf("[Base] Grid %d x %d, ports/node=%d, MPI processes=%d.\n",
               cfg.rows, cfg.cols, cfg.ports, world_size);
        fflush(stdout);
        run_base_station(&cfg, base_rank);
    }
    else
    {
        dims[0] = cfg.rows;
        dims[1] = cfg.cols;
        MPI_Cart_create(nodes_comm, NUM_DIMS, dims, wrap_around, 0, &cart_comm);
        if (cart_comm == MPI_COMM_NULL)
        {
            fprintf(stderr, "[Rank %d] Failed to create MPI Cartesian topology.\n",
                    world_rank);
            MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
        }
        run_charging_node(&cfg, cart_comm, base_rank);
        MPI_Comm_free(&cart_comm);
        MPI_Comm_free(&nodes_comm);
    }

    MPI_Finalize();
    return EXIT_SUCCESS;
}
