//////////////////////////////////////////////////////////////////////////////////////
// wsn_v1.c - FIT3143 Lab 3, Version 1: lockstep rounds, blocking collectives
// ----------------------------------------------------------------------------------
// Simulates an EV charging wireless sensor network with a 2D MPI Cartesian topology.
// One MPI process represents each charging station, and one extra MPI process
// represents the base station. OpenMP threads update charging ports in parallel.
//
// V1 is the baseline. Every round, ALL processes synchronise:
//   1. Base station MPI_Bcast(RUN)            - blocking collective (world)
//   2. Every node updates its ports (OpenMP)
//   3. Every node: 2 x MPI_Neighbor_alltoall  - blocking collective (Cartesian)
//   4. Every node: MPI_Gather of its record   - blocking collective (world)
//   5. Base station MPI_Send advice to each alerting node (count, then list)
// After the last round, the base station MPI_Bcast(STOP) for a graceful shutdown.
//
// Based on the procedural C structure used in FIT3143 Week 11 MPI examples.
// Example: mpirun -np 10 ./wsn_v1 3 3 --ports 6 --rounds 5 --scenario demo
//////////////////////////////////////////////////////////////////////////////////////

#include "common.h"

#define VERSION "V1"
#define CMD_STOP 0
#define CMD_RUN 1
#define TAG_ADVICE_COUNT 210
#define TAG_ADVICE_NODES 211

// Round control broadcast by the base station; sent_at measures its delay.
typedef struct
{
    int command;
    double sent_at;
} Command;

typedef struct
{
    long long total_alerts;
    long long total_recommendations;
    double start_time;
} BaseMetrics;

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
    int i;
    double start;

    memset(report, 0, sizeof(*report));
    report->rank = node->rank;
    report->row = node->coords[0];
    report->col = node->coords[1];
    report->free_ports = node->free_ports;
    report->occupied_ports = cfg->ports - node->free_ports;
    report->port_threads = node->port_threads;
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

    start = global_time();
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
    report->neighbour_exchange_seconds = global_time() - start;
    report->neighbour_bytes_sent = report->neighbour_messages_sent * (int)sizeof(int);
    report->neighbour_bytes_received = report->neighbour_messages_received * (int)sizeof(int);

    if (!report->congested) return;

    for (i = 0; i < NUM_NEIGHBOURS; i++)
        if (node->neighbours[i] != MPI_PROC_NULL)
            report->neighbour_free[i] = replies_in[i];

    report->redirect_rank = choose_redirect(cfg, report->neighbours, report->neighbour_free);
    if (report->redirect_rank < 0)
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
    Command command;
    CommStats stats;
    int round = 0;
    memset(&stats, 0, sizeof(stats));
    initialise_node(&node, cfg, cart_comm);
    print_node_topology(&node, cfg);

    while (1)
    {
        // All world ranks participate in the base station's RUN/STOP broadcast.
        MPI_Bcast(&command, (int)sizeof(command), MPI_BYTE, base_rank, MPI_COMM_WORLD);
        if (command.command == CMD_STOP)
        {
            stats_record(&stats, DELAY_TERM, global_time() - command.sent_at);
            break;
        }
        round++;
        stats.cycles++;

        update_charging_ports(&node, cfg, round);
        exchange_neighbour_availability(&node, cfg, cart_comm, &report);
        stats.messages_sent += report.neighbour_messages_sent;
        stats.bytes_sent += report.neighbour_bytes_sent;
        if (report.congested)
            stats_record(&stats, DELAY_NBR_RTT, report.neighbour_exchange_seconds);

        // One fixed-size collective snapshot per node per round keeps the base
        // station up to date without many separate point-to-point heartbeats.
        report.sent_at = global_time();
        MPI_Gather(&report, (int)sizeof(report), MPI_BYTE, NULL, (int)sizeof(report),
                   MPI_BYTE, base_rank, MPI_COMM_WORLD);
        stats_count_message(&stats, (int)sizeof(report));

        if (report.redirect_rank >= 0)
        {
            stats.redirects++;
            printf("[Round %d][Node %d] %d free ports; redirect EVs to neighbour %d.\n",
                   round, node.rank, node.free_ports, report.redirect_rank);
            fflush(stdout);
        }
        if (report.alert)
        {
            stats.alerts++;
            receive_base_advice(&node, base_rank, round);
            stats_record(&stats, DELAY_REPORT_RTT, global_time() - report.sent_at);
        }
    }

    gather_stats(&stats, base_rank, NULL);
    free_node(&node);
    printf("[Node %d] Graceful shutdown complete.\n", node.rank);
    fflush(stdout);
}

/*
 * Function: process_round_reports
 * Purpose : Logs all charging-node snapshots, tracks congestion alerts and their
 *           timestamps, finds alternative nodes, and sends MPI advice replies.
 *           All MPI calls are performed by the base station's main thread.
 */
void process_round_reports(const Config *cfg, int round, int node_count,
                           const NodeReport *snapshots, double *last_report_time,
                           double *last_congested_time, FILE *log,
                           BaseMetrics *metrics, CommStats *stats, int *recommendations)
{
    int i, j;
    double now = global_time();
    int round_alerts = 0;
    long long round_neighbour_messages = 0;

    // Make all incident reports from this round visible before giving advice.
    // Only records with alert=1 count as reports: MPI_Gather needs every rank to
    // contribute, but the base station's knowledge comes from alerts alone.
    for (i = 0; i < node_count; i++)
    {
        const NodeReport *r = &snapshots[i];
        round_neighbour_messages += r->neighbour_messages_sent;
        if (!r->alert) continue;

        round_alerts++;
        stats_record(stats, DELAY_REPORT, now - r->sent_at);
        record_report(r, now, last_report_time, last_congested_time);
    }
    metrics->total_alerts += round_alerts;

    fprintf(log, "\n================ ROUND %d ================\n", round);
    fprintf(log, "simulation_elapsed_sec=%.6f alerts=%d neighbour_message_slots=%lld\n",
            now - metrics->start_time, round_alerts, round_neighbour_messages);

    for (i = 0; i < node_count; i++)
    {
        const NodeReport *r = &snapshots[i];
        fprintf(log, "NODE rank=%d coord=(%d,%d) time=%s free=%d occupied=%d "
                     "port_threads=%d congested=%d alert=%d redirect_neighbour=%d "
                     "incoming_requests=%d neighbour_messages_sent=%d "
                     "neighbour_messages_received=%d neighbour_bytes_sent=%d "
                     "neighbour_bytes_received=%d exchange_time_sec=%.9f\n",
                r->rank, r->row, r->col, r->timestamp, r->free_ports, r->occupied_ports,
                r->port_threads, r->congested, r->alert, r->redirect_rank,
                r->incoming_requests, r->neighbour_messages_sent,
                r->neighbour_messages_received, r->neighbour_bytes_sent,
                r->neighbour_bytes_received, r->neighbour_exchange_seconds);

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
        candidate_count = find_alternative_nodes(cfg, r, last_report_time,
                                                  last_congested_time, now,
                                                  recommendations, &recent_count);
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
        stats_count_message(stats, (int)sizeof(int));
        if (candidate_count > 0)
        {
            MPI_Send(recommendations, candidate_count, MPI_INT, r->rank,
                     TAG_ADVICE_NODES, MPI_COMM_WORLD);
            stats_count_message(stats, candidate_count * (int)sizeof(int));
            metrics->total_recommendations += candidate_count;
        }
    }
    fflush(log);
    printf("[Base] Round %d: %d congestion alert(s), %lld neighbour message slots.\n",
           round, round_alerts, round_neighbour_messages);
    fflush(stdout);
}

/*
 * Function: broadcast_command
 * Purpose : Base station's RUN/STOP control broadcast to every node.
 */
void broadcast_command(int value, int base_rank, int node_count, CommStats *stats)
{
    Command command;
    command.command = value;
    command.sent_at = global_time();
    MPI_Bcast(&command, (int)sizeof(command), MPI_BYTE, base_rank, MPI_COMM_WORLD);
    stats->messages_sent += node_count;    // One logical message per node
    stats->bytes_sent += (long long)node_count * (long long)sizeof(command);
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
    int round;
    FILE *log = open_base_log(cfg, VERSION);
    NodeReport *snapshots = NULL;
    double *last_report_time = NULL;     // When each node last sent an alert report
    double *last_congested_time = NULL;  // When a report last showed a node congested
    int *recommendations = NULL;
    BaseMetrics metrics;
    CommStats stats, total;
    double simulation_seconds;
    memset(&metrics, 0, sizeof(metrics));
    memset(&stats, 0, sizeof(stats));

    // MPI_Gather includes the base station as its final sender, so reserve N+1 slots.
    snapshots = (NodeReport *)calloc((size_t)node_count + 1U, sizeof(NodeReport));
    last_report_time = (double *)malloc((size_t)node_count * sizeof(double));
    last_congested_time = (double *)malloc((size_t)node_count * sizeof(double));
    recommendations = (int *)malloc((size_t)node_count * sizeof(int));
    if (snapshots == NULL || last_report_time == NULL || last_congested_time == NULL ||
        recommendations == NULL)
    {
        fprintf(stderr, "[Base] Memory allocation error.\n");
        MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
    }
    for (int i = 0; i < node_count; i++)
    {
        last_report_time[i] = -1.0;
        last_congested_time[i] = -1.0;
    }

    metrics.start_time = global_time();
    fprintf(log, "Note: neighbour_message_slots counts logical per-neighbour integer "
                 "payloads, not MPI's internal collective packets.\n");
    fprintf(log, "Note: advice uses only alert reports (alert=1); other gathered "
                 "records are logged for reference only.\n");

    for (round = 1; round <= cfg->rounds; round++)
    {
        broadcast_command(CMD_RUN, base_rank, node_count, &stats);

        // Root also participates in the collective with its own dummy send buffer.
        NodeReport empty_report;
        memset(&empty_report, 0, sizeof(empty_report));
        MPI_Gather(&empty_report, (int)sizeof(empty_report), MPI_BYTE,
                   snapshots, (int)sizeof(empty_report), MPI_BYTE,
                   base_rank, MPI_COMM_WORLD);
        process_round_reports(cfg, round, node_count, snapshots,
                              last_report_time, last_congested_time, log,
                              &metrics, &stats, recommendations);

        if (round < cfg->rounds) sleep_milliseconds(cfg->interval_ms);
    }

    // A final explicit MPI control broadcast signals every charging process to exit.
    broadcast_command(CMD_STOP, base_rank, node_count, &stats);
    simulation_seconds = global_time() - metrics.start_time;

    gather_stats(&stats, base_rank, &total);
    write_summary(log, cfg, VERSION, simulation_seconds, &total,
                  metrics.total_alerts, metrics.total_recommendations);
    append_csv(cfg, VERSION, simulation_seconds, &total);
    fclose(log);

    printf("[Base] Finished %d rounds. Total alerts: %lld. Log: %s\n",
           cfg->rounds, metrics.total_alerts, cfg->log_path);
    fflush(stdout);
    free(recommendations);
    free(last_congested_time);
    free(last_report_time);
    free(snapshots);
}

/*
 * Function: main
 * Purpose : Sets up MPI and the Cartesian topology (common.c), then runs Task 1
 *           or Task 2 by MPI rank, and finalises MPI cleanly.
 */
int main(int argc, char *argv[])
{
    Config cfg;
    MPI_Comm cart_comm;
    int base_rank;
    int role = wsn_init(&argc, &argv, VERSION, &cfg, &cart_comm, &base_rank);

    if (role == ROLE_INVALID) return EXIT_FAILURE;
    if (role == ROLE_BASE)
        run_base_station(&cfg, base_rank);
    else
    {
        run_charging_node(&cfg, cart_comm, base_rank);
        MPI_Comm_free(&cart_comm);
    }

    MPI_Finalize();
    return EXIT_SUCCESS;
}
