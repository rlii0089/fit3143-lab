//////////////////////////////////////////////////////////////////////////////////////
// wsn_v2.c - FIT3143 Lab 3, Version 2: event-driven, blocking point-to-point
// ----------------------------------------------------------------------------------
// Changes from V1 (lockstep rounds, blocking collectives):
//  * No global rounds: each node runs its own port-update cycle, so no node ever
//    waits for the slowest node in the grid (no RUN broadcast, no MPI_Gather).
//  * A node asks its neighbours ONLY when it is congested, then blocks in
//    MPI_Recv until every neighbour has replied. While blocked it still answers
//    any neighbour request that arrives, so two congested neighbours asking each
//    other at once cannot deadlock.
//  * ONLY nodes that raise an alert send a report to the base station; the node
//    then blocks in MPI_Recv for the advice (it cannot answer neighbours then).
//  * The base station checks for reports with MPI_Iprobe and receives them with
//    a blocking MPI_Recv, so it can still send TERM on time when idle.
//  * Graceful shutdown: on TERM a node sends DONE to its neighbours and the base
//    station, then keeps answering requests until every neighbour has sent DONE
//    (no neighbour can ask after its DONE, as MPI keeps one sender's messages in
//    order). The base station answers reports until every node has sent DONE.
//
// Every send is MPI_Bsend: blocking, but it returns once the message is copied
// into an attached buffer, so two processes sending to each other at the same
// time cannot deadlock (a standard MPI_Send may wait for the matching receive).
// Between cycles a node uses MPI_Iprobe only to check whether a message has
// arrived before calling the blocking MPI_Recv, so an idle node still starts its
// next cycle on time.
//
// Only the main thread calls MPI (MPI_THREAD_FUNNELED); the OpenMP port threads
// only touch the shared port array.
// Example: mpirun -np 10 ./wsn_v2 3 3 --ports 6 --rounds 5 --scenario corner
//////////////////////////////////////////////////////////////////////////////////////

#include "common.h"

#define VERSION "V2"
#define TAG_DONE 301

// Any message a node can receive from a neighbour (the MPI tag says which).
typedef union
{
    NbrRequest request;
    NbrResponse response;
} NbrMessage;

typedef struct
{
    ChargingNode node;
    CommStats stats;
    int cycle;
    int terminating;               // TERM received: start no new requests/reports
    int requests_serviced;         // Neighbour requests answered this cycle
    int neighbour_count;
    int done_from;                 // Neighbours that have sent DONE
    int responses_pending;         // Replies still expected for this node's request
    int response_free[NUM_NEIGHBOURS];
    NodeReport report;
} NodeState;

/*
 * Function: attach_send_buffer
 * Purpose : Attaches the buffer MPI_Bsend copies outgoing messages into, sized
 *           for the most messages this process can have in flight at once.
 */
void attach_send_buffer(int messages, int largest_message)
{
    int size = 2 * messages * (largest_message + MPI_BSEND_OVERHEAD);  // 2x margin
    char *buffer = (char *)malloc((size_t)size);
    if (buffer == NULL) MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
    MPI_Buffer_attach(buffer, size);
}

/*
 * Function: detach_send_buffer
 * Purpose : Waits until every buffered message has been delivered, then frees it.
 */
void detach_send_buffer(void)
{
    char *buffer;
    int size;
    MPI_Buffer_detach(&buffer, &size);
    free(buffer);
}

/*
 * Function: direction_of
 * Purpose : Maps a neighbour's Cartesian rank to TOP/BOTTOM/LEFT/RIGHT.
 */
int direction_of(const ChargingNode *node, int rank)
{
    int i;
    for (i = 0; i < NUM_NEIGHBOURS; i++)
        if (node->neighbours[i] == rank) return i;
    fprintf(stderr, "[Node %d] Message from non-neighbour %d.\n", node->rank, rank);
    MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
    return -1;
}

/*
 * Function: receive_neighbour_message
 * Purpose : Blocks in MPI_Recv for the next message from any neighbour. A
 *           request is answered straight away with this node's free-port count;
 *           a response is stored for this node's own request; DONE is counted.
 */
void receive_neighbour_message(NodeState *st, MPI_Comm cart_comm)
{
    NbrMessage msg;
    NbrResponse reply;
    MPI_Status status;
    double now;

    MPI_Recv(&msg, (int)sizeof(msg), MPI_BYTE, MPI_ANY_SOURCE, MPI_ANY_TAG, cart_comm,
             &status);
    now = global_time();

    if (status.MPI_TAG == TAG_NBR_REQ)
    {
        stats_record(&st->stats, DELAY_NBR_REQ, now - msg.request.sent_at);
        reply.free_ports = st->node.free_ports;
        reply.sent_at = global_time();
        MPI_Bsend(&reply, (int)sizeof(reply), MPI_BYTE, status.MPI_SOURCE, TAG_NBR_RESP,
                  cart_comm);
        stats_count_message(&st->stats, (int)sizeof(reply));
        st->requests_serviced++;
    }
    else if (status.MPI_TAG == TAG_NBR_RESP)
    {
        stats_record(&st->stats, DELAY_NBR_RESP, now - msg.response.sent_at);
        st->response_free[direction_of(&st->node, status.MPI_SOURCE)] =
            msg.response.free_ports;
        st->responses_pending--;
    }
    else if (status.MPI_TAG == TAG_DONE)
        st->done_from++;
}

/*
 * Function: report_and_wait
 * Purpose : Builds the congestion report (all neighbours full), sends it to the
 *           base station, then blocks in MPI_Recv until the advice arrives and
 *           prints what would be forwarded to incoming EVs.
 */
void report_and_wait(NodeState *st, const Config *cfg, int base_rank,
                     double exchange_seconds)
{
    NodeReport *r = &st->report;
    Advice advice;
    double now;
    int i;

    memset(r, 0, sizeof(*r));
    r->rank = st->node.rank;
    r->row = st->node.coords[0];
    r->col = st->node.coords[1];
    r->free_ports = st->node.free_ports;
    r->occupied_ports = cfg->ports - st->node.free_ports;
    r->port_threads = st->node.port_threads;
    r->congested = 1;
    r->alert = 1;
    r->redirect_rank = -1;
    r->incoming_requests = st->requests_serviced;
    r->neighbour_exchange_seconds = exchange_seconds;
    current_timestamp(r->timestamp, TIME_LEN);   // When the incident was discovered

    for (i = 0; i < NUM_NEIGHBOURS; i++)
    {
        r->neighbours[i] = st->node.neighbours[i];
        r->neighbour_free[i] = -1;
        if (st->node.neighbours[i] == MPI_PROC_NULL) continue;
        r->neighbour_free[i] = st->response_free[i];
        r->requested[i] = 1;
    }
    // One request out and one response back per neighbour.
    r->neighbour_messages_sent = st->neighbour_count;
    r->neighbour_messages_received = st->neighbour_count;
    r->neighbour_bytes_sent = st->neighbour_count * (int)sizeof(NbrRequest);
    r->neighbour_bytes_received = st->neighbour_count * (int)sizeof(NbrResponse);

    r->sent_at = global_time();
    MPI_Bsend(r, (int)sizeof(NodeReport), MPI_BYTE, base_rank, TAG_REPORT, MPI_COMM_WORLD);
    stats_count_message(&st->stats, (int)sizeof(NodeReport));
    st->stats.alerts++;

    // Blocked here: neighbour requests wait until the advice has arrived.
    MPI_Recv(&advice, (int)sizeof(advice), MPI_BYTE, base_rank, TAG_ADVICE,
             MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    now = global_time();
    stats_record(&st->stats, DELAY_ADVICE, now - advice.sent_at);
    stats_record(&st->stats, DELAY_REPORT_RTT, now - r->sent_at);

    printf("[Cycle %d][Node %d] Congestion alert -> base station. ", st->cycle, st->node.rank);
    if (advice.count == 0)
        printf("No available nearby nodes.\n");
    else
    {
        printf("Nearest available node(s) for EVs:");
        for (i = 0; i < advice.count; i++) printf(" %d", advice.nodes[i]);
        printf("\n");
    }
    fflush(stdout);
}

/*
 * Function: ask_neighbours
 * Purpose : A congested node sends a request to every neighbour, then blocks
 *           until all of them have replied. Redirects EVs to the best free
 *           neighbour, or (if all are full) reports to the base station.
 */
void ask_neighbours(NodeState *st, const Config *cfg, MPI_Comm cart_comm, int base_rank)
{
    NbrRequest request;
    double start, exchange_seconds;
    int i, redirect;

    start = global_time();
    request.sent_at = start;
    st->responses_pending = 0;
    for (i = 0; i < NUM_NEIGHBOURS; i++)
    {
        st->response_free[i] = -1;
        if (st->node.neighbours[i] == MPI_PROC_NULL) continue;
        MPI_Bsend(&request, (int)sizeof(request), MPI_BYTE, st->node.neighbours[i],
                  TAG_NBR_REQ, cart_comm);
        stats_count_message(&st->stats, (int)sizeof(request));
        st->responses_pending++;
    }

    // One blocking MPI_Recv per message until every reply is in; neighbours'
    // own requests that arrive meanwhile are answered inside the same loop.
    while (st->responses_pending > 0)
        receive_neighbour_message(st, cart_comm);

    // One sample per completed exchange (until ALL neighbours replied), exactly
    // what V1 measures around its two neighbour collectives.
    exchange_seconds = global_time() - start;
    stats_record(&st->stats, DELAY_NBR_RTT, exchange_seconds);

    redirect = choose_redirect(cfg, st->node.neighbours, st->response_free);
    if (redirect >= 0)
    {
        st->stats.redirects++;
        printf("[Cycle %d][Node %d] %d free ports; redirect EVs to neighbour %d.\n",
               st->cycle, st->node.rank, st->node.free_ports, redirect);
        fflush(stdout);
    }
    else
        report_and_wait(st, cfg, base_rank, exchange_seconds);
}

/*
 * Function: run_charging_node
 * Purpose : Runs Task 1 as an event loop. Each pass: update ports when the
 *           node's own cycle is due (asking neighbours if congested), answer
 *           any neighbour requests that have arrived, and check for TERM.
 *           Sleeps poll_us between passes so idle nodes do not burn a CPU core.
 */
void run_charging_node(const Config *cfg, MPI_Comm cart_comm, int base_rank)
{
    NodeState st;
    Termination term_in;
    MPI_Status status;
    int i, flag;
    double next_cycle;

    memset(&st, 0, sizeof(st));
    initialise_node(&st.node, cfg, cart_comm);
    print_node_topology(&st.node, cfg);
    for (i = 0; i < NUM_NEIGHBOURS; i++)
        if (st.node.neighbours[i] != MPI_PROC_NULL) st.neighbour_count++;

    // In flight at once: one request and one reply per neighbour, one report,
    // and DONE to every neighbour and the base station.
    attach_send_buffer(3 * NUM_NEIGHBOURS + 2, (int)sizeof(NodeReport));

    next_cycle = global_time();
    while (!st.terminating)
    {
        double now = global_time();
        // Exactly --rounds cycles (same workload as V1); then just keep
        // servicing neighbours until TERM arrives.
        if (st.cycle < cfg->rounds && now >= next_cycle)
        {
            st.cycle++;
            st.stats.cycles++;
            st.requests_serviced = 0;
            update_charging_ports(&st.node, cfg, st.cycle);
            next_cycle += cfg->interval_ms / 1000.0;
            if (next_cycle < now) next_cycle = now;   // Fell behind: don't burst

            if (st.node.free_ports < cfg->threshold)
                ask_neighbours(&st, cfg, cart_comm, base_rank);
        }

        // Receive only what has already arrived, so an idle node never blocks.
        MPI_Iprobe(MPI_ANY_SOURCE, MPI_ANY_TAG, cart_comm, &flag, MPI_STATUS_IGNORE);
        while (flag)
        {
            receive_neighbour_message(&st, cart_comm);
            MPI_Iprobe(MPI_ANY_SOURCE, MPI_ANY_TAG, cart_comm, &flag, MPI_STATUS_IGNORE);
        }

        MPI_Iprobe(base_rank, TAG_TERM, MPI_COMM_WORLD, &flag, &status);
        if (flag)
        {
            MPI_Recv(&term_in, (int)sizeof(term_in), MPI_BYTE, base_rank, TAG_TERM,
                     MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            stats_record(&st.stats, DELAY_TERM, global_time() - term_in.sent_at);
            st.terminating = 1;
        }
        else
            sleep_microseconds(cfg->poll_us);
    }

    // This node will send no more requests or reports: tell its neighbours and
    // the base station, then keep answering until every neighbour is done too.
    for (i = 0; i < NUM_NEIGHBOURS; i++)
    {
        if (st.node.neighbours[i] == MPI_PROC_NULL) continue;
        MPI_Bsend(&term_in, 0, MPI_BYTE, st.node.neighbours[i], TAG_DONE, cart_comm);
        stats_count_message(&st.stats, 0);
    }
    MPI_Bsend(&term_in, 0, MPI_BYTE, base_rank, TAG_DONE, MPI_COMM_WORLD);
    stats_count_message(&st.stats, 0);
    while (st.done_from < st.neighbour_count)
        receive_neighbour_message(&st, cart_comm);
    detach_send_buffer();

    gather_stats(&st.stats, base_rank, NULL);
    free_node(&st.node);
    printf("[Node %d] Graceful shutdown complete.\n", st.node.rank);
    fflush(stdout);
}

/*
 * Function: handle_report
 * Purpose : Processes one congestion report: updates the base station's
 *           knowledge, computes the nearest available nodes, sends the advice
 *           straight away (MPI_Bsend), then logs the incident.
 */
void handle_report(const Config *cfg, const NodeReport *r, int source, double start,
                   double *last_report_time, double *last_congested_time, FILE *log,
                   CommStats *stats, long long *recommendations)
{
    int j, recent_count, count;
    double now = global_time();
    Advice advice;

    stats_record(stats, DELAY_REPORT, now - r->sent_at);
    record_report(r, now, last_report_time, last_congested_time);

    memset(&advice, 0, sizeof(advice));
    count = find_alternative_nodes(cfg, r, last_report_time, last_congested_time, now,
                                   advice.nodes, &recent_count);
    advice.count = count;
    advice.recent_neighbour_reports = recent_count;
    advice.sent_at = global_time();
    MPI_Bsend(&advice, (int)sizeof(advice), MPI_BYTE, source, TAG_ADVICE, MPI_COMM_WORLD);
    stats_count_message(stats, (int)sizeof(advice));
    *recommendations += count;

    // Logged after the advice is on its way, so file I/O does not delay the node.
    fprintf(log, "\nREPORT received_at_sec=%.6f\n", now - start);
    fprintf(log, "  ALERT incident_time=%s sender_rank=%d sender_coord=(%d,%d) "
                 "sender_free_ports=%d port_threads=%d incoming_requests=%d "
                 "neighbour_messages_sent=%d neighbour_messages_received=%d "
                 "neighbour_bytes_sent=%d neighbour_bytes_received=%d "
                 "exchange_time_sec=%.9f\n",
            r->timestamp, r->rank, r->row, r->col, r->free_ports, r->port_threads,
            r->incoming_requests, r->neighbour_messages_sent,
            r->neighbour_messages_received, r->neighbour_bytes_sent,
            r->neighbour_bytes_received, r->neighbour_exchange_seconds);
    record_neighbours(log, r);
    fprintf(log, "  ADVICE to_node=%d recent_neighbour_alerts=%d count=%d nearest_nodes=",
            source, recent_count, count);
    if (count == 0) fprintf(log, "none");
    for (j = 0; j < count; j++)
        fprintf(log, "%s%d", (j ? "," : ""), advice.nodes[j]);
    fprintf(log, "\n");
}

/*
 * Function: run_base_station
 * Purpose : Runs Task 2 using exactly one MPI process and one MPI-calling thread.
 *           Periodically checks for reports (MPI_Iprobe), receives each with a
 *           blocking MPI_Recv and answers it straight away. After rounds x
 *           interval_ms it sends TERM to every node, then answers any remaining
 *           reports until every node has sent DONE.
 */
void run_base_station(const Config *cfg, int base_rank)
{
    int node_count = cfg->rows * cfg->cols;
    int i, flag, done_count = 0;
    FILE *log = open_base_log(cfg, VERSION);
    double *last_report_time = NULL;     // When each node last sent an alert report
    double *last_congested_time = NULL;  // When a report last showed a node congested
    MPI_Status status;
    NodeReport report_in;
    Termination term_out;
    CommStats stats, total;
    long long reports_received = 0, recommendations = 0;
    double start, end_time, simulation_seconds;
    memset(&stats, 0, sizeof(stats));

    last_report_time = (double *)malloc((size_t)node_count * sizeof(double));
    last_congested_time = (double *)malloc((size_t)node_count * sizeof(double));
    if (last_report_time == NULL || last_congested_time == NULL)
    {
        fprintf(stderr, "[Base] Memory allocation error.\n");
        MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
    }
    for (i = 0; i < node_count; i++)
    {
        last_report_time[i] = -1.0;
        last_congested_time[i] = -1.0;
    }
    // In flight at once: one advice per node (it waits for it before reporting
    // again) and one TERM per node.
    attach_send_buffer(2 * node_count, (int)sizeof(Advice));

    start = global_time();
    end_time = start + cfg->rounds * (cfg->interval_ms / 1000.0);

    while (global_time() < end_time)
    {
        MPI_Iprobe(MPI_ANY_SOURCE, TAG_REPORT, MPI_COMM_WORLD, &flag, &status);
        if (!flag)
        {
            sleep_microseconds(cfg->poll_us);
            continue;
        }
        MPI_Recv(&report_in, (int)sizeof(report_in), MPI_BYTE, status.MPI_SOURCE,
                 TAG_REPORT, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        reports_received++;
        handle_report(cfg, &report_in, status.MPI_SOURCE, start, last_report_time,
                      last_congested_time, log, &stats, &recommendations);
    }

    term_out.sent_at = global_time();
    for (i = 0; i < node_count; i++)
    {
        MPI_Bsend(&term_out, (int)sizeof(term_out), MPI_BYTE, i, TAG_TERM, MPI_COMM_WORLD);
        stats_count_message(&stats, (int)sizeof(term_out));
    }
    printf("[Base] Simulation time reached; TERM sent to %d nodes.\n", node_count);
    fflush(stdout);

    // Nothing else to do but wait: block for each message. Reports sent before
    // TERM reached their node still get advice.
    while (done_count < node_count)
    {
        MPI_Recv(&report_in, (int)sizeof(report_in), MPI_BYTE, MPI_ANY_SOURCE,
                 MPI_ANY_TAG, MPI_COMM_WORLD, &status);
        if (status.MPI_TAG == TAG_DONE)
        {
            done_count++;
            continue;
        }
        reports_received++;
        handle_report(cfg, &report_in, status.MPI_SOURCE, start, last_report_time,
                      last_congested_time, log, &stats, &recommendations);
    }
    detach_send_buffer();
    simulation_seconds = global_time() - start;

    gather_stats(&stats, base_rank, &total);
    write_summary(log, cfg, VERSION, simulation_seconds, &total, reports_received,
                  recommendations);
    append_csv(cfg, VERSION, simulation_seconds, &total);
    fclose(log);

    printf("[Base] Finished after %.3f s. Reports received: %lld. Log: %s\n",
           simulation_seconds, reports_received, cfg->log_path);
    fflush(stdout);
    free(last_congested_time);
    free(last_report_time);
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
